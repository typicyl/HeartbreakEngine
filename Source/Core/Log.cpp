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

// Persistent on-disk log next to the executable ("<exe>.log"). Written in addition
// to stdout/stderr so a shipped build leaves a post-mortem behind even when there's
// no console attached (the "opens the log and quits" case). Lazily opened on the
// first line; if the directory isn't writable we simply skip it (never fatal).
std::mutex g_fileMutex;
std::FILE* g_logFile = nullptr;
bool g_logFileTried = false;

std::FILE* LogFile() {
    if (g_logFileTried) return g_logFile;
    g_logFileTried = true;
#if defined(_WIN32)
    char exePath[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path = (n > 0 && n < MAX_PATH) ? std::string(exePath) + ".log" : "HeartbreakEngine.log";
    if (::fopen_s(&g_logFile, path.c_str(), "w") != 0) g_logFile = nullptr;
#else
    g_logFile = std::fopen("HeartbreakEngine.log", "w");
#endif
    return g_logFile;
}
} // namespace

void FlushLog() {
    std::fflush(stdout);
    std::fflush(stderr);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    if (g_logFile) std::fflush(g_logFile);
}

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

    // Mirror to a persistent log file next to the exe (flushed per line), so a crash
    // on another machine leaves the full boot trail on disk, not just in a console.
    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        if (std::FILE* f = LogFile()) {
            std::fputs(line.c_str(), f);
            std::fflush(f);
        }
    }

#if defined(_WIN32)
    // Mirror to the VS/WinDbg output pane so logs show up under the debugger.
    ::OutputDebugStringA(line.c_str());
#endif

    RecordLine(message); // keep the boot/studio screen's {log} console current
}

} // namespace detail
} // namespace hbe
