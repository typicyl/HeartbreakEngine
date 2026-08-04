// Core/Rng.h - the deterministic random source. Same seed, same bytes, everywhere.
//
// WHY THIS EXISTS RATHER THAN <random>. A procedural generator promises "the same seed
// produces the same human". `std::mt19937` gives identical bit streams across
// implementations, but the DISTRIBUTIONS do not: `std::uniform_real_distribution` and
// `std::normal_distribution` are unspecified in the standard and genuinely differ between
// libstdc++, libc++ and the MSVC STL. A generator built on them reproduces on one toolchain
// and quietly diverges on another - and it would diverge between a developer's build and a
// shipped one, which is the worst place to find out.
//
// So the stream AND the mapping to floats live here, in about forty lines, with no
// dependency on the standard library's unspecified parts.
//
// THREE RULES this type exists to enforce:
//
//   1. NO GLOBAL STATE. Every stream is an explicit object. A shared generator means the
//      order in which callers happen to run decides the result, and the moment any of that
//      work moves into a job the output changes with the CPU's core count.
//   2. STREAMS ARE SPLIT, NOT SHARED. `Split(salt)` derives an independent child stream, so
//      "the fat noise" and "the asymmetry" can be drawn in either order, or in parallel,
//      without disturbing each other. Adding a new consumer must not renumber the old ones.
//   3. INTEGER SEEDS ONLY. A float seed is a category error: two bit patterns can compare
//      equal, and denormals and NaNs do not round-trip.
#pragma once

#include "Core/Types.h"

#include <cstring>

namespace hbe {

// SplitMix64. Chosen because it is 64 bits of state, passes BigCrush, needs no warm-up
// (unlike Mersenne Twister, whose first outputs after a small seed are poor), and is short
// enough to be obviously correct - which matters when a whole content pipeline is keyed on
// it reproducing exactly.
class Rng {
public:
    Rng() = default;
    explicit Rng(u64 seed) : state_(seed) {}

    u64 NextU64() {
        u64 z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    u32 NextU32() { return static_cast<u32>(NextU64() >> 32); }

    // [0,1). Built by hand from the top 24 bits, NOT by dividing a random integer by its
    // maximum: 24 bits is exactly float's mantissa, so every value is representable, the
    // spacing is uniform, and 1.0f can never come out (which would break any caller doing
    // `array[int(f * n)]`).
    f32 NextFloat() { return static_cast<f32>(NextU64() >> 40) * (1.0f / 16777216.0f); }

    f32 Range(f32 lo, f32 hi) { return lo + (hi - lo) * NextFloat(); }

    // Symmetric bipolar noise, the shape most generator parameters actually want.
    f32 Signed() { return NextFloat() * 2.0f - 1.0f; }

    // Approximately normal, via the central limit theorem on four samples. Cheap, bounded
    // (no tail past +/-2), and - unlike Box-Muller - it cannot produce an infinity from an
    // unlucky zero. Bounded is a FEATURE here: a body parameter must never be handed a
    // six-sigma outlier because one seed was unusual.
    f32 Gaussian() {
        return (NextFloat() + NextFloat() + NextFloat() + NextFloat() - 2.0f);
    }

    // An INDEPENDENT child stream. This is what keeps a generator stable while it grows:
    // give each subsystem its own salt and they can draw in any order, on any thread, in
    // any number, without shifting each other's output.
    Rng Split(u64 salt) const {
        u64 z = state_ ^ (salt * 0xD6E8FEB86659FD93ull);
        z = (z ^ (z >> 32)) * 0xD6E8FEB86659FD93ull;
        z = (z ^ (z >> 32)) * 0xD6E8FEB86659FD93ull;
        return Rng(z ^ (z >> 32));
    }

    u64 State() const { return state_; }

private:
    u64 state_ = 0x853C49E6748FEA9Bull; // a default seed, never 0
};

// The content hash. Integer-only, over raw bit patterns, in an order the CALLER fixes -
// never over a container's iteration order, and never over a float comparison. This is what
// a bake cache is keyed on, so it has to mean "the same inputs" exactly.
class Hasher {
public:
    void Mix(u64 v) {
        h_ ^= v + 0x9E3779B97F4A7C15ull + (h_ << 6) + (h_ >> 2);
    }
    void Mix(u32 v) { Mix(static_cast<u64>(v)); }
    void Mix(i32 v) { Mix(static_cast<u64>(static_cast<u32>(v))); }
    // Hashed by BIT PATTERN. Deliberately not by value: two floats that compare equal can
    // differ in bits (-0.0 vs 0.0), and a NaN is not equal to itself.
    void Mix(f32 v) {
        u32 bits = 0;
        static_assert(sizeof(bits) == sizeof(v));
        std::memcpy(&bits, &v, sizeof(bits));
        // Normalise the one bit pattern that is a real duplicate: -0.0 and 0.0 compare
        // equal and mean the same parameter, so they must hash the same.
        if (bits == 0x80000000u) bits = 0u;
        Mix(bits);
    }
    void Mix(const void* data, usize bytes) {
        const u8* p = static_cast<const u8*>(data);
        for (usize i = 0; i < bytes; ++i) Mix(static_cast<u32>(p[i]));
    }
    u64 Value() const { return h_; }

private:
    u64 h_ = 0xCBF29CE484222325ull;
};

} // namespace hbe
