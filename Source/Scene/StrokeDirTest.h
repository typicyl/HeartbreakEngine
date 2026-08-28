// Scene/StrokeDirTest.h - stroke arrangement under a VARYING direction field.
//
// Separate from --test-procpaint on purpose. That test pins the direction so the
// GPU evaluator can be diffed against the CPU painting oracle - it is the
// SEMANTIC correctness gate and must not change. This one asks the different
// question the oracle cannot: with direction varying across a surface, do the
// strokes still read as individual brush gestures, or as procedural hatching?
//
// Renders the bare stroke BODY only - no bristles, no grain, no canvas, no
// domain warp - onto a flat plane, a sphere, and a box, plus a direction debug
// view (RGB = tangent direction) so "the flow field is bad" can be told apart
// from "the stroke rendering is bad". Also measures direction continuity.
//
// Headless: no GPU, no window, no Project. Same contract as --test-seamweld.
#pragma once

#include <filesystem>
#include <string>

namespace hbe::procpaint {

bool DirectionSelfTest(const std::filesystem::path& outDir, std::string& report);

} // namespace hbe::procpaint
