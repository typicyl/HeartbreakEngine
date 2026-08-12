// UI/Svg/SvgCache.h - on-demand SVG rasterization for UI vector assets.
//
// An `.svg` referenced by a UIElement stays resolution-independent: it is parsed
// once and RASTERIZED ON DEMAND at the pixel size the element is drawn, then cached
// as a bindless texture - never baked to a single bitmap at import (which would
// defeat the point). This header is free of the LunaSVG type (only SvgCache.cpp
// includes <lunasvg.h>), so the vector dependency never leaks into the include graph
// and main_editor.cpp can call the self-test without pulling LunaSVG in.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>

namespace hbe {
class Renderer;
}

namespace hbe::ui::svg {

// True if `path` names an SVG asset (case-insensitive ".svg" suffix).
bool IsSvgPath(const std::string& path);

// Rasterizes the `.svg` at `rel` (Assets-relative) to `pxW`x`pxH` device pixels and
// uploads it into the bindless table, returning the texture index (0 = missing or
// failed). The raw file bytes are cached per path and the rasterized result per
// (path,pxW,pxH), so a redraw at the same size is a hash lookup and the texture is
// uploaded once. Reads through the VFS, so it works out of a mounted `.uap` in a
// shipped build.
u32 ResolveSvg(Renderer& renderer, const std::filesystem::path& assetsDir,
               const std::string& rel, u32 pxW, u32 pxH);

// Drops cached documents + rasterized indices (project switch / re-import). The
// bindless slots of freed rasters are intentionally leaked (engine-wide no-free
// texture policy, same as UITexCache).
void ClearSvgCache();

// Headless self-test (--test-uisvg): parse + rasterize an embedded SVG at two sizes,
// asserting non-empty coverage, correct dimensions and RGBA byte order. No GPU.
bool SvgSelfTest();

} // namespace hbe::ui::svg
