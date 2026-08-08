// Core/Platform.h - the operating system, behind one interface.
//
// This header contains NO platform headers and NO platform types. That is the whole point,
// and it is the same discipline RHI.h already keeps: an interface that mentions HWND is not
// an abstraction of a window, it is a Win32 header with extra steps.
//
// WHY THIS EXISTS, concretely. An inventory of the tree found the same few OS questions asked
// over and over, each with its own slightly different answer:
//
//   * "where is my executable" - reimplemented TWELVE times across nine files, every one of
//     them a GetModuleFileNameW into a MAX_PATH buffer, and not one of them handling the
//     truncation case the same way.
//   * "where does per-user data go" - reimplemented SIX times, five wide-char and one narrow,
//     with two different fallback policies when the environment variable is missing.
//
// Duplication like that is not merely untidy. It means a fix applies to one copy, and the
// other eleven keep the bug - which is exactly what happened with the install-root lookup
// earlier in this project's life. Collapsing them into one implementation is worth doing on
// a Windows-only engine, and it happens to be most of what a port would need anyway.
//
// SCOPE. This is deliberately NOT a windowing or input abstraction - those live in
// Core/Window.h and Core/Input.h and are a much larger job (Window.h currently leaks the
// Win32 message signature into its public API). This is the small, boring, duplicated
// filesystem-and-process layer - executable/user paths, machine identity, the process
// crash handler, and system-font lookup - which is the part that pays for itself
// immediately. The heavier native GUI services (file/folder pickers, desktop colour
// sampling) that only the editor needs, and that drag in COM/comdlg/GDI, live in the
// sibling Core/NativeDialogs.h so the shipped runtime never links them.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hbe::platform {

// The full path of the running executable. Empty only if the OS refuses to answer, which in
// practice means the process is being torn down.
//
// EVERY caller wanting "the folder my program is in" should use ExecutableDir() rather than
// the current working directory: a program launched from a shortcut, a debugger, or another
// process routinely starts with its CWD somewhere else entirely, and several bugs in this
// codebase's history came from that assumption.
std::filesystem::path ExecutablePath();
std::filesystem::path ExecutableDir();

// The per-user data directory for this application - %LOCALAPPDATA%/HeartbreakEngine on
// Windows. Created if it does not exist.
//
// Returns a usable path even when the OS gives nothing back: falling back to the temp
// directory keeps settings, identity keys and caches working (if not persisting well) rather
// than writing them to the current directory, which on a shipped build may be read-only or
// may be somewhere the user will never find.
std::filesystem::path UserDataDir();

// A subdirectory of UserDataDir(), created on demand. `name` must be a single path component.
std::filesystem::path UserDataDir(const std::string& name);

// The per-user data ROOT itself - %LOCALAPPDATA% on Windows - WITHOUT this application's
// folder appended, and not created.
//
// Exists for one legitimate case: a SHIPPED GAME stores its settings under its own name, not
// under the engine's. Routing that through UserDataDir() would bury every game's settings
// inside a HeartbreakEngine folder and orphan the ones already written. Engine and tool code
// wants UserDataDir(); only a shipped title's own directory belongs here.
std::filesystem::path UserDataRoot();

// True when the current process is running with administrative rights. Used to explain a
// permission failure rather than to gate behaviour - asking for elevation is a decision for
// an installer, not for an engine.
bool IsElevated();

// A stable identifier for this machine, derived from OS-provided identity rather than from
// anything the user typed. Not a secret and not a licence key: it distinguishes installs.
std::string MachineId();

// Start `exe` as a NEW, fully detached process with `args`, and return at once WITHOUT
// waiting for it - this is a hand-off (the Hub launching the full editor on a project), not a
// child we manage, so its handles are dropped immediately. Returns false if the process could
// not be started; the caller decides how to report that (this stays log-free so the launcher
// core can link it). No shell is involved: `exe` is run directly, so nothing in `args` is
// interpreted by a command processor. Args are paths - flags are valid path tokens too - so
// each survives to the child in the OS-native encoding with no lossy narrow round-trip.
bool LaunchDetached(const std::filesystem::path& exe,
                    const std::vector<std::filesystem::path>& args);

// Install the process-wide "the program is dying" handler. On Windows this is the
// Structured-Exception filter that turns a silent access-violation death during boot (a
// graphics-driver fault on another machine, our own null deref) into ONE actionable log
// line naming the faulting module + offset, then flushes every sink before the OS finishes
// the process. It lived inline in Engine.cpp behind #if _WIN32, which is exactly the kind of
// OS branch this layer exists to absorb: a POSIX backend installs signal handlers here
// instead, and Engine.cpp calls this once, unconditionally, knowing nothing about either.
//
// Idempotent and safe to call before the window or renderer exists - that is the whole
// point, since the faults it catches happen during device creation.
void InstallCrashHandler();

// The system UI fonts to try, in preference order, for rendering text (the in-game UI atlas
// and the editor's ImGui theme). Absolute paths; a caller reads the first that loads. Empty
// only on a system with none of them, in which case UI text is disabled rather than crashing.
//
// This replaces two copies of a hardcoded "C:\\Windows\\Fonts\\segoeui.ttf" literal. Beyond
// being a portability leak, that literal was WRONG on any machine whose Windows is not on C:
// (an SSD-swap or enterprise image routinely puts it on D:); the Win32 backend now asks the
// OS for its actual Windows/Fonts directory. A second platform returns its own faces here.
std::vector<std::filesystem::path> SystemUiFontCandidates();

bool SelfTest(); // --test-platform

} // namespace hbe::platform
