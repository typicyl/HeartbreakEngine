// Core/FileFilter.h - a neutral vocabulary type: a file-dialog type filter.
//
// It lives in its own tiny header, apart from Core/NativeDialogs.h, on purpose. The asset
// registry (Assets/AssetFormats.h) PRODUCES filters from the source-format table, and the
// native dialog backend CONSUMES them; both are runtime-agnostic about each other. If this
// struct lived in NativeDialogs.h, every runtime translation unit that includes AssetFormats.h
// would transitively pull in a header whose banner reads "native desktop GUI services an
// EDITOR needs" - a false coupling. Keeping the shared type here lets the producer and the
// consumer each depend only on a neutral Core vocabulary type, not on one another.
//
// There is nothing OS-specific here, by design.
#pragma once

#include <string>
#include <vector>

namespace hbe::platform {

// One entry in an open-dialog's type filter. `extensions` are bare, lower-case, dotless
// tokens ("png", "hbproj"); a dialog backend renders them into whatever pattern syntax the OS
// wants ("*.png;*.jpg"). An empty `extensions` means "everything". A backend is expected to
// append its own "All files" entry, so producers never add one.
struct FileFilter {
    std::string label;
    std::vector<std::string> extensions;
};

} // namespace hbe::platform
