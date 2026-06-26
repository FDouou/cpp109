# cpp109

C++20 header-only 日志库

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
| `AsyncSink`        | 异步包装器，将任意 Sink 包装为后台线程写入，通过 SPSC 环形缓冲区解耦 I/O |

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
独占一个后台线程和 SPSC 无锁环形缓冲区（RingBuffer），生产者写入无竞争。

```
生产者线程                          AsyncSink                       文件
────────────────────────────────────────────────────────────────────────
worker_1 ───→ RingBuffer_1 ───→ bg_thread_1 ───→ file_1.log
worker_2 ───→ RingBuffer_2 ───→ bg_thread_2 ───→ file_2.log
worker_3 ───→ RingBuffer_3 ───→ bg_thread_3 ───→ file_3.log
   ...
worker_N ───→ RingBuffer_N ───→ bg_thread_N ───→ file_N.log
```

设计：

- **SPSC 环形缓冲区入队**：入队仅两次原子操作（写位置预留 + 提交），无需 CAS 重试
- **条件变量通知**：数据到达后通知 consumer 线程，空闲时线程休眠
- **格式化分工**：消息体的 `std::format` 在后台 consumer 线程完成，前台生产者线程只做二进制编码

### 构造方式

```cpp
// 方式 1：工厂模式从零构造（内部创建 Sink 再包装）
auto async_console = cpp109::make_async_sink<cpp109::ConsoleSink>();

// 方式 2：包装已有的 Sink
auto file = std::make_shared<cpp109::FileSink>("app.log");
auto async_file = std::make_shared<cpp109::AsyncSink<>>(file);
```

### 多线程使用示例

```cpp
// 每个线程绑定独立的 AsyncSink + 文件，互不阻塞
for (int i = 0; i < 8; ++i) {
    auto sink = cpp109::make_async_sink<cpp109::FileSink>(
        "thread_" + std::to_string(i) + ".log", true);
    auto logger = cpp109::get_logger("worker_" + std::to_string(i));
    logger->add_sink(sink);
}
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
   .set_formatter("[%L] %v");

cfg.add_logger("admin")
   .set_sinks({"console"})
   .set_level(cpp109::LogLevel::DEBUG);

cfg.apply();
```

任意 sink 可通过 `set_async()` 包装：

```cpp
cfg.add_sink("file")
   .set_class<cpp109::RotatingFileSink>()
   .set_property("filename", "logs/app.log")
   .set_property("max_size_mb", "10")
   .set_property("max_files", "5")
    .set_async()                                   // 默认容量 1MB，overflow_policy=block
    .set_queue_size(1 << 20)                       // 可选：自定义队列容量（字节）
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

### 入队延迟（async + FileSink）

| 场景            | P50      | P99       |
|----------------|----------|-----------|
| async + args   | ~18 ns   | ~104 ns   |
| async no args  | ~16 ns   | ~78 ns    |

- 测量方式：rdtsc，200K 预热 + 2M 测量
- 编译器：MSVC /O2
- CPU：i9 12900HX

### 多线程吞吐（async + FileSink，per-thread 独立文件）

| 线程数 | 总吞吐量             |
|-------|---------------------|
| 1t    | ~5,000,000 msg/s    |
| 4t    | ~16,000,000 msg/s   |
| 8t    | ~21,900,000 msg/s   |
| 16t   | ~32,400,000 msg/s   |

- 测量方式：1s 预热 + 2s 测量
- 编译器：MSVC /O2
- 纯日志无模拟工作负载
