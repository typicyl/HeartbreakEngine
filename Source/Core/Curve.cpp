// Core/Curve.cpp - implementation of the scalar animation-curve subsystem.
#include "Core/Curve.h"

#include <algorithm>
#include <cmath>

namespace hbe::curve {
namespace {

// Cubic Bezier scalar helpers (used for weighted-tangent segments).
inline f32 Bez(f32 p0, f32 p1, f32 p2, f32 p3, f32 u) {
    const f32 v = 1.0f - u;
    return v * v * v * p0 + 3.0f * v * v * u * p1 + 3.0f * v * u * u * p2 + u * u * u * p3;
}
inline f32 BezDeriv(f32 p0, f32 p1, f32 p2, f32 p3, f32 u) {
    const f32 v = 1.0f - u;
    return 3.0f * v * v * (p1 - p0) + 6.0f * v * u * (p2 - p1) + 3.0f * u * u * (p3 - p2);
}

// Hermite basis over a segment of span `dt`, parameter s in [0,1]. Tangents are
// SLOPES (dValue/dTime); multiplied by dt to become unit-interval tangents.
inline f32 Hermite(f32 p0, f32 mLeave, f32 p1, f32 mArrive, f32 dt, f32 s) {
    const f32 m0 = mLeave * dt;
    const f32 m1 = mArrive * dt;
    const f32 s2 = s * s;
    const f32 s3 = s2 * s;
    const f32 h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const f32 h10 = s3 - 2.0f * s2 + s;
    const f32 h01 = -2.0f * s3 + 3.0f * s2;
    const f32 h11 = s3 - s2;
    return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
}

// True when the segment [a,b] needs the weighted (Bezier) path: a's LEAVE side or
// b's ARRIVE side declares a weight.
inline bool SegmentWeighted(const Keyframe& a, const Keyframe& b) {
    const bool leaveW = (a.weightMode == WeightMode::Leave || a.weightMode == WeightMode::Both);
    const bool arriveW = (b.weightMode == WeightMode::Arrive || b.weightMode == WeightMode::Both);
    return leaveW || arriveW;
}

// Evaluate a weighted cubic segment at absolute time `t` by solving the Bezier's
// X (time) for its parameter u, then reading Y (value). Unweighted sides fall back
// to the Hermite-equivalent 1/3 weight, so a partially-weighted segment is exact.
f32 EvalWeighted(const Keyframe& a, const Keyframe& b, f32 dt, f32 t) {
    const bool leaveW = (a.weightMode == WeightMode::Leave || a.weightMode == WeightMode::Both);
    const bool arriveW = (b.weightMode == WeightMode::Arrive || b.weightMode == WeightMode::Both);
    // Weights are fractions of the segment time span; clamp so control points stay
    // ordered (BezierX monotonic) and the Newton solve is well-conditioned.
    const f32 wL = std::clamp(leaveW ? a.leaveWeight : (1.0f / 3.0f), 0.001f, 0.999f);
    const f32 wR = std::clamp(arriveW ? b.arriveWeight : (1.0f / 3.0f), 0.001f, 0.999f);

    const f32 x0 = a.time, x3 = b.time;
    const f32 x1 = x0 + wL * dt;
    const f32 x2 = x3 - wR * dt;
    const f32 y0 = a.value, y3 = b.value;
    const f32 y1 = y0 + wL * dt * a.leaveTangent;
    const f32 y2 = y3 - wR * dt * b.arriveTangent;

    // Solve BezierX(u) = t. X is monotonically increasing in u (x1>=x0, x2<=x3,
    // x1<=x2 given the clamps), so Newton from the linear guess converges fast;
    // bisection is the safety net.
    f32 u = std::clamp((t - x0) / dt, 0.0f, 1.0f);
    f32 lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 12; ++i) {
        const f32 x = Bez(x0, x1, x2, x3, u);
        const f32 err = x - t;
        if (std::fabs(err) < 1e-6f) break;
        if (err > 0.0f) hi = u; else lo = u;
        const f32 dx = BezDeriv(x0, x1, x2, x3, u);
        f32 next = (std::fabs(dx) > 1e-9f) ? (u - err / dx) : (0.5f * (lo + hi));
        if (!(next > lo && next < hi)) next = 0.5f * (lo + hi); // fell out of bracket
        u = next;
    }
    return Bez(y0, y1, y2, y3, u);
}

// Value inside a single segment [a,b] at absolute time `t` (a.time <= t <= b.time).
f32 EvalSegment(const Keyframe& a, const Keyframe& b, f32 t) {
    const f32 dt = b.time - a.time;
    if (dt <= 0.0f) return b.value;
    const f32 s = (t - a.time) / dt;
    switch (a.interp) {
        // Stepped: hold a's value across the segment, stepping to b exactly at b.time
        // (s == 1). Interior keys reach their own value via the next segment at s == 0;
        // this branch matters at the terminal segment's right endpoint.
        case Interp::Constant: return (s >= 1.0f) ? b.value : a.value;
        case Interp::Linear:   return a.value + (b.value - a.value) * s;
        case Interp::Cubic:
        default:
            if (SegmentWeighted(a, b)) return EvalWeighted(a, b, dt, t);
            return Hermite(a.value, a.leaveTangent, b.value, b.arriveTangent, dt, s);
    }
}

// Index of the last key whose time <= t (t assumed within [front,back]).
usize SegmentIndex(const std::vector<Keyframe>& k, f32 t) {
    usize lo = 0, hi = k.size() - 1;
    while (lo + 1 < hi) {
        const usize mid = (lo + hi) / 2;
        if (k[mid].time <= t) lo = mid; else hi = mid;
    }
    return lo;
}

// Value strictly inside the key range [front.time, back.time].
f32 EvalCore(const Curve& c, f32 t) {
    const auto& k = c.keys;
    const usize i = SegmentIndex(k, t);
    return EvalSegment(k[i], k[i + 1], t);
}

f32 ExtrapolateCyclic(const Curve& c, f32 t, Extrap mode) {
    const f32 t0 = c.keys.front().time, t1 = c.keys.back().time;
    const f32 range = t1 - t0;
    if (range <= 0.0f) return c.keys.front().value;
    const f32 rel = t - t0;
    const f32 q = std::floor(rel / range);
    const f32 mapped = rel - q * range; // [0, range)
    f32 v;
    if (mode == Extrap::Oscillate && (static_cast<long long>(q) & 1LL)) {
        v = EvalCore(c, t1 - mapped); // mirror on odd cycles
    } else {
        v = EvalCore(c, t0 + mapped);
    }
    if (mode == Extrap::CycleWithOffset) {
        v += q * (c.keys.back().value - c.keys.front().value);
    }
    return v;
}

f32 ExtrapolateBefore(const Curve& c, f32 t) {
    const Keyframe& f = c.keys.front();
    switch (c.preExtrap) {
        case Extrap::Constant: return f.value;
        case Extrap::Linear:   return f.value + f.leaveTangent * (t - f.time);
        default:               return ExtrapolateCyclic(c, t, c.preExtrap);
    }
}

f32 ExtrapolateAfter(const Curve& c, f32 t) {
    const Keyframe& l = c.keys.back();
    switch (c.postExtrap) {
        case Extrap::Constant: return l.value;
        case Extrap::Linear:   return l.value + l.arriveTangent * (t - l.time);
        default:               return ExtrapolateCyclic(c, t, c.postExtrap);
    }
}

} // namespace

