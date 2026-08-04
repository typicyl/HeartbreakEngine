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
// filesystem-and-process layer, which is the part that pays for itself immediately.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>

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

bool SelfTest(); // --test-platform

} // namespace hbe::platform
