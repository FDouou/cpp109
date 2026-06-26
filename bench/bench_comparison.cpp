// bench_comparison.cpp — cpp109 vs spdlog vs quill 多线程吞吐量对比
//
// 架构差异：
//   cpp109: per-thread SPSC 无锁队列 + N 后台线程（每个队列 1 个独立后台线程 + 独立文件）
//   spdlog: MPMC 共享阻塞队列 + 1 后台线程
//   spdlog 2q: 两个独立 spdlog async logger，各使用独立 thread_pool（各 1 后台线程）+ 独立文件
//   quill:   per-thread SPSC 无锁队列 + 1 后台线程轮询所有队列
//
// 公平性原则：
//   所有库使用相同的 WARMUP_SEC=2, MEASURE_SEC=10, WORK_US=50
//   所有库都写文件（真实 I/O）
//   日志内容相同："m {count}" 格式
//   格式化复杂度相近：都带时间戳、级别、线程ID
//
// 编译要求：需要 cpp109、spdlog、quill 三个库的头文件可用
//   CMakeLists.txt 中 bench_comparison 已链接 109cpp、spdlog::spdlog、quill::quill

#include "log/log.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>

// ── quill v8.2.0 ───────────────────────────────────────────────────────
// 注意：log/log.hpp 定义了 LOG_INFO(fmt, ...) 宏（无 logger 参数），而 quill v8.x
// 的 LOG_INFO(logger, fmt, ...) 宏会与之冲突。为避免宏碰撞，本文件统一使用
// QUILL_LOG_INFO(logger, fmt, ...) 宏（带 QUILL_ 前缀的版本），绝不使用 LOG_INFO。
//
// 已知问题：quill v8.2.0 的默认队列类型 UnboundedBlocking 在 Windows x64 上
// 扩容到 2GB 时 capacity 翻倍为 2GB，_alloc_aligned(2*2GB=4GB) 触发
// STATUS_STACK_BUFFER_OVERRUN 崩溃（_aligned_malloc 检查失败导致 __fastfail）。
// 根因：代码使用 if (capacity > max_bounded_queue_size) 而非 >=，导致 2GB 时检查通过，
//      但 2GB capacity * 2 = 4GB 分配超限。
// 修复：使用 UnboundedUnlimited（永不阻塞，适合 benchmark）并增大初始容量至 4 MiB，
//       减少扩容频率。12 秒 benchmark 内每队列最大扩容至约 512 MiB（alloc 1 GiB），
//       远低于 2 GiB 崩溃阈值。
//
// quill v8.2.0 的正确 API（已根据实际源码确认）：
//   - Backend:   quill/Backend.h      → quill::Backend::start(), quill::Backend::stop()
//   - Frontend:  quill/Frontend.h     → Frontend::create_or_get_sink, create_or_get_logger
//   - LogMacros: quill/LogMacros.h    → QUILL_LOG_INFO(logger, fmt, ...)
//   - Logger:    quill/Logger.h       → quill::Logger* (没有 info() 成员函数，必须用宏)
//   - FileSink:  quill/sinks/FileSink.h → quill::FileSink
//
// 注：Backend::start() 使用 std::call_once，只能启动一次。因此在 main() 中一次性启动，
//    各次 bench_quill 调用使用唯一文件名，共享同一个后台线程。
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>

// ── 自定义 FrontendOptions：UnboundedUnlimited 队列（无上限扩容）─────
// 解决 Windows 上 UnboundedBlocking 扩容到 2GB 时触发 4GB _aligned_malloc 崩溃的问题。
// UnboundedUnlimited 不检查 2GB 上限，会持续扩容，适合短期 benchmark。
// 初始容量设为 4 MiB（比默认 128 KiB 大），减少扩容频率。
struct QuillBenchFrontendOptions
{
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedUnlimited;
    static constexpr uint32_t initial_queue_capacity = 4U * 1024U * 1024U; // 4 MiB
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr bool huge_pages_enabled = false;
};
using QuillBenchFrontend = quill::FrontendImpl<QuillBenchFrontendOptions>;
using QuillBenchLogger = quill::LoggerImpl<QuillBenchFrontendOptions>;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ── 平台相关的时间、RSS、CPU 辅助函数（同 bench.cpp、bench_spdlog.cpp）─────

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
    double now_us()
    {
        static LARGE_INTEGER freq;
        static int init = (QueryPerformanceFrequency(&freq), 0);
        (void)init;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (double)(t.QuadPart) * 1e6 / (double)freq.QuadPart;
    }
    size_t current_rss_mb()
    {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            return pmc.WorkingSetSize / 1024 / 1024;
        return 0;
    }
    double process_cpu_sec()
    {
        FILETIME ct, et, kt, ut;
        if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) return 0;
        ULARGE_INTEGER u; u.LowPart = ut.dwLowDateTime; u.HighPart = ut.dwHighDateTime;
        return u.QuadPart * 1e-7;
    }