// ---------------------------------------------------------------------------
f32 Evaluate(const Curve& c, f32 t) {
    const auto& k = c.keys;
    if (k.empty()) return c.defaultValue;
    if (k.size() == 1) return k[0].value;
    if (t < k.front().time) return ExtrapolateBefore(c, t);
    if (t > k.back().time) return ExtrapolateAfter(c, t);
    return EvalCore(c, t);
}

f32 EvaluateDerivative(const Curve& c, f32 t) {
    if (c.keys.size() < 2) return 0.0f;
    // Central difference, scaled to the curve's time span so it is stable for both
    // second-long UI fades and minute-long shots.
    const f32 span = std::max(1e-3f, c.keys.back().time - c.keys.front().time);
    const f32 h = span * 1e-4f;
    return (Evaluate(c, t + h) - Evaluate(c, t - h)) / (2.0f * h);
}

// ---------------------------------------------------------------------------
void RecomputeKeyTangent(Curve& c, usize i) {
    auto& k = c.keys;
    if (i >= k.size()) return;
    Keyframe& key = k[i];
    if (key.tangentMode == Tangent::User || key.tangentMode == Tangent::Break) return;

    if (key.tangentMode == Tangent::Flat) {
        key.arriveTangent = key.leaveTangent = 0.0f;
        return;
    }

    const bool hasPrev = i > 0;
    const bool hasNext = i + 1 < k.size();

    if (key.tangentMode == Tangent::LinearTan) {
        key.arriveTangent = hasPrev ? (key.value - k[i - 1].value) / std::max(1e-6f, key.time - k[i - 1].time)
                                    : 0.0f;
        key.leaveTangent = hasNext ? (k[i + 1].value - key.value) / std::max(1e-6f, k[i + 1].time - key.time)
                                   : 0.0f;
        return;
    }

    // Auto: smooth slope from neighbours, flattened at local extrema so a peak
    // does not overshoot (the sensible default authors expect).
    f32 slope = 0.0f;
    if (hasPrev && hasNext) {
        const f32 dPrev = key.value - k[i - 1].value;
        const f32 dNext = k[i + 1].value - key.value;
        if (dPrev * dNext <= 0.0f) {
            slope = 0.0f; // local extremum -> flatten
        } else {
            slope = (k[i + 1].value - k[i - 1].value) /
                    std::max(1e-6f, k[i + 1].time - k[i - 1].time);
        }
    } else if (hasNext) {
        slope = (k[i + 1].value - key.value) / std::max(1e-6f, k[i + 1].time - key.time);
    } else if (hasPrev) {
        slope = (key.value - k[i - 1].value) / std::max(1e-6f, key.time - k[i - 1].time);
    }
    key.arriveTangent = key.leaveTangent = slope;
}

