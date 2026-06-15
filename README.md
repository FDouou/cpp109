# cpp109

C++20 header-only日志库

## 编译要求

- C++20 编译器
- CMake 3.20+

## 快速开始

### CMake 集成

```cmake
add_subdirectory(path/to/cpp109)
target_link_libraries(your_target cpp109)
```

### Header-only 集成

纯头文件库，无需编译，直接将 `include/log/` 拷贝到项目中：

```
your_project/
├── include/
│   └── log/          ← 拷贝整个目录过来
└── main.cpp
```

```cpp
#include "log/log.hpp"
```

编译时确保 include 路径正确：

```bash
g++ -std=c++20 -I./include main.cpp
```

### 基础用法

```cpp
#include "log/log.hpp"

int main() {
    auto logger = cpp109::get_logger("app");
    logger->add_sink(std::make_shared<cpp109::ConsoleSink>());

    logger->info("hello {}", "world");
    logger->warn("something might be wrong, code={}", 500);
    logger->error("something went wrong");
}
```

### 便捷宏

针对默认 logger 提供一组宏，无需手动调用 `get_logger`：

```cpp
// 基本级别宏
LOG_TRACE("trace level message");
LOG_DEBUG("debug value = {}", x);
LOG_INFO("hello {}", "world");
LOG_WARN("something might be wrong, code={}", 500);
LOG_ERROR("something went wrong");
LOG_FATAL("fatal error, aborting...");  // 触发 std::abort()

// 条件宏：condition 为 true 时才输出
LOG_INFO_IF(x > 100, "x is large: {}", x);
LOG_WARN_IF(ret != 0, "bad return code: {}", ret);
LOG_ERROR_IF(!file.is_open(), "file not found: {}", path);

// 限频宏：每隔 N 次调用输出一次
LOG_INFO_EVERY_N(100, "progress: {} items processed", count);

// 前 N 次宏：仅前 N 次调用输出
LOG_INFO_FIRST_N(10, "startup phase: {}", step);
```

> 这些宏最终调用 `cpp109::Registry::instance().default_logger()`，默认级别为 INFO，首次调用时自动附加 `ConsoleSink`。

## Sink 列表

| Sink               | 说明                                              |
| ------------------ | ----------------------------------------------- |
| `ConsoleSink`      | 控制台输出，支持彩色日志（按级别着色），可输出到 stdout 或 stderr        |
| `FileSink`         | 文件输出，支持覆盖/追加模式，可配置自动 flush 间隔                   |
| `RotatingFileSink` | 按大小滚动的文件输出，达到指定大小后自动滚动，可配置保留文件数                 |
| `DailyFileSink`    | 按时间滚动的文件输出，支持按分钟/小时/天创建新文件，文件名支持时间占位符           |
| `CallbackSink`     | 自定义回调，每条日志触发回调函数，适用于发送到网络、数据库或第三方监控             |
| `AsyncSink`        | 异步包装器，将任意 Sink 包装为后台线程写入，通过无锁 RingBuffer 解耦 I/O |

### Sink 示例

```cpp
// 控制台输出（带颜色）
auto console = std::make_shared<cpp109::ConsoleSink>();

// 文件输出（追加模式）
auto file = std::make_shared<cpp109::FileSink>("app.log", false);

// 按大小滚动（10MB，保留 5 个文件）
auto rotating = std::make_shared<cpp109::RotatingFileSink>("app.log", 10, 5);

// 按天滚动（文件名含日期）
auto daily = std::make_shared<cpp109::DailyFileSink>("logs/%Y%m%d.log");

// 自定义回调
auto callback = std::make_shared<cpp109::CallbackSink>(
    [](const std::string& msg, const cpp109::LogEvent& event) {
        // 发送到远程服务器、写入数据库等
    }
);

logger->add_sink(console);
logger->add_sink(file);
```

