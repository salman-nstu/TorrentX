#include "torrentx/util/logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace tx::log {

namespace {

const char* levelTag(Level level)
{
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

std::string timestamp()
{
    using std::chrono::system_clock;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count() % 1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(millis));
    return buffer;
}

} // namespace

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

void Logger::write(Level level, const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fprintf(stderr, "%s %s %s\n", timestamp().c_str(), levelTag(level), message.c_str());
}

} // namespace tx::log
