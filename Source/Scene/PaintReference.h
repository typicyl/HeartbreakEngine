// Scene/PaintReference.h - render a GROUND-TRUTH Heartbreak painting, headless.
//
// This is an INVESTIGATION tool, not a renderer feature. It drives the real
// painting code (paint::Stroke -> BakeFromStrokes -> Flatten) with synthetic
// stroke paths and writes the resulting canvas to PNGs, so "what does a
// Heartbreak brush stroke actually look like" can be answered by looking at the
// output of the actual system rather than by reading its source and guessing.
//
// It exists because the procedural painterly renderer has to be judged against
// this, not against an idea of painterliness: same brushes, same profiles, same
// deposition, same relief. Anything the procedural system produces that does not
// share this visual structure is wrong regardless of how it scores on stability.
//
// Headless: no GPU, no window, no Project. Same contract as --test-seamweld.
#pragma once

#include <filesystem>
#include <string>

namespace hbe::paint {

// Paints a set of reference swatches - one per built-in brush - into `outDir`:
//   ref_<brush>_color.png   the flattened albedo canvas (what the eye reads)
//   ref_<brush>_relief.png  the relief/height channel (what rakes the light)
// Returns false if nothing could be written.
bool WriteReferenceSwatches(const std::filesystem::path& outDir, std::string& report);

} // namespace hbe::paint
