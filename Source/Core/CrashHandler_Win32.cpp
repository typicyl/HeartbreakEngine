// Core/CrashHandler_Win32.cpp - the Windows implementation of platform::InstallCrashHandler().
//
// WHY ITS OWN FILE, and not part of Platform_Win32.cpp. This handler's whole job is to LOG
// before the process dies, so it depends on Core/Log. Platform_Win32.cpp is also linked into
// hbe_hubcore - the engine-free launcher core - which does not link Log.cpp; folding the
// handler in there would drag an unresolved hbe::FlushLog into the Hub's link. So the process
// crash handler lives here, in a translation unit added only to the engine libraries. A
// second platform adds CrashHandler_Posix.cpp (sigaction + backtrace) beside this one.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "Core/Platform.h"
#include "Core/Log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hbe::platform {

namespace {

// Last-resort SEH handler. An access violation during init (e.g. a graphics driver /
// device-creation fault on another machine) would otherwise terminate the process SILENTLY,
// leaving only the pre-crash log - exactly the "it logged 'starting' and quit" report. Log
// the code + address, flush every sink (incl. the on-disk log), then let the OS finish. Turns
// a silent death into an actionable crash line. (Moved here verbatim from Engine.cpp, which is
// what let that file stop including <windows.h> for a single function.)
LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    const u32 code = rec ? rec->ExceptionCode : 0u;
    const void* addr = rec ? rec->ExceptionAddress : nullptr;

    // Resolve the faulting *instruction* to the module (DLL/exe) it lives in. This is what
    // turns "0x7FFB.. in some DLL" into an actual name - e.g. our own exe (a real bug), a GPU
    // driver (nvwgf2umx/atidxx), or kernel32 (a null handle we handed it).
    char modName[MAX_PATH] = "<unknown module>";
    uintptr_t modOffset = 0;
    if (addr) {
        HMODULE mod = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(addr), &mod) &&
            mod) {
            // DELIBERATELY NOT ExecutablePath(). This asks a different question: "which MODULE
            // does this faulting address live in?" - the answer is a DLL as often as the exe,
            // which is the entire point of printing it. ExecutablePath only ever answers for
            // the running process. Leave this one on the raw API.
            char full[MAX_PATH] = {};
            if (::GetModuleFileNameA(mod, full, MAX_PATH) > 0) {
                const char* leaf = std::strrchr(full, '\\');
                std::snprintf(modName, sizeof(modName), "%s", leaf ? leaf + 1 : full);
            }
            modOffset = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
        }
    }

    // For an access violation, ExceptionInformation says read vs write and the exact address
    // touched. A tiny address = a NULL/near-null deref (our own bad pointer, NOT a graphics
    // driver) - which points the finger squarely at engine code.
    if (code == 0xC0000005u /*EXCEPTION_ACCESS_VIOLATION*/ && rec && rec->NumberParameters >= 2) {
        const unsigned long long rw = rec->ExceptionInformation[0]; // 0 read,1 write,8 DEP
        const unsigned long long bad = rec->ExceptionInformation[1];
        const char* op = rw == 1 ? "write to" : (rw == 8 ? "execute at" : "read from");
        HBE_ERROR("FATAL: access violation in {}+0x{:X} - tried to {} 0x{:016X}{}",
                  modName, modOffset, op, bad,
                  bad < 0x10000ull ? " (NULL/near-null pointer - an engine bug, not the GPU)."
                                   : " - engine terminating.");
    } else {
        HBE_ERROR("FATAL: unhandled exception 0x{:08X} in {}+0x{:X} at 0x{:016X} - terminating.",
                  code, modName, modOffset, reinterpret_cast<uintptr_t>(addr));
    }
    HBE_ERROR("Send the <exe>.log file next to the executable. If this is graphics-side, try "
              "--d3d12 / --vulkan / --opengl and update the GPU driver.");
    FlushLog();
    return EXCEPTION_EXECUTE_HANDLER; // stop searching; the process ends
}

} // namespace

void InstallCrashHandler() {
    // Install first so a crash ANYWHERE in boot gets logged before the process dies.
    ::SetUnhandledExceptionFilter(&CrashHandler);
}

} // namespace hbe::platform