> 异步 Sink 详见下方 [异步日志](#异步日志) 章节。

## 异步日志

cpp109 不依赖全局线程池，而是采用 **per-sink 独立线程模型**——每个 `AsyncSink`
独占一个后台线程和 SPSC 无锁环形缓冲区（RingBuffer），生产者写入零竞争。

```
生产者线程                          AsyncSink                       文件
────────────────────────────────────────────────────────────────────────
worker_1 ───→ RingBuffer_1 ───→ bg_thread_1 ───→ file_1.log
worker_2 ───→ RingBuffer_2 ───→ bg_thread_2 ───→ file_2.log
worker_3 ───→ RingBuffer_3 ───→ bg_thread_3 ───→ file_3.log
   ...
worker_N ───→ RingBuffer_N ───→ bg_thread_N ───→ file_N.log
```

**核心优势**：

- **无锁入队**：SPSC RingBuffer 无需 CAS 重试，入队仅两次原子操作
- **无 Head-of-line 阻塞**：一个 worker 的磁盘写入阻塞，不影响其他 worker
- **低延迟唤醒**：数据到达立即通过条件变量通知 consumer 线程，空闲时零 CPU 占用
- **格式化分工**：消息体的 `std::format` 在 worker 线程完成，pattern formatting 由 consumer 线程承担，避免串行瓶颈

> **使用建议**：
> - AsyncSink 的价值在于 worker 不被 I/O 阻塞——入队 ~2µs 即返回，consumer 后台写盘
> - FileSink 等 I/O 密集型 Sink 的 consumer 线程大部分时间阻塞在磁盘写入，实际 CPU 占用极低，
>   线程数可以更宽松
> - ConsoleSink / NullSink 等 CPU 密集型 Sink 的 consumer 持续占用 CPU，总线程数不宜超物理核心
> - 不在意 worker 阻塞的场景，sync Sink（内部 `std::lock_guard`）总吞吐更高
> - 多线程写同一个文件用 sync Sink，不要用 AsyncSink（SPSC 队列限定单生产者）

### 构造方式

```cpp
// 方式 1：工厂模式从零构造（内部创建 Sink 再包装）
auto async_console = cpp109::make_async_sink<cpp109::ConsoleSink>();

// 方式 2：包装已有的 Sink
auto file = std::make_shared<cpp109::FileSink>("app.log");
auto async_file = std::make_shared<cpp109::AsyncSink<>>(file);
```

### 多线程高并发示例

```cpp
// AsyncSink 数量建议参考 CPU 物理核心数
for (int i = 0; i < 8; ++i) {
    auto sink = cpp109::make_async_sink<cpp109::FileSink>(
        "thread_" + std::to_string(i) + ".log", true);
    auto logger = cpp109::get_logger("worker_" + std::to_string(i));
    logger->add_sink(sink);
}

// 每个线程绑定独立的 AsyncSink + 文件，互不阻塞
```

## Logger 层级

logger 名称中的 `.` 会建立父子关系，子 logger 的日志默认向上 propagate：

```cpp
auto parent = cpp109::get_logger("app");
auto child  = cpp109::get_logger("app.module");

child->set_propagate(true);   // 子 logger 日志同时传递给 parent（默认开启）
child->set_propagate(false);  // 关闭传递，仅写入自己的 sink
```

## 格式化

通过 `set_pattern` 配置输出格式，支持以下占位符：

| 占位符         | 说明                          |
| ----------- | --------------------------- |
| `%Y %m %d`  | 年 / 月 / 日                   |
| `%H %M %S`  | 时 / 分 / 秒                   |
| `%f` / `%F` | 毫秒（3位）/ 微秒（6位）              |
| `%l` / `%L` | 级别小写 (trace) / 级别大写 (TRACE) |
| `%n`        | logger 名称                   |
| `%t`        | 线程 ID                       |
| `%g` / `%G` | 文件名 basename / 完整路径         |
| `%#`        | 行号                          |
| `%!`        | 函数名                         |
| `%v`        | 日志消息正文                      |
| `%%`        | 字面量百分号                      |

默认格式：

```
"[%Y-%m-%d %H:%M:%S.%f] [%L] [%t] [%g:%#] %v"
```

```cpp
// 全局修改默认格式
cpp109::Registry::instance().set_pattern("[%H:%M:%S] [%l] %v");

// 单个 sink 修改格式
sink->set_pattern("[%L] %v");
```

## Config API

通过 `Config` 批量配置 sink 和 logger，替代逐一手动创建：

```cpp
cpp109::Config cfg;

cfg.add_sink("console")
   .set_class<cpp109::ConsoleSink>()
   .set_level(cpp109::LogLevel::DEBUG)
   .set_formatter("simple");

cfg.add_logger("admin")
   .set_sinks({"console"})
   .set_level(cpp109::LogLevel::DEBUG);

cfg.apply();
```

任意 sink 可通过 `set_async()`包装：

```cpp
cfg.add_sink("file")
   .set_class<cpp109::RotatingFileSink>()
   .set_property("filename", "logs/app.log")
   .set_property("max_size_mb", "10")
   .set_property("max_files", "5")
   .set_async()                                   // 默认 queue_size=8192, overflow_policy=block
   .set_queue_size(8192)                          
   .set_overflow_policy("block");                 
```



## 构建

```bash
cmake -B build -S .
cmake --build build
```

### 运行测试

```bash
ctest --test-dir build
```

## 性能

以下数据在 Windows 11 / MinGW GCC 14 / SSD 环境测得，CPU 为 Intel i9-12900HX（16 个物理核心）。
预热 3s，测量 10s，单条日志 ~100 字节，格式化占位符使用默认配置。

### 单线程基线

| 模式               | 吞吐量          | P50 延迟 | P99 延迟 |
| ----------------- | ------------ | ------ | ------ |
| sync, no work     | 1,748,000 msg/s | <1µs   | 5µs    |
| async, no work    | 555,000 msg/s | 2µs    | 4µs    |
| sync + 10µs work  | 90,000 msg/s  | <1µs    | 6µs   |
| async + 10µs work | 85,000 msg/s  | 2µs    | 6µs    |

> 不带 work 时 sync 更快（单线程没有 I/O 可重叠，AsyncSink 的入队和事件复制是纯开销）。
> 带 10µs 工作负载后两者持平，work 本身成为耗时主体。

### 多线程吞吐（per-thread 独立 AsyncSink + 独立文件）

| 线程数 | 总吞吐量            | 扩展比 |
| --- | --------------- | ----- |
| 4t  | 1,154,000 msg/s | 1.00× |
| 8t  | 2,097,000 msg/s | 1.82× |
| 16t | 3,332,000 msg/s | 2.89× |
| 32t | 3,659,000 msg/s | 3.17× |
| 64t | 3,936,000 msg/s | 3.41× |

### 多线程 NullSink（排除磁盘 I/O，仅保留格式化与队列开销）

| 模式 | 4t | 8t | 16t | 32t | 64t |
|------|----|----|-----|-----|-----|
| async+null | 10,845,000 | 7,494,000 | 5,625,000  | 5,517,000  | 5,682,000  |
| sync+null | 10,218,000 | 9,711,000 | 5,811,000  | 5,276,000  | 5,386,000  |

> `t` = worker 线程数。async 行另有等量 consumer 后台线程（如 4t async = 4 worker + 4 consumer = 8 线程），sync 行无额外线程。两行线程总数不同，表格为相同 worker 数下的对照。

