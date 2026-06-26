cmake_minimum_required(VERSION 3.20)
project(109cpp
    VERSION 1.0.0
    DESCRIPTION "logging library for C++20"
    LANGUAGES CXX
)

# ── C++ 标准：强制 C++20（依赖 std::format + std::source_location）─────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── 检查 C++20 关键特性 ─────────────────────────────────────────────
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
    #include <format>
    #include <source_location>
    int main() {
        auto s = std::format(\"hello {}\", 42);
        auto loc = std::source_location::current();
        return 0;
    }
" LOG109CPP_CXX20_SUPPORTED)
if(NOT LOG109CPP_CXX20_SUPPORTED)
    message(FATAL_ERROR "109cpp requires a C++20 compiler with <format> and <source_location> support")
endif()

# ── 构建类型 ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── 编译选项 ────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/O2 /Ob2 /Oi /Ot /utf-8)
else()
    add_compile_options(-O3 -march=native)
endif()
# 日志级别编译期开关（可选），低于该级别的宏调用在编译期优化掉
# add_compile_definitions(LOG109CPP_ACTIVE_LEVEL=2)  # 0=TRACE, 1=DEBUG, 2=INFO ...

# ── 第三方依赖 ──────────────────────────────────────────────────────
include(FetchContent)

# quill v8.2.0（异步日志库，用于 benchmark 对比）
# 注意：quill 的 CMakeLists.txt 使用 project(quill) 默认启用 C 和 CXX。
# 若 C 编译器检测失败（如缺少 Windows SDK），可设置以下变量跳过：
#   set(CMAKE_C_COMPILER_WORKS TRUE CACHE INTERNAL "")
FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG        v8.2.0
    GIT_SHALLOW    TRUE
)
set(QUILL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QUILL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(QUILL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(quill)

# ── 库目标（header-only）──────────────────────────────────────────
add_library(109cpp INTERFACE)

target_include_directories(109cpp INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ── 示例程序 ────────────────────────────────────────────────────────
add_executable(example_basic examples/basic.cpp)
target_link_libraries(example_basic 109cpp)

add_executable(example_file examples/file_output.cpp)
target_link_libraries(example_file 109cpp)

add_executable(example_rotating examples/rotating.cpp)
target_link_libraries(example_rotating 109cpp)

add_executable(example_async examples/async.cpp)
target_link_libraries(example_async 109cpp)

add_executable(example_custom_fmt examples/custom_formatter.cpp)
target_link_libraries(example_custom_fmt 109cpp)

# ── 测试 ────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_async tests/test_async.cpp)
target_link_libraries(test_async 109cpp)
add_test(NAME test_async COMMAND test_async)

add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config 109cpp)
add_test(NAME test_config COMMAND test_config)

add_executable(test_formatter tests/test_formatter.cpp)
target_link_libraries(test_formatter 109cpp)
add_test(NAME test_formatter COMMAND test_formatter)

add_executable(test_level tests/test_level.cpp)
target_link_libraries(test_level 109cpp)
add_test(NAME test_level COMMAND test_level)

add_executable(test_logger tests/test_logger.cpp)
target_link_libraries(test_logger 109cpp)
add_test(NAME test_logger COMMAND test_logger)

add_executable(test_sinks tests/test_sinks.cpp)
target_link_libraries(test_sinks 109cpp)
add_test(NAME test_sinks COMMAND test_sinks)

add_executable(test_macros tests/test_macros.cpp)
target_link_libraries(test_macros 109cpp)
add_test(NAME test_macros COMMAND test_macros)

# ── 性能基准（手动运行，不加入 ctest）────────────────────────────
add_executable(bench bench/bench.cpp)
target_link_libraries(bench 109cpp)

add_executable(bench_latency bench/bench_latency.cpp)
target_link_libraries(bench_latency 109cpp)

add_executable(bench_latency_breakdown bench/bench_latency_breakdown.cpp)
target_link_libraries(bench_latency_breakdown 109cpp)

add_executable(bench_latency_instrumented bench/bench_latency_instrumented.cpp)
target_link_libraries(bench_latency_instrumented 109cpp)

add_executable(batch_test bench/batch_test.cpp)
target_link_libraries(batch_test 109cpp)

add_executable(bench_latency_quill bench/bench_latency_quill.cpp)
target_link_libraries(bench_latency_quill PRIVATE quill::quill)
