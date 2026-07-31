// Core/Log.h - minimal, dependency-free logging with severity levels.
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace hbe {

enum class LogLevel { Trace, Info, Warn, Error };

// Most-recent log lines (oldest first), newline-joined, up to `maxLines`. Thread-
// safe. Backs the studio/boot screen's {log} token so it reads like a boot console.
std::string RecentLog(unsigned maxLines = 12);

// Flush every log sink (stdout/stderr + the on-disk log file) right now. Call from a
// crash handler so the last lines survive a hard termination.
void FlushLog();

// TRACE IS OFF BY DEFAULT, AND THAT IS LOAD-BEARING, NOT A STYLE CHOICE.
//
// LogWrite is unbuffered: every line costs fflush(stdout) + fflush(<exe>.log) +
// OutputDebugStringA (~100 us each under a debugger) on the CALLING thread, and every
// line also lands in the RecentLog ring that backs the boot/loading screen's `{log}`
// token. That is fine for once-per-level events and wrong for per-frame ones: tag
// streaming spawns and despawns shards while the loading screen is up, so its per-event
// lines were both an unbudgeted main-thread cost inside the structural window AND the
// text the player saw on the loading screen instead of boot status.
//
// So HBE_TRACE is for high-frequency diagnostics, and when tracing is off those calls
// write nothing, flush nothing and record nothing - only the std::format cost remains.
// Turn it on to debug streaming/instantiate churn.
void SetTraceEnabled(bool enabled);
bool TraceEnabled();

namespace detail {
// Writes an already-formatted line to the console (and the debugger on Windows).
void LogWrite(LogLevel level, std::string_view message);
} // namespace detail

template <typename... Args>
void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    detail::LogWrite(level, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace hbe

// Convenience macros - compile to nothing-special but read cleanly at call sites.
#define HBE_TRACE(...) ::hbe::Log(::hbe::LogLevel::Trace, __VA_ARGS__)
#define HBE_INFO(...)  ::hbe::Log(::hbe::LogLevel::Info,  __VA_ARGS__)
#define HBE_WARN(...)  ::hbe::Log(::hbe::LogLevel::Warn,  __VA_ARGS__)
#define HBE_ERROR(...) ::hbe::Log(::hbe::LogLevel::Error, __VA_ARGS__)
