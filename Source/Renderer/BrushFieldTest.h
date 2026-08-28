// Renderer/BrushFieldTest.h - headless proof of the procedural brush field.
//
// Runs the REAL shader code: Shaders/BrushField.hlsli is compiled as C++ here
// (see its __cplusplus shim), so these are properties of the shipped field, not
// of a hand-written CPU lookalike that could drift from it.
//
// Covers, in order:
//   1. hash uniformity + decorrelation at world-scale coordinates (the failure
//      that killed the previous frac()-based hash),
//   2. non-repetition: autocorrelation of the anisotropic fBm must decay and
//      stay decayed (|rho| < 0.25 past 4x the correlation length),
//   3. the DIRECTION field on representative geometry - floor, wall, ceiling,
//      sphere, curved terrain, cylinder, and a hard normal discontinuity -
//      checking for sudden flips, noisy orientation, and how much surface area
//      falls into the F-parallel-to-N degenerate case,
//   4. determinism (same inputs -> bit-identical outputs).
//
// Headless: no GPU, no window, no Project. Same contract as --test-seamweld.
#pragma once

#include <string>

namespace hbe::brushfield {

// Runs every check. `report` gets a one-line human summary either way; the full
// per-case table is printed to stdout as it runs.
bool SelfTest(std::string& report);

} // namespace hbe::brushfield
