// ===========================================================================
//  Logger.hpp — minimal thread-safe structured logger (header-only).
//
//  Levels: Debug < Info < Warning < Error. Every record is timestamped and
//  retained in a ring buffer that the GUI can render and export. A Qt signal
//  bridge (see LogBridge) lets background threads post into the GUI safely.
// ===========================================================================
#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <ctime>
#include <cstdio>
#include <functional>

namespace sis {

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

inline const char* logLevelName(LogLevel l) {
    switch (l) { case LogLevel::Debug: return "DEBUG";
                 case LogLevel::Info: return "INFO";
                 case LogLevel::Warning: return "WARN";
                 case LogLevel::Error: return "ERROR"; }
    return "?";
}

struct LogRecord {
    double      tSec = 0;       // seconds since epoch
    LogLevel    level = LogLevel::Info;
    std::string category;       // e.g. "DAQ", "HW", "ROOT", "CONFIG"
    std::string message;
};

class Logger {
public:
    static Logger& instance() { static Logger g; return g; }

    void setSink(std::function<void(const LogRecord&)> sink) {
        std::lock_guard<std::mutex> lk(fMx); fSink = std::move(sink);
    }
    void setMinLevel(LogLevel l) { fMin = l; }
    LogLevel minLevel() const { return fMin; }

    void log(LogLevel level, const std::string& cat, const std::string& msg) {
        if ((int)level < (int)fMin) return;
        LogRecord r;
        r.tSec = (double)std::time(nullptr);
        r.level = level; r.category = cat; r.message = msg;
        std::function<void(const LogRecord&)> sink;
        {
            std::lock_guard<std::mutex> lk(fMx);
            fBuf.push_back(r);
            while (fBuf.size() > kMax) fBuf.pop_front();
            sink = fSink;
        }
        if (sink) sink(r);
    }
    void debug(const std::string& c, const std::string& m){ log(LogLevel::Debug,c,m); }
    void info (const std::string& c, const std::string& m){ log(LogLevel::Info ,c,m); }
    void warn (const std::string& c, const std::string& m){ log(LogLevel::Warning,c,m); }
    void error(const std::string& c, const std::string& m){ log(LogLevel::Error,c,m); }

    // Export the full retained buffer to a text file. Returns false on failure.
    bool exportTo(const std::string& path) {
        std::lock_guard<std::mutex> lk(fMx);
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return false;
        for (const auto& r : fBuf) {
            std::tm tm{}; std::time_t t = (std::time_t)r.tSec;
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char ts[32]; std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
            std::fprintf(f, "%s  %-5s  [%s] %s\n", ts, logLevelName(r.level),
                         r.category.c_str(), r.message.c_str());
        }
        std::fclose(f);
        return true;
    }

    std::vector<LogRecord> snapshot() {
        std::lock_guard<std::mutex> lk(fMx);
        return std::vector<LogRecord>(fBuf.begin(), fBuf.end());
    }

private:
    Logger() = default;
    static constexpr size_t kMax = 5000;
    std::mutex fMx;
    std::deque<LogRecord> fBuf;
    std::function<void(const LogRecord&)> fSink;
    LogLevel fMin = LogLevel::Debug;
};

#define SIS_LOG_INFO(cat, msg)  ::sis::Logger::instance().info(cat, msg)
#define SIS_LOG_WARN(cat, msg)  ::sis::Logger::instance().warn(cat, msg)
#define SIS_LOG_ERROR(cat, msg) ::sis::Logger::instance().error(cat, msg)
#define SIS_LOG_DEBUG(cat, msg) ::sis::Logger::instance().debug(cat, msg)

} // namespace sis
