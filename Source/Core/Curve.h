// Core/Curve.h - reusable animation-curve subsystem (scalar float channels).
//
// This is the math foundation of the cinematic system's keyframe/graph editor
// (see Source/Cinematics), but it lives in Core and depends only on Core/Types +
// glm so ANY system can animate a scalar property with it (UI, camera, gameplay).
// The cinematic Sequencer never re-implements curve math; it composes these.
//
// A Curve is a sorted list of Keyframes. Each key carries a value, a per-segment
// interpolation mode (the mode on a key governs the interval to its RIGHT, i.e.
// this key -> the next), and tangents (arrive/leave slopes, optionally weighted)
// for cubic segments. Pre/post extrapolation decides behaviour outside the key
// range. Evaluation is deterministic and idempotent, at arbitrary sub-frame time.
//
// The design follows the well-understood "rich curve" model (per-key interp +
// tangent mode + optional weighted tangents), implemented natively here.
#pragma once

#include "Core/Types.h"

#include <vector>

namespace hbe::curve {

// Interpolation of the segment that STARTS at this key (this key -> next key).
// Serialized as an int; append-only.
enum class Interp : u8 {
    Constant = 0, // hold this key's value until the next key (stepped)
    Linear,       // straight line to the next key
    Cubic,        // Hermite/Bezier using leave/arrive tangents
};

// How a key's tangents are determined. Auto keeps a smooth catmull-rom-like slope
// that follows neighbours; User/Break freeze author-edited handles (Break lets the
// arrive and leave slopes differ - a corner); Flat forces zero slope; LinearTan
// aims each handle straight at the neighbour key.
enum class Tangent : u8 {
    Auto = 0,  // smooth, auto-computed from neighbours (both handles equal)
    User,      // author-set, arrive == leave (a unified handle)
    Break,     // author-set, arrive and leave independent (a broken handle)
    Flat,      // zero slope both sides (a hold/peak)
    LinearTan, // handles point straight at the adjacent keys
};

// Which side(s) of a cubic segment use WEIGHTED tangents. Weighted tangents let a
// handle control how far along the segment it pulls (its "length"), not just the
// slope - the difference between a lazy ease and a snappy one. Unweighted segments
// evaluate with the fast Hermite basis; weighted ones solve a cubic Bezier.
enum class WeightMode : u8 {
    None = 0, // both sides unweighted (Hermite)
    Arrive,   // the arriving (right) handle of the segment is weighted
    Leave,    // the leaving (left) handle of the segment is weighted
    Both,
};

// Behaviour when evaluating outside the key range (before the first / after the
// last key). Cycle repeats the range; CycleWithOffset repeats and accumulates the
// end-to-start delta (so a looping walk keeps travelling); Oscillate ping-pongs.
enum class Extrap : u8 {
    Constant = 0,    // hold the edge key's value
    Linear,          // continue along the edge key's tangent
    Cycle,           // repeat the curve
    CycleWithOffset, // repeat, accumulating the value delta across the range
    Oscillate,       // ping-pong
};

// One keyframe. `arriveTangent`/`leaveTangent` are slopes (dValue/dTime). Weights
// are fractions of the segment's time span (0..1), used only in weighted mode.
struct Keyframe {
    f32 time = 0.0f;
    f32 value = 0.0f;
    Interp interp = Interp::Cubic;
    Tangent tangentMode = Tangent::Auto;
    WeightMode weightMode = WeightMode::None;
    f32 arriveTangent = 0.0f; // slope coming INTO this key (from the previous key)
    f32 leaveTangent = 0.0f;  // slope leaving this key (toward the next key)
    f32 arriveWeight = 0.333f; // fraction of the PREVIOUS segment's time span
    f32 leaveWeight = 0.333f;  // fraction of the NEXT segment's time span
};

// A scalar animation channel. `keys` is kept sorted ascending by time. When empty,
// Evaluate returns `defaultValue`.
struct Curve {
    std::vector<Keyframe> keys;
    Extrap preExtrap = Extrap::Constant;
    Extrap postExtrap = Extrap::Constant;
    f32 defaultValue = 0.0f;
};

// -- Evaluation ---------------------------------------------------------------
// Value at absolute time `t`. Deterministic and idempotent. Handles empty curves
// (returns defaultValue), single-key curves (returns that value), extrapolation
// outside the range, and per-segment interpolation inside it.
f32 Evaluate(const Curve& c, f32 t);

// The instantaneous slope (dValue/dt) at `t`, for velocity-driven consumers.
f32 EvaluateDerivative(const Curve& c, f32 t);

// -- Tangents -----------------------------------------------------------------
// Recompute tangents for every key whose tangentMode is auto-derived
// (Auto/Flat/LinearTan). User/Break handles are left untouched. Call after any
// edit that inserts/removes/moves keys.
void RecomputeAutoTangents(Curve& c);
// Recompute just key `i` (and it alone). Used during interactive handle drags.
void RecomputeKeyTangent(Curve& c, usize i);

// -- Editing ------------------------------------------------------------------
// Insert a key at `time` with `value`, keeping the vector sorted; recomputes
// affected auto-tangents. Returns the new key's index. If a key already exists
// within `mergeTol` seconds, it is updated in place instead.
usize Insert(Curve& c, f32 time, f32 value, f32 mergeTol = 1e-4f);
// Insert a key at `time` whose value is sampled from the CURRENT curve, so the
// shape is preserved (a "split" that adds control without changing the result).
usize InsertOnCurve(Curve& c, f32 time);
void Remove(Curve& c, usize i);
// Index of the key nearest `time` within `tol` seconds, or -1.
int FindKey(const Curve& c, f32 time, f32 tol);

// Time-shift every key by `dt` (used to move a whole clip).
void ShiftTime(Curve& c, f32 dt);
// Scale key times about `pivot` (retime); factor <1 compresses, >1 stretches.
void ScaleTime(Curve& c, f32 pivot, f32 factor);
// Offset / scale key values (about `pivot`).
void OffsetValue(Curve& c, f32 dv);
void ScaleValue(Curve& c, f32 pivot, f32 factor);

// -- Curve reduction ----------------------------------------------------------
// Remove interior keys whose removal changes the sampled curve by less than
// `tolerance` (max abs value error), evaluated at `sampleHz`. Greedy: repeatedly
// drops the key with the smallest introduced error until none is below tolerance.
// Returns the number of keys removed. Endpoints are always kept.
usize Reduce(Curve& c, f32 tolerance, f32 sampleHz = 60.0f);

// -- Utility ------------------------------------------------------------------
// Sort keys by time and coalesce duplicates (keeps the last). Call if keys were
// mutated directly (e.g. a drag past a neighbour).
void Sort(Curve& c);
f32 MinTime(const Curve& c);
f32 MaxTime(const Curve& c);
// Value range across the key times (for graph-editor auto-fit).
void ValueRange(const Curve& c, f32& outMin, f32& outMax);

} // namespace hbe::curve
