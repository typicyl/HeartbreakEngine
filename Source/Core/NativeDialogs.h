// Core/NativeDialogs.h - the native desktop GUI services an EDITOR needs, behind one
// interface with no OS types in sight.
//
// WHY SEPARATE FROM Core/Platform.h. Platform.h is the small filesystem/process layer that
// the shipped runtime AND the launcher link. These functions are different: they exist only
// for authoring tools, and their Win32 backend drags in COM (ole32), the common-dialog
// library (comdlg32) and GDI. Folding them into Platform_Win32.cpp would force every shipped
// game exe - and the engine-free Hub launcher - to link those import libraries for code they
// never call. So this is its own translation unit, added only to the editor library, exactly
// the way the RHI keeps each backend in its own file.
//
// NO WIN32 IN THE HEADER. The old code had the opposite: BuildImportDialogFilter() lived in
// the PORTABLE Assets/AssetFormats.h and returned a std::wstring that was secretly a Win32
// "Label\0patterns\0...\0\0" double-NUL block - a Win32 file format leaking into a header
// with nothing else Windows about it. Here the filter is an ordinary list of {label,
// extensions} (Core/FileFilter.h); the backend is the only place that knows Win32 wants it
// double-NUL-packed.
#pragma once

#include "Core/FileFilter.h" // platform::FileFilter (the neutral type OpenFileDialog takes)
#include "Core/Types.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace hbe::platform {

// Show the OS "open an existing file" dialog and return the chosen path, or nullopt if the
// user cancelled. `filters` is offered in order; the first is the default selection. The
// dialog must NOT change the process's current directory (several asset paths in this engine
// are resolved relative to it) and only accepts a path to a file that exists.
std::optional<std::filesystem::path> OpenFileDialog(const std::vector<FileFilter>& filters);

// Show the OS "pick a folder" dialog and return the chosen directory, or nullopt if
// cancelled. Uses the modern shell folder picker where the OS has one.
std::optional<std::filesystem::path> PickFolderDialog();

// Sample the colour of the pixel currently under the mouse cursor, as it appears on the
// desktop right now (the composited, lit, painted result the user sees) - this is the
// editor's eyedropper. Writes 0..255 R/G/B and returns true on success; returns false (and
// leaves the outputs untouched) if the OS declines. A future portable backend would sample
// the engine's own framebuffer instead of the desktop, which is strictly better but needs a
// GPU readback path; the desktop scrape is what the Win32 backend has always done.
bool SampleDesktopColorAtCursor(u8& r, u8& g, u8& b);

} // namespace hbe::platform