#else
    #include <sys/resource.h>
    #include <sys/time.h>
    #include <unistd.h>
    double now_us()
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return tv.tv_sec * 1e6 + tv.tv_usec;
    }
    size_t current_rss_mb()
    {
        struct rusage u;
        getrusage(RUSAGE_SELF, &u);
        return u.ru_maxrss / 1024;
    }
    double process_cpu_sec()
    {
        struct rusage u;
        getrusage(RUSAGE_SELF, &u);
        return u.ru_utime.tv_sec + u.ru_utime.tv_usec * 1e-6
             + u.ru_stime.tv_sec + u.ru_stime.tv_usec * 1e-6;
    }
#endif

namespace {

// ── 基准参数 ──────────────────────────────────────────────────────────

constexpr int WARMUP_SEC   = 2;
constexpr int MEASURE_SEC  = 10;
constexpr int WORK_US      = 50;

// ── simulated_work ──────────────────────────────────────────────────

#ifdef _WIN32
void simulated_work()
{
    static LARGE_INTEGER freq;
    static int init = (QueryPerformanceFrequency(&freq), 0);
    (void)init;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    do { QueryPerformanceCounter(&now); }
    while ((now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart < WORK_US);
}
#else
void simulated_work()
{
    auto s = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - s).count() < WORK_US) {}
}
#endif

// ── count_lines ────────────────────────────────────────────────────

size_t count_lines(const char* filename)
{
    FILE* f = fopen(filename, "r");
    if (!f) return 0;
    size_t n = 0;
    char buf[65536];
    while (size_t r = fread(buf, 1, sizeof(buf), f))
        for (size_t i = 0; i < r; ++i)
            if (buf[i] == '\n') ++n;
    fclose(f);
    return n;
}

// ── BenchResult ────────────────────────────────────────────────────

struct LatencyStats { double p50, p90, p99; };

LatencyStats compute_latency(std::vector<double>& samples)
{
    if (samples.size() < 100) return {0, 0, 0};
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    return {
        samples[(size_t)(n * 0.50)],
        samples[(size_t)(n * 0.90)],
        samples[(size_t)(n * 0.99)]
    };
}

struct BenchResult
{
    std::string name;
    int    threads;
    uint64_t produced;
    uint64_t consumed;
    double   elapsed_sec;
    double   cpu_sec;
    size_t   rss_mb;
    LatencyStats lat;
    uint64_t rate() const { return (uint64_t)(produced / elapsed_sec); }
    double   loss_pct() const {
        return produced > 0 ? 100.0 * (double)(produced - consumed) / produced : 0;
    }
};

// ── tee_printf（同时写 stdout 和文件）──────────────────────────────────

static FILE* g_out = nullptr;

void tee_printf(const char* fmt, ...)
{
    va_list args1, args2;
    va_start(args1, fmt);
    va_copy(args2, args1);

    vprintf(fmt, args1);
    if (g_out) vfprintf(g_out, fmt, args2);

    va_end(args2);
    va_end(args1);
}

// ── 删除临时文件 ─────────────────────────────────────────────────────

void remove_files(const std::vector<std::string>& paths)
{
    for (const auto& p : paths)
        std::remove(p.c_str());
}

// ══════════════════════════════════════════════════════════════════════
//  bench_cpp109  —  N 前端 + N 后端（per-thread 独立 AsyncSink + 独立文件）
// ══════════════════════════════════════════════════════════════════════

