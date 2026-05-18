#pragma once

#include <string>
#include <string_view>
#include "util/string.hpp"

namespace cpp109 {

enum class LogLevel : int {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5,
    OFF   = 6
};

const char* level_to_string_caps(LogLevel level) noexcept{
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        case LogLevel::OFF:   return "OFF";
    }
    return nullptr;
}

const char* level_to_string(LogLevel level) noexcept{
    switch (level) {
        case LogLevel::TRACE: return "trace";
        case LogLevel::DEBUG: return "debug";
        case LogLevel::INFO:  return "info";
        case LogLevel::WARN:  return "warn";
        case LogLevel::ERROR: return "error";
        case LogLevel::FATAL: return "fatal";
        case LogLevel::OFF:   return "off";
    }
    return nullptr;
}

LogLevel level_from_string(std::string_view str) noexcept{
    using namespace cpp109::util;
    if (str.empty()) return LogLevel::OFF;
    if (iequals(str, "trace")) return LogLevel::TRACE;
    if (iequals(str, "debug")) return LogLevel::DEBUG;
    if (iequals(str, "info"))  return LogLevel::INFO;
    if (iequals(str, "warn"))  return LogLevel::WARN;
    if (iequals(str, "error")) return LogLevel::ERROR;
    if (iequals(str, "fatal")) return LogLevel::FATAL;
    return LogLevel::OFF;
}

} // namespace cpp109
