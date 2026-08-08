// Core/Platform_Win32.cpp - the Windows implementation of Core/Platform.h.
//
// This is the ONLY file in the platform layer that includes <windows.h>. A second backend
// would sit beside it as Platform_Linux.cpp and the CMake selection would pick one, exactly
// as the RHI already does for D3D12/Vulkan/OpenGL.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "Core/Platform.h"

#include <cstdio>
#include <vector>

namespace fs = std::filesystem;

namespace hbe::platform {

fs::path ExecutablePath() {
    // GROWS RATHER THAN TRUNCATING. Every one of the twelve hand-rolled copies of this in
    // the tree used a fixed MAX_PATH buffer, which silently returns a TRUNCATED path when
    // the real one is longer - and a truncated path is worse than none, because it looks
    // like a valid path to a file that does not exist. Long paths are ordinary now: a
    // OneDrive-redirected profile with a deep project folder passes 260 characters easily.
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {}; // the OS refused; nothing sensible to return
        if (n < buf.size()) return fs::path(std::wstring(buf.data(), n));
        if (buf.size() >= 32768) return {}; // the Windows path ceiling; stop growing
        buf.resize(buf.size() * 2);
    }
}

fs::path ExecutableDir() {
    const fs::path p = ExecutablePath();
    return p.empty() ? fs::path() : p.parent_path();
}

fs::path UserDataRoot() {
    std::error_code ec;
    fs::path root;
    // The environment variable rather than SHGetKnownFolderPath: it is what every existing
    // copy in this tree used, so this cannot change where anyone's settings already live.
    // Switching to the shell API would silently orphan existing data on some configurations.
    if (const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0); n > 0) {
        std::vector<wchar_t> buf(n);
        const DWORD got = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), n);
        if (got > 0 && got < n) root = fs::path(std::wstring(buf.data(), got));
    }
    // ONE fallback policy, not two. The old copies disagreed - five fell back to the temp
    // directory and one to ".", which meant a machine with no LOCALAPPDATA wrote its
    // identity key next to the executable while everything else went to temp.
    if (root.empty()) root = fs::temp_directory_path(ec);
    return root;
}

fs::path UserDataDir() {
    const fs::path root = UserDataRoot();
    if (root.empty()) return {};
    std::error_code ec;
    const fs::path dir = root / "HeartbreakEngine";
    fs::create_directories(dir, ec);
    return dir;
}

fs::path UserDataDir(const std::string& name) {
    const fs::path base = UserDataDir();
    if (base.empty() || name.empty()) return base;
    std::error_code ec;
    const fs::path dir = base / name;
    fs::create_directories(dir, ec);
    return dir;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    const bool ok =
        ::GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size) != 0;
    ::CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

std::string MachineId() {
    // The machine GUID the OS itself keeps. Read-only, no elevation needed, and stable
    // across reboots and hardware changes in a way a MAC address or volume serial is not.
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
                        KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t value[128] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LSTATUS r =
        ::RegQueryValueExW(key, L"MachineGuid", nullptr, &type,
                           reinterpret_cast<LPBYTE>(value), &bytes);
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_SZ) return {};
    const int need =
        ::WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<usize>(need - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), need, nullptr, nullptr);
    return out;
}

bool LaunchDetached(const fs::path& exe, const std::vector<fs::path>& args) {
    // Build the command line: "exe" "arg0" "arg1" ... - each token quoted so a space in a
    // path survives. A double-quote cannot appear in a Windows path, so plain quote-wrapping
    // is sufficient (and matches the hand-rolled launch this replaced). Quoting a flag like
    // "--project" is harmless: the child's argv parser strips the quotes.
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    for (const fs::path& a : args) {
        cmd += L" \"";
        cmd += a.wstring();
        cmd += L'"';
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Run the exe DIRECTLY (lpApplicationName set) - no shell, no PATH search. CreateProcess
    // may write into the command-line buffer, so it must be mutable: cmd.data(), not c_str().
    if (!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                          &si, &pi)) {
        return false;
    }
    ::CloseHandle(pi.hThread); // a detached hand-off, not a child we wait on
    ::CloseHandle(pi.hProcess);
    return true;
}

// InstallCrashHandler() is deliberately NOT here. It calls into Core/Log (HBE_ERROR/FlushLog),
// and this translation unit is also linked into hbe_hubcore (the engine-free launcher), which
// does not link Log.cpp - putting the handler here would drag an unresolved Log symbol into
// the Hub. It lives in its own TU, Core/CrashHandler_Win32.cpp, added only to the engine
// libraries. SystemUiFontCandidates() below uses nothing but kernel32, so it stays.

