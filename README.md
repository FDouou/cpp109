# 109cpp
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

## Sink 列表

| Sink | 说明 |
|------|------|
| `ConsoleSink` | 控制台输出，支持彩色日志（按级别着色），可输出到 stdout 或 stderr |
| `FileSink` | 文件输出，支持覆盖/追加模式，可配置自动 flush 间隔 |
| `RotatingFileSink` | 按大小滚动的文件输出，达到指定大小后自动滚动，可配置保留文件数 |
| `DailyFileSink` | 按时间滚动的文件输出，支持按分钟/小时/天创建新文件，文件名支持时间占位符 |
| `CallbackSink` | 自定义回调，每条日志触发回调函数，适用于发送到网络、数据库或第三方监控 |

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

## 构建

```bash
cmake -B build -S .
cmake --build build
```

### 运行测试

```bash
ctest --test-dir build
```
