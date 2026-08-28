#pragma once

#include <chrono>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

template <> struct std::formatter<std::filesystem::path> : std::formatter<std::string> {
    auto format(const std::filesystem::path& p, std::format_context& ctx) const {
        return std::formatter<std::string>::format(p.string(), ctx);
    }
};

enum LogLevel { TRACE = 0, DEBUG, INFO, WARN, ERR, CRIT };

namespace debug {
    inline bool verbose = false;
    inline bool quiet = false;

    inline std::mutex log_mutex;

    inline const char* level_name(LogLevel level) {
        switch (level) {
        case TRACE:
            return "TRACE";
        case DEBUG:
            return "DEBUG";
        case INFO:
            return "INFO";
        case WARN:
            return "WARN";
        case ERR:
            return "ERR";
        case CRIT:
            return "CRIT";
        default:
            return "";
        }
    }

    template <typename... Args> void log(LogLevel level, const std::string& fmt, Args&&... args) {

        if (quiet && level < ERR) {
            return;
        }
        if (!verbose && level == TRACE) {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm{};
        localtime_r(&t, &tm);

        std::ostringstream line;
        line << '[' << std::put_time(&tm, "%F %T") << '.' << std::setfill('0') << std::setw(3) << ms.count()
             << std::setfill(' ') << "] ";
        line << '[' << level_name(level) << "] ";
        line << std::vformat(fmt, std::make_format_args(args...)) << '\n';

        std::lock_guard<std::mutex> lock(log_mutex);
        std::cerr << line.str() << std::flush;
    }
} // namespace debug
