//
// logger.h — Minimalist structured logger for Orion.
//
// Why roll our own?
//   spdlog / fmt / otel-cpp each solve this better at the cost of a dep. We
//   want zero-external-dep JSON lines so `docker logs | jq` works out of
//   the box. If we later swap to otel-cpp the call sites don't change.
//
// Output format:
//   {"ts":"2026-04-17T14:03:12.512Z","lvl":"INFO","comp":"ClusterHead",
//    "event":"task_dispatched","task_id":"t1","node_id":"node-2"}
//
// Thread-safety:
//   A single mutex around std::cerr. The cost is negligible for the volume
//   we emit (O(100) lines/sec at worst).
//
// Usage:
//   LOG_INFO("ClusterHead", "task_dispatched",
//            {{"task_id", task_id}, {"node_id", node_id}});
//
// Field values are all strings so callers don't fight overloads. Numbers
// should be stringified (std::to_string) at the call site.
//
#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

namespace orion::observability {

// Thread-local trace context so the logger can pick it up without 
// threading it through every call site.
extern thread_local std::string g_current_trace_id;

enum class LogLevel { DEBUG, INFO, WARN, ERROR, CRITICAL };

inline const char* level_str(LogLevel l) {
    switch (l) {
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO";
        case LogLevel::WARN:     return "WARN";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRITICAL";
    }
    return "?";
}

// Minimal JSON string escaping: handles ", \, \n, \r, \t, control chars.
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

inline std::string iso8601_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    auto t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return os.str();
}

class Logger {
public:
    static Logger& instance() {
        static Logger l;
        return l;
    }

    void log(LogLevel lvl,
             const std::string& component,
             const std::string& event,
             const std::vector<std::pair<std::string, std::string>>& fields) {
        std::ostringstream os;
        os << "{\"ts\":\"" << iso8601_now() << "\""
           << ",\"lvl\":\"" << level_str(lvl) << "\""
           << ",\"comp\":\"" << json_escape(component) << "\""
           << ",\"event\":\"" << json_escape(event) << "\"";

        if (!g_current_trace_id.empty()) {
            os << ",\"trace_id\":\"" << g_current_trace_id << "\"";
        }

        for (const auto& [k, v] : fields) {
            os << ",\"" << json_escape(k) << "\":\"" << json_escape(v) << "\"";
        }
        os << "}\n";

        std::lock_guard<std::mutex> lock(mu_);
        // stderr for errors, stdout for the rest — matches conventional log ship-out.
        if (lvl == LogLevel::ERROR || lvl == LogLevel::CRITICAL || lvl == LogLevel::WARN) {
            std::cerr << os.str() << std::flush;
        } else {
            std::cout << os.str() << std::flush;
        }
    }

private:
    Logger() = default;
    std::mutex mu_;
};

} // namespace orion::observability

// Variadic macros so the brace-initializer-list's inner commas don't split
// macro arguments. Call sites look like:
//     LOG_INFO("ClusterHead", "task_dispatched",
//              {"task_id", task_id},
//              {"node_id", node_id});
// Each `{k, v}` is a std::pair; they become initializer elements of the vector.
#define ORION_LOG_FIELDS(...) \
    std::vector<std::pair<std::string, std::string>>{__VA_ARGS__}

#define LOG_INFO(comp, event, ...) \
    ::orion::observability::Logger::instance().log( \
        ::orion::observability::LogLevel::INFO, (comp), (event), \
        ORION_LOG_FIELDS(__VA_ARGS__))
#define LOG_WARN(comp, event, ...) \
    ::orion::observability::Logger::instance().log( \
        ::orion::observability::LogLevel::WARN, (comp), (event), \
        ORION_LOG_FIELDS(__VA_ARGS__))
#define LOG_ERROR(comp, event, ...) \
    ::orion::observability::Logger::instance().log( \
        ::orion::observability::LogLevel::ERROR, (comp), (event), \
        ORION_LOG_FIELDS(__VA_ARGS__))
#define LOG_CRITICAL(comp, event, ...) \
    ::orion::observability::Logger::instance().log( \
        ::orion::observability::LogLevel::CRITICAL, (comp), (event), \
        ORION_LOG_FIELDS(__VA_ARGS__))
