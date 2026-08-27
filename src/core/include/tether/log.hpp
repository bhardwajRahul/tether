#pragma once

#include <chrono>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>

template <> struct std::formatter<std::filesystem::path> : std::formatter<std::string> {
    auto format(const std::filesystem::path& p, std::format_context& ctx) const {
        return std::formatter<std::string>::format(p.string(), ctx);
    }
};

enum LogLevel { TRACE = 0, DEBUG, INFO, WARN, ERR, CRIT };

namespace debug {
    inline bool verbose = false;
    inline bool quiet = false;
    template <typename... Args> void log(LogLevel level, const std::string& fmt, Args&&... args) {

        if (quiet && level < ERR) {
            return;
        }
        if (!verbose && level == TRACE) {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm{};
        localtime_r(&t, &tm);
        std::cerr << '[' << std::put_time(&tm, "%F %T") << '.' << std::setfill('0') << std::setw(3)
                  << ms.count() << std::setfill(' ') << "] ";

        std::cerr << '[';

        switch (level) {
        case TRACE:
            std::cerr << "TRACE";
            break;
        case DEBUG:
            std::cerr << "DEBUG";
            break;
        case INFO:
            std::cerr << "INFO";
            break;
        case WARN:
            std::cerr << "WARN";
            break;
        case ERR:
            std::cerr << "ERR";
            break;
        case CRIT:
            std::cerr << "CRIT";
            break;
        default:
            break;
        }

        std::cerr << "] ";

        std::cerr << std::vformat(fmt, std::make_format_args(args...)) << std::endl;
    }
} // namespace debug
