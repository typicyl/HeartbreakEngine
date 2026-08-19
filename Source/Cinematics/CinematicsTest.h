// Cinematics/CinematicsTest.h - headless self-tests for the cinematic system.
#pragma once

namespace hbe::cine {

// Curve engine math (Core/Curve): interpolation, tangents, extrapolation, reduction.
bool CurveSelfTest();
// Sequence model + serialization round-trip + registry + deterministic evaluation.
bool SelfTest();

} // namespace hbe::cine
