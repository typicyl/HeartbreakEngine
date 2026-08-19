// Material/MaterialAuthoringTest.h - headless self-test for the unified material-authoring core.
//
// Covers (per the request's test list): material-graph serialization + loading, node compilation,
// DETERMINISTIC compilation, parameter overrides, layer blending, height blending, normal blending,
// box-brush weight evaluation, box rotation, box falloff, world-space + local-space tiling, mask
// serialization, and cross-platform (little-endian) serialization stability. Runs with no GPU / no
// window / no project, wired as `--test-material` in main_editor.cpp.
#pragma once

namespace hbe::mat {

// Returns true iff every material-authoring invariant holds. Logs the first failure of each block.
bool SelfTest();

} // namespace hbe::mat