BenchResult bench_cpp109(int n, bool with_work)
{
    size_t rss_before = current_rss_mb();

    std::vector<std::string> paths;
    std::vector<std::shared_ptr<cpp109::Logger>> loggers;

    // 创建 n 个独立 logger，每个有独立的 AsyncSink + FileSink + 文件
    for (int i = 0; i < n; ++i) {
        std::string p = "__cmp_cpp109_" + std::to_string(i) + ".log";
        paths.push_back(p);
        std::remove(p.c_str());
        // 构造 FileSink，设置不自动 flush（-1），再包装为 AsyncSink
        auto inner = std::make_shared<cpp109::FileSink>(p, true);
        inner->set_flush_interval(-1);
        auto sink = std::make_shared<cpp109::AsyncSink<>>(std::move(inner));
        auto l = cpp109::get_logger("__cmp_cpp109_" + std::to_string(i));
        l->clear_sinks();
        l->set_level(cpp109::LogLevel::TRACE);
        l->add_sink(sink);
        loggers.push_back(l);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        auto& l = loggers[id];
        uint64_t local = 0;
        while (warmup) {
            l->info("w{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        local = 0;  // 重置 local，只计量测阶段
        while (running) {
            l->info("m{} {}", id, local);
            if (with_work) simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    total.store(0);

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();

    // 刷新所有异步 sink
    for (auto& l : loggers)
        l->flush();

    uint64_t actual = 0;
    for (const auto& p : paths)
        actual += count_lines(p.c_str());

    remove_files(paths);
    loggers.clear();
    cpp109::Registry::instance().remove_all();

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    std::string tag = std::to_string(n) + "t cpp109 N+N" + (with_work ? " + work" : "");
    return {tag, n, total.load(), actual,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}

// ══════════════════════════════════════════════════════════════════════
//  bench_spdlog_async  —  N 前端 + 1 后端（共享 async 队列 + 单文件）
// ══════════════════════════════════════════════════════════════════════

BenchResult bench_spdlog_async(int n, bool with_work)
{
    size_t rss_before = current_rss_mb();

    const std::string path = "__cmp_spdlog.log";
    std::remove(path.c_str());

    // 初始化全局线程池（1 个后台线程）
    spdlog::init_thread_pool(65536, 1);

    // 创建 1 个 async logger
    auto logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("_cmp_spd", path, true);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    logger->set_level(spdlog::level::trace);
    // 注意：不设置 flush_on，避免每条日志都 fsync 破坏 async 性能。
    // spdlog async 的后台线程会自动处理队列中的消息并写入文件。
    // 测试结束后通过 logger->flush() + 短暂等待保证数据落盘。

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int /*id*/) {
        uint64_t local = 0;
        while (warmup) {
            logger->info("w {}", local);
            if (with_work) simulated_work();
            ++local;
        }
        local = 0;
        while (running) {
            logger->info("m {}", local);
            if (with_work) simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    total.store(0);

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();

    logger->flush();
    // 等待后台线程将队列中剩余消息写入磁盘
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    spdlog::drop_all();  // 从 registry 移除所有 logger

    uint64_t actual = count_lines(path.c_str());
    std::remove(path.c_str());

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    std::string tag = std::to_string(n) + "t spdlog N+1" + (with_work ? " + work" : "");
    return {tag, n, total.load(), actual,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}

// ══════════════════════════════════════════════════════════════════════
//  bench_spdlog_async_2q  —  N 前端 + 2 后端（两个独立 async logger，各 n/2 前端）
// ══════════════════════════════════════════════════════════════════════
//  实现方案：
//    手动创建 2 个独立 thread_pool（各 1 后台线程），然后分别创建 async_logger。
//    spdlog 默认的 async logger 共享一个全局线程池。要获得独立的后台线程，
//    需要绕过 create_async 工厂，直接构造 async_logger 并传入自定义 thread_pool。
//    async_logger 构造函数接受 std::weak_ptr<details::thread_pool>。
//    我们需要保持 thread_pool 的 shared_ptr 存活（async_logger 只持有 weak_ptr）。
//
//  注意：details::thread_pool 是 spdlog 的 internal API，不是公开接口。
//  在 spdlog 编译为静态库时不可用，但 header-only 模式下可用。
//  如果编译失败，备选方案是使用两个独立的 sync logger（各自内部加锁），
//  但这样就不是 async 了，对比不公平。另一个备选是使用两个 spdlog 实例
//  （但 spdlog 是单例设计，不能多实例）。

BenchResult bench_spdlog_async_2q(int n, bool with_work)
{
    size_t rss_before = current_rss_mb();

    const std::string path1 = "__cmp_spd2q_0.log";
    const std::string path2 = "__cmp_spd2q_1.log";
    std::remove(path1.c_str());
    std::remove(path2.c_str());

    // 创建两个独立的线程池，各 1 个后台线程
    auto tp1 = std::make_shared<spdlog::details::thread_pool>(65536, 1);
    auto tp2 = std::make_shared<spdlog::details::thread_pool>(65536, 1);

    // 创建两个文件 sink
    auto sink1 = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path1, true);
    auto sink2 = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path2, true);

    // 创建两个 async logger，各使用独立的 thread_pool
    auto logger1 = std::make_shared<spdlog::async_logger>(
        "_cmp_spd2q_0", sink1, std::weak_ptr<spdlog::details::thread_pool>(tp1));
    auto logger2 = std::make_shared<spdlog::async_logger>(
        "_cmp_spd2q_1", sink2, std::weak_ptr<spdlog::details::thread_pool>(tp2));

    logger1->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    logger2->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    logger1->set_level(spdlog::level::trace);
    logger2->set_level(spdlog::level::trace);

    // 注册到 registry（方便管理，但非必须）
    // 注意：spdlog 的 registry 对 async_logger 的线程池没有所有权，
    // 线程池由我们手动管理的 shared_ptr 保持存活。
    {
        auto& reg = spdlog::details::registry::instance();
        reg.register_logger(logger1);
        reg.register_logger(logger2);
    }

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int id) {
        auto& l = (id < n / 2) ? logger1 : logger2;
        uint64_t local = 0;
        while (warmup) {
            l->info("w {}", local);
            if (with_work) simulated_work();
            ++local;
        }
        local = 0;
        while (running) {
            l->info("m {}", local);
            if (with_work) simulated_work();
            ++local;
        }
        total += local;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    total.store(0);

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();

    logger1->flush();
    logger2->flush();

    // 从 registry 中注销
    spdlog::drop("_cmp_spd2q_0");
    spdlog::drop("_cmp_spd2q_1");

    // 保持 tp1, tp2 存活直到后台处理完所有消息
    // thread_pool 的析构会 drain 队列并 join 线程
    logger1.reset();
    logger2.reset();
    // 现在可以安全销毁线程池
    tp1.reset();
    tp2.reset();

    uint64_t actual = count_lines(path1.c_str()) + count_lines(path2.c_str());
    std::remove(path1.c_str());
    std::remove(path2.c_str());

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    std::string tag = std::to_string(n) + "t spdlog N+2" + (with_work ? " + work" : "");
    return {tag, n, total.load(), actual,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}

// ══════════════════════════════════════════════════════════════════════
//  bench_quill  —  N 前端 + 1 后端（per-thread 队列 + 1 后台轮询）
// ══════════════════════════════════════════════════════════════════════
//  quill 自动为每个调用 QUILL_LOG_INFO 的线程创建 SPSC 队列，1 个后台线程轮询所有队列。
//
//  注意：Backend::start() 使用 std::call_once，只能在 main() 中启动一次。
//  因此本函数不启动/停止后台线程，而是假设其在 main() 中已启动并保持运行。
//  每次调用使用唯一文件名（静态计数器），以避免多轮 benchmark 之间的文件冲突。

#ifndef SKIP_QUILL
BenchResult bench_quill(int n, bool with_work)
{
    size_t rss_before = current_rss_mb();

    // 每次调用使用唯一文件名（后端线程跨所有调用共享）
    static std::atomic<int> s_quill_run{0};
    int run_id = s_quill_run++;
    std::string path = "__cmp_quill_" + std::to_string(run_id) + ".log";
    std::remove(path.c_str());

    // ── 创建 FileSink 和 Logger ─────────────────────────────────────
    // 使用自定义 FrontendOptions 的 QuillBenchFrontend 替代默认 quill::Frontend，
    // 以使用 UnboundedUnlimited 队列（无上限扩容，永不阻塞），
    // 避免 Windows 上 UnboundedBlocking 扩容到 2GB 时分配 4GB 对齐内存崩溃。
    //
    // SinkManager 对 FileSink 特殊处理：sink_name 同时作为 FileSink 的 filename 参数。
    // 使用唯一 sink_name，避免跨调用重用旧的 sink。
    auto file_sink = QuillBenchFrontend::create_or_get_sink<quill::FileSink>(path);
    std::string logger_name = "cmp_quill_" + std::to_string(run_id);
    QuillBenchLogger* logger = QuillBenchFrontend::create_or_get_logger(logger_name, std::move(file_sink));

    // 注意：为避免 LOG_INFO 宏与 cpp109 冲突，以下统一使用 QUILL_LOG_INFO 宏。
    // quill v8.x 的 Logger 没有 info()/debug() 等成员函数，必须使用 QUILL_LOG_INFO 宏。

    std::atomic<uint64_t> total{0};
    std::atomic<bool> warmup{true};
    std::atomic<bool> running{true};

    auto worker = [&](int /*id*/) {
        uint64_t local = 0;
        while (warmup) {
            QUILL_LOG_INFO(logger, "w {}", local);
            if (with_work) simulated_work();
            ++local;
        }
        local = 0;
        while (running) {
            QUILL_LOG_INFO(logger, "m {}", local);
            if (with_work) simulated_work();
            ++local;
        }
        // Use relaxed atomic so total load is non-zero even if total races with benchmark end
        total.fetch_add(local, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(WARMUP_SEC));
    warmup = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    total.store(0, std::memory_order_relaxed);

    double cpu_before = process_cpu_sec();
    size_t rss_peak = current_rss_mb();

    std::this_thread::sleep_for(std::chrono::seconds(MEASURE_SEC));
    double cpu_elapsed = process_cpu_sec() - cpu_before;
    running = false;

    for (auto& t : threads) t.join();

    // ── flush 确保所有 pending 消息写入文件 ──────────────────────────
    // quill::Logger::flush_log() 阻塞直到 backend 处理完当前队列所有消息
    logger->flush_log();

    uint64_t actual = count_lines(path.c_str());
    std::remove(path.c_str());

    size_t rss_delta = (rss_peak > rss_before) ? (rss_peak - rss_before) : 0;
    std::string tag = std::to_string(n) + "t quill N+1" + (with_work ? " + work" : "");
    return {tag, n, total.load(std::memory_order_relaxed), actual,
            (double)MEASURE_SEC, cpu_elapsed, rss_delta, {0, 0, 0}};
}
#endif // SKIP_QUILL

// ══════════════════════════════════════════════════════════════════════
//  打印表头
// ══════════════════════════════════════════════════════════════════════

void print_comparison_header()
{
    tee_printf("\n");
    tee_printf("================================================================================\n");
    tee_printf("  Multi-thread throughput comparison: cpp109 vs spdlog vs quill\n");
    tee_printf("  Warmup: %ds   Measure: %ds   Simulated work: %dus\n",
           WARMUP_SEC, MEASURE_SEC, WORK_US);
    tee_printf("================================================================================\n");
}

void print_table_header()
{
    tee_printf("%-7s %-18s %10s %10s %10s\n",
           "Threads", "Scenario", "Produced", "Consumed", "msg/s");
    tee_printf("%s\n", std::string(57, '-').c_str());
}

void print_table_row(const BenchResult& r)
{
    tee_printf("%-7d %-18s %10llu %10llu %10llu\n",
           r.threads, r.name.c_str(),
           (unsigned long long)r.produced,
           (unsigned long long)r.consumed,
           (unsigned long long)r.rate());
}

void print_compact_table_header()
{
    tee_printf("\n%-8s", "Threads");
    tee_printf(" %15s %15s %15s %15s",
           "cpp109 (N+N)", "spdlog (N+1)", "spdlog (N+2)", "quill (N+1)");
    tee_printf("\n%-8s", "-------");
    tee_printf(" %15s %15s %15s %15s",
           "-------------", "-------------", "-------------", "-------------");
    tee_printf("\n");
}

void print_compact_table_row(int n,
                              uint64_t cpp109_rate,
                              uint64_t spd_rate,
                              uint64_t spd2q_rate,
                              uint64_t quill_rate)
{
    auto fmt_rate = [](uint64_t r) -> std::string {
        if (r == 0) return "     N/A     ";
        char buf[64];
        if (r >= 1000000)
            std::snprintf(buf, sizeof(buf), "%8llu M", (unsigned long long)(r / 1000000));
        else if (r >= 1000)
            std::snprintf(buf, sizeof(buf), "%8llu K", (unsigned long long)(r / 1000));
        else
            std::snprintf(buf, sizeof(buf), "%8llu   ", (unsigned long long)r);
        return buf;
    };

    tee_printf("%-8d %15s %15s %15s %15s\n",
           n,
           fmt_rate(cpp109_rate).c_str(),
           fmt_rate(spd_rate).c_str(),
           fmt_rate(spd2q_rate).c_str(),
           fmt_rate(quill_rate).c_str());
}

} // anonymous namespace

// ══════════════════════════════════════════════════════════════════════
//  main
// ══════════════════════════════════════════════════════════════════════

int main()
{
    g_out = fopen("bench/bench_comparison.txt", "a");
    if (!g_out) g_out = fopen("../bench/bench_comparison.txt", "a");

    // ── 一次性启动 quill 后台线程 ──────────────────────────────────
    // Backend::start() 使用 std::call_once，只能调用一次。
    // 所有 bench_quill 调用共享这个后台线程。
#ifndef SKIP_QUILL
    quill::Backend::start();
#endif

    print_comparison_header();

    // ── 测试线程数 ──────────────────────────────────────────────────
    const std::vector<int> thread_counts = {4, 8, 16, 32};

    // ── 无模拟工作（纯日志吞吐）─────────────────────────────────────
    tee_printf("\n>>> Without simulated work (pure logging throughput) <<<\n");
    print_compact_table_header();

    for (int n : thread_counts) {
        auto r1 = bench_cpp109(n, false);
        cpp109::Registry::instance().remove_all();

        auto r2 = bench_spdlog_async(n, false);
        // spdlog 的 registry 已被 drop_all 清理

        auto r3 = bench_spdlog_async_2q(n, false);

#ifndef SKIP_QUILL
        auto r4 = bench_quill(n, false);
#else
        BenchResult r4{"skip quill", n, 0, 0, 1.0, 0, 0, {0,0,0}};
#endif

        print_compact_table_row(n, r1.rate(), r2.rate(), r3.rate(), r4.rate());
    }

    // ── 有模拟工作（混合负载）─────────────────────────────────────────
    tee_printf("\n>>> With simulated work (%dus per log) <<<\n", WORK_US);
    print_compact_table_header();

    for (int n : thread_counts) {
        auto r1 = bench_cpp109(n, true);
        cpp109::Registry::instance().remove_all();

        auto r2 = bench_spdlog_async(n, true);

        auto r3 = bench_spdlog_async_2q(n, true);

#ifndef SKIP_QUILL
        auto r4 = bench_quill(n, true);
#else
        BenchResult r4{"skip quill", n, 0, 0, 1.0, 0, 0, {0,0,0}};
#endif

        print_compact_table_row(n, r1.rate(), r2.rate(), r3.rate(), r4.rate());
    }

    // ── 详细结果（含 Produced/Consumed）──────────────────────────────
    tee_printf("\n\n>>> Detailed results (with simulated work) <<<\n");
    print_table_header();

    for (int n : thread_counts) {
        auto r1 = bench_cpp109(n, true);
        cpp109::Registry::instance().remove_all();
        print_table_row(r1);

        auto r2 = bench_spdlog_async(n, true);
        print_table_row(r2);

        auto r3 = bench_spdlog_async_2q(n, true);
        print_table_row(r3);

#ifndef SKIP_QUILL
        auto r4 = bench_quill(n, true);
        print_table_row(r4);
#endif
    }

    tee_printf("\n");

    // ── 停止 quill 后台线程 ────────────────────────────────────────
#ifndef SKIP_QUILL
    quill::Backend::stop();
#endif

    if (g_out) fclose(g_out);
    return 0;
}
