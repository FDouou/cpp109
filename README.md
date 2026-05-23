# cpp109

(未完成)
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

- **线性扩展**：每增加一个写入线程，吞吐量线性增长，不会落入单消费者瓶颈
- **零锁竞争**：SPSC RingBuffer 无需 CAS 重试，入队仅两次原子操作
- **无头阻塞**：一个 worker 的磁盘写入阻塞，不影响其他 worker
- **条件变量唤醒**：数据到达立即通知 worker，空闲时零 CPU 占用

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
// 每个线程绑定独立的 AsyncSink 和文件，达到最大吞吐
for (int i = 0; i < 16; ++i) {
    auto sink = cpp109::make_async_sink<cpp109::FileSink>(
        "thread_" + std::to_string(i) + ".log", true);
    auto logger = cpp109::get_logger("worker_" + std::to_string(i));
    logger->add_sink(sink);
}

// 16 个线程同时高频写日志，各自独立队列和文件，互不阻塞
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

以下数据在 Windows 11 / MinGW GCC 14 / SSD 环境下测得（预热 3s，测量 10s，单条日志 \~100 字节）。

### 单线程（带 10µs 模拟工作负载）

| 模式    | 吞吐量          | P50 延迟 | P99 延迟 |
| ----- | ------------ | ------ | ------ |
| sync  | 79,000 msg/s | 2µs    | 12µs   |
| async | 81,000 msg/s | 1µs    | 7µs    |

> 10µs 工作负载下异步与同步持平，实际业务代码中间隔更长，两者体验无差异。

### 多线程 async（per-thread 独立 AsyncSink）

| 线程数 | 总吞吐量            | 扩展比   | 说明            |
| --- | --------------- | ----- | ------------- |
| 4t  | 803,000 msg/s   | 1.00x | <br />        |
| 8t  | 1,431,000 msg/s | 1.78x | <br />        |
| 16t | 2,259,000 msg/s | 2.81x | <br />        |
| 32t | 2,645,000 msg/s | 3.29x | <br />        |
| 64t | 2,665,000 msg/s | 3.32x | SSD 磁盘 I/O 瓶颈 |

> 32t 之后受物理磁盘写入上限约束

