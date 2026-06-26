// batch_test.cpp — 批量提交压力测试（单线程，避免多线程栈问题）
#include "log/log.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#define BT_ASSERT(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while(0)

// 每个唯�?的格式字符串有�?个独立的 static const TinyMeta（模拟真实场景中每�?日志调用点�??
static const cpp109::TinyMeta _meta_large_batch{
    "bt.cpp", 0, "main", "large batch", nullptr
};
static const cpp109::TinyMeta _meta_hello{
    "bt.cpp", 10, "main", "hello {}",
    &cpp109::detail::decode_and_format<int>
};
static const cpp109::TinyMeta _meta_multi{
    "bt.cpp", 0, "main", "multi {}",
    &cpp109::detail::decode_and_format<int>
};
static const cpp109::TinyMeta _meta_pressure{
    "bt.cpp", 0, "main", "pressure {}",
    &cpp109::detail::decode_and_format<int>
};
static const cpp109::TinyMeta _meta_mixed{
    "bt.cpp", 0, "main", "mixed msg", nullptr
};
static const cpp109::TinyMeta _meta_single_after{
    "bt.cpp", 200, "main", "single after batch {}",
    &cpp109::detail::decode_and_format<int>
};

// 构建一条带参数的记录：在 ptr 处写入 TinyHeader + 编码后的参数 a1
static std::size_t enc1(std::byte* ptr, const cpp109::TinyMeta* meta, uint64_t tid, int a1) {
    uint32_t as = static_cast<uint32_t>(cpp109::detail::compute_encoded_size(a1));
    cpp109::TinyHeader hdr{};
    hdr.timestamp_tsc = cpp109::rdtsc_ns();
    hdr.meta          = meta;
    hdr.thread_id     = tid;
    hdr.args_size     = as;
    hdr.level         = static_cast<std::uint8_t>(cpp109::LogLevel::INFO);
    hdr.flags         = 0;
    std::memcpy(ptr, &hdr, sizeof(hdr));
    cpp109::detail::encode_args(ptr + sizeof(hdr), a1);
    return sizeof(hdr) + as;
}

// 构建一条无参记录
static std::size_t enc0(std::byte* ptr, const cpp109::TinyMeta* meta, uint64_t tid) {
    cpp109::TinyHeader hdr{};
    hdr.timestamp_tsc = cpp109::rdtsc_ns();
    hdr.meta          = meta;
    hdr.thread_id     = tid;
    hdr.args_size     = 0;
    hdr.level         = static_cast<std::uint8_t>(cpp109::LogLevel::INFO);
    hdr.flags         = 0;
    std::memcpy(ptr, &hdr, sizeof(hdr));
    return sizeof(hdr);
}

int main() {
    int failures = 0;
    const char* path = "__batch_test.log";
    std::remove(path);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    auto sink = cpp109::make_async_sink<cpp109::FileSink>(path, true);
    auto log = cpp109::get_logger("batch_test");
    log->clear_sinks(); log->set_level(cpp109::LogLevel::TRACE); log->add_sink(sink);
    auto* ab = dynamic_cast<cpp109::AsyncSinkBase*>(sink.get());
    BT_ASSERT(ab);

    // Test 1
    fprintf(stdout, "T1 empty...\n"); ab->log_encoded_batch(nullptr, 0); fprintf(stdout, "  OK\n");

    // Test 2: large batch when ring empty
    fprintf(stdout, "T2 large...\n");
    {
        constexpr std::size_t TSZ = 900 * 1024;
        constexpr std::size_t RS = sizeof(cpp109::TinyHeader);
        constexpr int N = static_cast<int>(TSZ / RS);
        std::vector<std::byte> buf(static_cast<std::size_t>(N) * RS);
        for (int i = 0; i < N; ++i) enc0(buf.data() + static_cast<std::size_t>(i) * RS, &_meta_large_batch, 555);
        ab->log_encoded_batch(buf.data(), buf.size());
    }
    fprintf(stdout, "  OK\n");

    // Test 3: one msg
    fprintf(stdout, "T3 single...\n");
    {
        std::byte buf[256];
        auto sz = enc1(buf, &_meta_hello, 1, 42);
        ab->log_encoded_batch(buf, sz);
    }
    fprintf(stdout, "  OK\n");

    // Test 4: 100 records single batch
    fprintf(stdout, "T4 100x...\n");
    {
        constexpr int N = 100;
        constexpr std::size_t RS = sizeof(cpp109::TinyHeader) + sizeof(int);
        std::vector<std::byte> buf(static_cast<std::size_t>(N) * RS);
        for (int i = 0; i < N; ++i) enc1(buf.data() + static_cast<std::size_t>(i) * RS, &_meta_multi, 1, i);
        ab->log_encoded_batch(buf.data(), buf.size());
    }
    fprintf(stdout, "  OK\n");

    // Test 5: sequential batches (simulate multi-thread)
    fprintf(stdout, "T5 seq batches...\n");
    {
        constexpr int TOTAL = 5000, BATCH = 100;
        constexpr std::size_t RS = sizeof(cpp109::TinyHeader) + sizeof(int);
        std::vector<std::byte> buf(static_cast<std::size_t>(BATCH) * RS);
        for (int i = 0; i < TOTAL; i += BATCH) {
            int cnt = std::min(BATCH, TOTAL - i); if (cnt == 0) break;
            for (int j = 0; j < cnt; ++j) enc1(buf.data() + static_cast<std::size_t>(j) * RS, &_meta_pressure, 999, i + j);
            ab->log_encoded_batch(buf.data(), static_cast<std::size_t>(cnt) * RS);
        }
    }
    fprintf(stdout, "  OK\n");

    // Test 6: mixed
    fprintf(stdout, "T6 mixed...\n");
    {
        constexpr int N = 20;
        constexpr std::size_t RS = sizeof(cpp109::TinyHeader);
        std::vector<std::byte> buf(static_cast<std::size_t>(N) * RS);
        for (int i = 0; i < N; ++i) enc0(buf.data() + static_cast<std::size_t>(i) * RS, &_meta_mixed, 777);
        ab->log_encoded_batch(buf.data(), buf.size());
        for (int i = 0; i < 10; ++i) {
            uint32_t as = static_cast<uint32_t>(cpp109::detail::compute_encoded_size(i));
            std::byte* eb = cpp109::detail::get_encode_buffer(as);
            cpp109::detail::encode_args(eb, i);
            ab->log_encoded(&_meta_single_after,
                cpp109::LogLevel::INFO, 777, cpp109::rdtsc_ns(),
                eb, as);
        }
    }
    fprintf(stdout, "  OK\n");

    // Cleanup
    sink->stop(); log->clear_sinks(); cpp109::Registry::instance().remove_all();
    log.reset(); sink.reset();

    // Verify
    fprintf(stdout, "--- Verify ---\n");
    std::ifstream ifs(path); BT_ASSERT(ifs.is_open());
    int tl = 0, lc = 0, hc = 0; std::string line;
    while (std::getline(ifs, line)) { tl++; if (line.find("large batch") != std::string::npos) lc++; if (line.find("hello") != std::string::npos) hc++; }
    ifs.close();
    int el = static_cast<int>(900 * 1024 / sizeof(cpp109::TinyHeader));
    fprintf(stdout, "  total=%d large=%d(exp~%d) hello=%d\n", tl, lc, el, hc);
    BT_ASSERT(lc >= el - 5); BT_ASSERT(hc >= 1);
    std::remove(path);
    fprintf(stdout, "=== %s ===\n", failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
