#pragma once

#include "log_level.hpp"
#include "platform.hpp"
#include "timestamp.hpp"
#include "log_event.hpp"
#include "formatter.hpp"
#include "sink.hpp"
#include "logger.hpp"
#include "registry.hpp"
#include "ring_buffer.hpp"
#include "async_sink.hpp"
#include "config.hpp"
//#include "config_loader.hpp"

#include "sinks/console_sink.hpp"
#include "sinks/file_sink.hpp"
#include "sinks/rotating_file_sink.hpp"
#include "sinks/daily_file_sink.hpp"
#include "sinks/callback_sink.hpp"
#include "sinks/null_sink.hpp"

#define CPP109_DEFAULT_LOGGER() \
    cpp109::Registry::instance().default_logger()

#define LOG_TRACE(fmt, ...) CPP109_DEFAULT_LOGGER()->trace(fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) CPP109_DEFAULT_LOGGER()->debug(fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  CPP109_DEFAULT_LOGGER()->info(fmt,  ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  CPP109_DEFAULT_LOGGER()->warn(fmt,  ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) CPP109_DEFAULT_LOGGER()->error(fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) CPP109_DEFAULT_LOGGER()->fatal(fmt, ##__VA_ARGS__)

#define LOG_INFO_IF(cond, fmt, ...)  if (cond) LOG_INFO(fmt,  ##__VA_ARGS__)
#define LOG_WARN_IF(cond, fmt, ...)  if (cond) LOG_WARN(fmt,  ##__VA_ARGS__)
#define LOG_ERROR_IF(cond, fmt, ...) if (cond) LOG_ERROR(fmt, ##__VA_ARGS__)

#define LOG_INFO_EVERY_N(n, fmt, ...) \
    do { \
        static unsigned int _counter = 0; \
        if (++_counter % (n) == 1) \
            LOG_INFO(fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_WARN_EVERY_N(n, fmt, ...) \
    do { \
        static unsigned int _counter = 0; \
        if (++_counter % (n) == 1) \
            LOG_WARN(fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR_EVERY_N(n, fmt, ...) \
    do { \
        static unsigned int _counter = 0; \
        if (++_counter % (n) == 1) \
            LOG_ERROR(fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_INFO_FIRST_N(n, fmt, ...) \
    do { \
        static unsigned int _counter = 0; \
        if (++_counter <= static_cast<unsigned int>(n)) \
            LOG_INFO(fmt, ##__VA_ARGS__); \
    } while(0)