std::vector<fs::path> SystemUiFontCandidates() {
    // ASK THE OS for its Windows directory rather than hardcoding "C:\\Windows". The two
    // copies of this that this replaces both assumed C:, which is simply wrong on a machine
    // whose system drive is D: (an SSD swap, an enterprise image) - there the font failed to
    // load and UI text silently vanished. GetWindowsDirectory is kernel32, so this adds no
    // link dependency; the Fonts subfolder is a fixed relative name under it.
    fs::path fonts;
    if (const UINT n = ::GetWindowsDirectoryW(nullptr, 0); n > 0) {
        std::vector<wchar_t> buf(n);
        const UINT got = ::GetWindowsDirectoryW(buf.data(), n);
        if (got > 0 && got < n) fonts = fs::path(std::wstring(buf.data(), got)) / L"Fonts";
    }
    if (fonts.empty()) fonts = fs::path(L"C:\\Windows\\Fonts"); // last-resort, matches old behaviour

    std::vector<fs::path> out;
    // Preference order preserved from the old ReadFontFile: Segoe UI (the modern system UI
    // face), then Arial, then Tahoma as a floor that exists on essentially every install.
    for (const wchar_t* face : {L"segoeui.ttf", L"arial.ttf", L"tahoma.ttf"})
        out.push_back(fonts / face);
    return out;
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("platform FAIL: %s\n", what);
        ++g_fails;
    }
}
} // namespace

bool SelfTest() {
    g_fails = 0;
    std::error_code ec;

    const fs::path exe = ExecutablePath();
    Check(!exe.empty(), "the running executable's path must be discoverable");
    Check(fs::exists(exe, ec), "...and must actually exist on disk");
    Check(exe.is_absolute(), "...and must be absolute, never relative to the working dir");
    Check(exe.has_filename(), "...and must name a file");

    const fs::path dir = ExecutableDir();
    Check(!dir.empty() && fs::is_directory(dir, ec), "the executable's folder must exist");
    Check(dir == exe.parent_path(), "ExecutableDir must agree with ExecutablePath");
    // The distinction the twelve hand-rolled copies existed to make: this must NOT be the
    // working directory, which a shortcut or debugger routinely sets elsewhere.
    Check(dir.is_absolute(), "the executable folder must be absolute, not CWD-relative");

    const fs::path rootDir = UserDataRoot();
    Check(!rootDir.empty(), "a per-user root must always be produced");
    Check(rootDir.is_absolute(), "...and be absolute");

    const fs::path data = UserDataDir();
    Check(data.parent_path() == rootDir,
          "UserDataDir must sit directly under UserDataRoot - a shipped game uses the ROOT "
          "with its own name, and the two must not drift apart");
    Check(!data.empty(), "a per-user data directory must always be produced");
    Check(fs::is_directory(data, ec), "...and must be created if it did not exist");
    Check(data.filename() == "HeartbreakEngine", "...under our own name, not the profile root");
    Check(UserDataDir() == data, "repeated calls must agree");

    {
        const fs::path sub = UserDataDir("selftest");
        Check(fs::is_directory(sub, ec), "a named subdirectory must be created on demand");
        Check(sub.parent_path() == data, "...directly under the data directory");
        fs::remove(sub, ec);
        // An empty name must not escape the data directory or produce a trailing separator.
        Check(UserDataDir("") == data, "an empty subdirectory name yields the base directory");
    }

    // MachineId is allowed to be empty on a locked-down machine, but if it answers at all it
    // must be stable - a value that changed per call would silently break anything keyed on it.
    const std::string id = MachineId();
    if (!id.empty()) {
        Check(MachineId() == id, "the machine id must be STABLE across calls");
        Check(id.size() >= 8, "a machine id that short is not an identifier");
    }

    // System fonts: the list must exist, every entry must be an absolute path (a caller feeds
    // it straight to a file open), and on Windows at least one of the faces must actually be
    // present - a machine with no Segoe UI / Arial / Tahoma at all does not exist in practice,
    // and if this returns nothing the UI silently loses its text.
    const std::vector<fs::path> fonts = SystemUiFontCandidates();
    Check(!fonts.empty(), "at least one system UI font candidate must be offered");
    bool anyFontExists = false;
    for (const fs::path& f : fonts) {
        Check(f.is_absolute(), "every font candidate must be an absolute path");
        if (fs::exists(f, ec)) anyFontExists = true;
    }
    Check(anyFontExists, "at least one offered system UI font must exist on this machine");

    (void)IsElevated(); // must not crash; the value itself is environment-dependent

    if (g_fails == 0)
        std::printf("platform: one executable path (grown, never truncated at MAX_PATH like "
                    "the 12 hand-rolled copies), one user-data directory with ONE fallback "
                    "policy instead of the previous two, a stable machine id, and system UI "
                    "fonts resolved from the OS Windows dir (not a hardcoded C:)\n");
    return g_fails == 0;
}

} // namespace hbe::platform
