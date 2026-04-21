#pragma once
#include "host.h"
#include <iostream>
#include <ctime>
#include <iomanip>

class Logger {
public:
    enum Level { DEBUG_L=0, INFO_L=1, WARN_L=2, ERR_L=3 };
    static Logger& get() { static Logger l; return l; }

    void set_level(const std::string& lvl) {
        if (lvl=="DEBUG") min_level_=DEBUG_L;
        else if (lvl=="WARN") min_level_=WARN_L;
        else if (lvl=="ERROR") min_level_=ERR_L;
        else min_level_=INFO_L;
    }

    // Intentionally a no-op: file logging is disabled project-wide.
    // All log output goes to stdout (console) only.
    void set_file(const std::string& /*path*/) { /* disabled */ }

    void log(Level /*lvl*/, const std::string& /*msg*/) {
        // Intentionally silent. Previously formatted + wrote to stdout;
        // in svchost context stdout is unreachable anyway, and the user
        // explicitly asked for "no logs at all" — so we skip the format
        // step entirely (small CPU win on hot paths like evtlog cleaner
        // and stage-2 prefetch retries).
    }

    void info (const std::string& m){ log(INFO_L, m); }
    void warn (const std::string& m){ log(WARN_L, m); }
    void error(const std::string& m){ log(ERR_L,  m); }
    void debug(const std::string& m){ log(DEBUG_L,m); }

private:
    Logger() = default;
    std::mutex mu_;
    Level min_level_ = INFO_L;
};

#define LOG_INFO(m)  Logger::get().info(m)
#define LOG_WARN(m)  Logger::get().warn(m)
#define LOG_ERROR(m) Logger::get().error(m)
#define LOG_DEBUG(m) Logger::get().debug(m)
