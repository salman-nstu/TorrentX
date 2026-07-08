#pragma once

#include <mutex>
#include <sstream>
#include <string>

namespace tx::log {

enum class Level {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

/// Minimal thread-safe logger writing to stderr.
///
/// Usage: tx::log::info("connected to ", endpoint, " in ", ms, "ms");
class Logger {
public:
    static Logger& instance();

    void setLevel(Level level) { m_level = level; }
    Level level() const { return m_level; }

    void write(Level level, const std::string& message);

private:
    Logger() = default;

    std::mutex m_mutex;
    Level m_level = Level::Info;
};

namespace detail {

template <typename... Args>
void log(Level level, Args&&... args)
{
    Logger& logger = Logger::instance();
    if (level < logger.level()) {
        return;
    }
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    logger.write(level, oss.str());
}

} // namespace detail

template <typename... Args>
void debug(Args&&... args) { detail::log(Level::Debug, std::forward<Args>(args)...); }

template <typename... Args>
void info(Args&&... args) { detail::log(Level::Info, std::forward<Args>(args)...); }

template <typename... Args>
void warn(Args&&... args) { detail::log(Level::Warn, std::forward<Args>(args)...); }

template <typename... Args>
void error(Args&&... args) { detail::log(Level::Error, std::forward<Args>(args)...); }

} // namespace tx::log
