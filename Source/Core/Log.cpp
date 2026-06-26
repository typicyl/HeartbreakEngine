// Core/Log.cpp
#include "Core/Log.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace hbe {

namespace {
// In-memory ring of recent log lines (worker threads log too, so guard it).
constexpr std::size_t kLogHistoryCap = 256;
std::mutex g_logMutex;
std::deque<std::string> g_logHistory;

void RecordLine(std::string_view message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logHistory.emplace_back(message);
    while (g_logHistory.size() > kLogHistoryCap) g_logHistory.pop_front();
}
} // namespace

std::string RecentLog(unsigned maxLines) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    const std::size_t n = std::min<std::size_t>(maxLines, g_logHistory.size());
    std::string out;
    for (std::size_t i = g_logHistory.size() - n; i < g_logHistory.size(); ++i) {
        if (!out.empty()) out += '\n';
        out += g_logHistory[i];
    }
    return out;
}

namespace detail {

namespace {
const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "[trace]";
        case LogLevel::Info:  return "[info ]";
        case LogLevel::Warn:  return "[warn ]";
        case LogLevel::Error: return "[error]";
    }
    return "[?????]";
}
} // namespace

void LogWrite(LogLevel level, std::string_view message) {
    std::string line = std::string(LevelTag(level)) + " " + std::string(message) + "\n";

    std::FILE* out = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
    std::fputs(line.c_str(), out);
    std::fflush(out);

#if defined(_WIN32)
    // Mirror to the VS/WinDbg output pane so logs show up under the debugger.
    ::OutputDebugStringA(line.c_str());
#endif

    RecordLine(message); // keep the boot/studio screen's {log} console current
}

} // namespace detail
} // namespace hbe
