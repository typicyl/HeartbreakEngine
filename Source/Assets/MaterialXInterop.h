// Assets/MaterialXInterop.h - MaterialX (.mtlx) <-> .hbmat interchange. EDITOR / IMPORT-TIME ONLY.
//
// MaterialX is the INTERCHANGE layer: it is parsed at authoring/import time to move OpenPBR materials
// in and out of the DCC ecosystem, and is NEVER a runtime graph evaluator. Only MaterialXCore +
// MaterialXFormat are linked, and only into hbe_editor (HBE_HAS_MATERIALX) - the shipped runtime
// links none of it. Because SurfaceParams already uses the OpenPBR Surface parameter names, the
// mapping to/from an `open_pbr_surface` node is almost 1:1 (see RHI/SurfaceMaterial.h).
//
// When the build did NOT link MaterialX (HBE_HAS_MATERIALX undefined), MaterialXAvailable() returns
// false and Import/Export fail with a "not built" message - the .hbmat authoring path is unaffected.
#pragma once

#include "Assets/MaterialAsset.h"

#include <filesystem>
#include <optional>
#include <string>

namespace hbe::assets {

// True when this build linked MaterialXCore + MaterialXFormat.
bool MaterialXAvailable();

// Parses `mtlx`, finds the first surface-shader node (open_pbr_surface preferred, standard_surface
// mapped as a fallback), and maps its inputs to a MaterialAsset. Value inputs are read directly;
// base-colour / emissive inputs connected to an <image> node contribute a source texture path in the
// returned asset's *Tex fields (the caller runs the normal texture import on them). Returns nullopt on
// failure or when MaterialX is not built (message in `error`).
std::optional<MaterialAsset> ImportMaterialX(const std::filesystem::path& mtlx, std::string& error);

// Writes `mat` as a .mtlx document: one `open_pbr_surface` node (its OpenPBR inputs set from
// SurfaceParams) wired into a `surfacematerial`. Returns false on failure or when MaterialX is not
// built (message in `error`).
bool ExportMaterialX(const MaterialAsset& mat, const std::filesystem::path& mtlx, std::string& error);

// --test-openpbr: headless self-test of the material interchange. Round-trips a distinctive OpenPBR
// material through `.hbmat` (Save/Load) and, when MaterialX is linked, through `.mtlx` (Export/Import),
// and checks the shader-variant routing. When MaterialX is built this also proves the DLLs load and
// execute. Writes/removes scratch files under the OS temp dir; no GPU/window/project. Returns true on
// pass (logs each failure).
bool MaterialInteropSelfTest();

} // namespace hbe::assets