void RecomputeAutoTangents(Curve& c) {
    for (usize i = 0; i < c.keys.size(); ++i) RecomputeKeyTangent(c, i);
}

// ---------------------------------------------------------------------------
void Sort(Curve& c) {
    std::stable_sort(c.keys.begin(), c.keys.end(),
                     [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    // Coalesce keys that landed on the same time (keep the later one).
    for (usize i = 1; i < c.keys.size();) {
        if (std::fabs(c.keys[i].time - c.keys[i - 1].time) < 1e-6f) {
            c.keys.erase(c.keys.begin() + (i - 1));
        } else {
            ++i;
        }
    }
}

usize Insert(Curve& c, f32 time, f32 value, f32 mergeTol) {
    // Update in place if a key already sits within mergeTol.
    for (usize i = 0; i < c.keys.size(); ++i) {
        if (std::fabs(c.keys[i].time - time) <= mergeTol) {
            c.keys[i].value = value;
            RecomputeKeyTangent(c, i);
            if (i > 0) RecomputeKeyTangent(c, i - 1);
            if (i + 1 < c.keys.size()) RecomputeKeyTangent(c, i + 1);
            return i;
        }
    }
    Keyframe key;
    key.time = time;
    key.value = value;
    usize idx = 0;
    while (idx < c.keys.size() && c.keys[idx].time < time) ++idx;
    c.keys.insert(c.keys.begin() + idx, key);
    RecomputeKeyTangent(c, idx);
    if (idx > 0) RecomputeKeyTangent(c, idx - 1);
    if (idx + 1 < c.keys.size()) RecomputeKeyTangent(c, idx + 1);
    return idx;
}

usize InsertOnCurve(Curve& c, f32 time) {
    // Sample the CURRENT curve so the shape is preserved, and freeze the local
    // slope onto the new key (User) so recomputing neighbours cannot shift it.
    const f32 v = Evaluate(c, time);
    const f32 slope = EvaluateDerivative(c, time);
    usize idx = 0;
    while (idx < c.keys.size() && c.keys[idx].time < time) ++idx;
    Keyframe key;
    key.time = time;
    key.value = v;
    key.tangentMode = Tangent::User;
    key.arriveTangent = key.leaveTangent = slope;
    c.keys.insert(c.keys.begin() + idx, key);
    return idx;
}

void Remove(Curve& c, usize i) {
    if (i >= c.keys.size()) return;
    c.keys.erase(c.keys.begin() + i);
    if (i > 0) RecomputeKeyTangent(c, i - 1);
    if (i < c.keys.size()) RecomputeKeyTangent(c, i);
}

int FindKey(const Curve& c, f32 time, f32 tol) {
    int best = -1;
    f32 bestD = tol;
    for (usize i = 0; i < c.keys.size(); ++i) {
        const f32 d = std::fabs(c.keys[i].time - time);
        if (d <= bestD) { bestD = d; best = static_cast<int>(i); }
    }
    return best;
}

void ShiftTime(Curve& c, f32 dt) {
    for (auto& k : c.keys) k.time += dt;
}

void ScaleTime(Curve& c, f32 pivot, f32 factor) {
    for (auto& k : c.keys) k.time = pivot + (k.time - pivot) * factor;
    // Tangents are slopes (dValue/dTime); a time scale inversely scales them.
    if (std::fabs(factor) > 1e-6f) {
        const f32 inv = 1.0f / factor;
        for (auto& k : c.keys) { k.arriveTangent *= inv; k.leaveTangent *= inv; }
    }
}

void OffsetValue(Curve& c, f32 dv) {
    for (auto& k : c.keys) k.value += dv;
}

void ScaleValue(Curve& c, f32 pivot, f32 factor) {
    for (auto& k : c.keys) {
        k.value = pivot + (k.value - pivot) * factor;
        k.arriveTangent *= factor;
        k.leaveTangent *= factor;
    }
}

// ---------------------------------------------------------------------------
usize Reduce(Curve& c, f32 tolerance, f32 sampleHz) {
    if (c.keys.size() <= 2) return 0;
    const f32 step = (sampleHz > 0.0f) ? (1.0f / sampleHz) : (1.0f / 60.0f);
    usize removed = 0;

    for (;;) {
        if (c.keys.size() <= 2) break;
        f32 bestErr = tolerance;
        int bestIdx = -1;

        for (usize i = 1; i + 1 < c.keys.size(); ++i) {
            // Build the curve with key i removed and its neighbours re-smoothed.
            Curve trial = c;
            trial.keys.erase(trial.keys.begin() + i);
            RecomputeKeyTangent(trial, i - 1);
            if (i < trial.keys.size()) RecomputeKeyTangent(trial, i);

            const f32 a = c.keys[i - 1].time, b = c.keys[i + 1].time;
            f32 err = 0.0f;
            for (f32 t = a; t <= b; t += step) {
                err = std::max(err, std::fabs(Evaluate(c, t) - Evaluate(trial, t)));
                if (err >= bestErr) break;
            }
            if (err < bestErr) { bestErr = err; bestIdx = static_cast<int>(i); }
        }

        if (bestIdx < 0) break;
        c.keys.erase(c.keys.begin() + bestIdx);
        RecomputeKeyTangent(c, bestIdx - 1);
        if (static_cast<usize>(bestIdx) < c.keys.size()) RecomputeKeyTangent(c, bestIdx);
        ++removed;
    }
    return removed;
}

// ---------------------------------------------------------------------------
f32 MinTime(const Curve& c) { return c.keys.empty() ? 0.0f : c.keys.front().time; }
f32 MaxTime(const Curve& c) { return c.keys.empty() ? 0.0f : c.keys.back().time; }

void ValueRange(const Curve& c, f32& outMin, f32& outMax) {
    if (c.keys.empty()) { outMin = outMax = c.defaultValue; return; }
    outMin = outMax = c.keys.front().value;
    for (const auto& k : c.keys) {
        outMin = std::min(outMin, k.value);
        outMax = std::max(outMax, k.value);
    }
}

} // namespace hbe::curve
