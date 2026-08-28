// Scene/ProcPaintTest.h - the procedural-paint ORACLE COMPARISON.
//
// One procedural stroke set, realized two ways and diffed:
//
//   CPU (the oracle)   ProcGetStroke -> paint::Stroke -> BakeFromStrokes ->
//                      Flatten          i.e. the REAL painting system
//   GPU (the runtime)  ProcGetStroke -> ProcPaintAtFrame, the analytic evaluator
//                      that MeshPBR will call
//
// Both build strokes from the SAME ProcGetStroke, so any difference the test
// reports is a real disagreement about the paint model, not two generators
// drifting apart. This is the pattern --test-oceanfft already uses, where a CPU
// reference is "the oracle the GPU compute FFT is later diffed against".
//
// Bit-exactness is NOT the goal: the CPU path rasterizes overlapping dabs while
// the GPU path evaluates a profile analytically, so they differ at edges by
// construction. What must agree is the SEMANTICS and the broad visual structure -
// stroke width, taper, occupancy, gaps, overlap, deposition.
//
// Headless: no GPU, no window, no Project. Same contract as --test-seamweld.
#pragma once

#include <filesystem>
#include <string>

namespace hbe::procpaint {

// Runs the comparison. Writes side-by-side PNGs (and per-channel debug views)
// into `outDir` when it is non-empty. `report` gets a one-line summary; the full
// table goes to stdout.
bool OracleSelfTest(const std::filesystem::path& outDir, std::string& report);

} // namespace hbe::procpaint
