// Human/HumanParameters.h - everything a human IS, before any geometry exists.
//
// This is the authored surface of HeartbreakHuman: the complete set of knobs, plus a seed.
// Nothing else is an input to generation. If a human cannot be reproduced from one of these
// plus the generator version, something is reading state it should not.
//
// FOUR RULES ENCODED HERE, each of which is a bug that would otherwise surface late:
//
//  1. EVERY FIELD HAS A NEUTRAL DEFAULT. A .hbhuman written today must load unchanged under
//     a wider struct tomorrow. Defaults are "the archetype", not zero - `height = 1.75`, not
//     `height = 0`, because a missing key must mean "unspecified", never "collapse".
//  2. THE SEED IS AN INTEGER. A float seed is a category error: two bit patterns can compare
//     equal, denormals do not round-trip, and NaN is not equal to itself.
//  3. NORMALISED KNOBS ARE 0..1 WITH 0.5 NEUTRAL, not -1..1. Everything an artist adjusts
//     lands in one range, and a serialized 0 is distinguishable from "unset".
//  4. THE HASH IS INTEGER-ONLY, OVER A FIXED FIELD ORDER. It is the bake cache key. It must
//     never depend on float equality, container iteration order, or struct padding.
//
// Units are SI and explicit. `height` is metres, `weight` is kilograms - not normalised -
// because those two are measurements a person has in mind before they open the tool, and
// because the anatomy needs real units to compute real volumes.
#pragma once

#include "Core/Types.h"

#include <string>

namespace hbe::human {

// Bumped whenever the GENERATOR'S OUTPUT changes for identical inputs. It is part of every
// cache key: a code change must invalidate every bake, or a stale artifact is served
// silently and forever. (The engine's .hbgi cache has no such key and does exactly that.)
inline constexpr u32 kGeneratorVersion = 1;

// Regions are the vocabulary the whole system shares: muscles attach to them, fat is
// distributed over them, the surface is tagged with them, and the exporter partitions by
// them. Anything that wants to talk about "the upper arm" says so with one of these.
//
// Deliberately anatomical rather than mesh-shaped: a region is a fact about the body, not
// about how it happens to be cut up for rendering. The runtime partitioning is DERIVED from
// these at export, never the other way round.
enum class Region : u8 {
    Pelvis, Abdomen, Chest, UpperBack, Neck, Head,
    ShoulderL, UpperArmL, ForearmL, HandL,
    ShoulderR, UpperArmR, ForearmR, HandR,
    ThighL, CalfL, FootL,
    ThighR, CalfR, FootR,
    Count
};

const char* RegionName(Region r);
// True when the region belongs to the left/right limb sets, used by the mirroring rules.
bool RegionIsLeft(Region r);
Region MirrorRegion(Region r);

// Proportions, as multipliers around 1.0 rather than 0..1 knobs: these scale real measured
// lengths, so "1.05" means "five percent longer than the anthropometric norm" and stays
// meaningful when the norms are refined later.
struct BodyProportions {
    f32 legLength = 1.0f;      // hip -> ankle, as a fraction of the stature-derived norm
    f32 torsoLength = 1.0f;    // pelvis -> shoulder
    f32 armLength = 1.0f;
    f32 neckLength = 1.0f;
    f32 shoulderWidth = 1.0f;
    f32 hipWidth = 1.0f;
    f32 ribcageDepth = 1.0f;
    f32 handSize = 1.0f;
    f32 footSize = 1.0f;
    f32 headSize = 1.0f;
};

// Per-region multipliers on top of the global `muscleMass`. This is what makes "big arms,
// ordinary legs" expressible - a single global slider can only ever produce one body type.
struct MuscleTuning {
    f32 shoulders = 1.0f, chest = 1.0f, back = 1.0f, arms = 1.0f;
    f32 forearms = 1.0f, abdomen = 1.0f, glutes = 1.0f, thighs = 1.0f, calves = 1.0f;
    f32 neck = 1.0f;
};

// Where the fat goes, independently of how much there is. Two people at the same BMI
// carrying it differently is most of what makes them look like different people, so this is
// NOT derivable from `weight` and must be its own set of knobs.
struct FatTuning {
    f32 abdomen = 1.0f, hips = 1.0f, glutes = 1.0f, chest = 1.0f;
    f32 arms = 1.0f, thighs = 1.0f, calves = 1.0f, back = 1.0f;
    f32 submental = 1.0f; // under the jaw - the first place age and weight show
    f32 face = 1.0f;
};

// Skull and soft-tissue proportions. Stage 1 shapes the CRANIUM AND JAW ONLY; the finer
// features are declared here so the parameter file and the UI do not have to change shape
// when the facial anatomy lands. Fields marked (later) are parsed, saved and shown, and
// currently affect nothing - which is stated in the tooltip rather than hidden.
struct FaceParameters {
    f32 skullLength = 1.0f, skullWidth = 1.0f, faceLength = 1.0f;
    f32 jawWidth = 1.0f, jawAngle = 0.5f, chinProjection = 0.5f;
    f32 cheekbone = 0.5f;      // (later)
    f32 browRidge = 0.5f;      // (later)
    f32 eyeSpacing = 0.5f;     // (later)
    f32 eyeSize = 0.5f;        // (later)
    f32 noseLength = 0.5f;     // (later)
    f32 noseWidth = 0.5f;      // (later)
    f32 noseProjection = 0.5f; // (later)
    f32 lipFullness = 0.5f;    // (later)
    f32 earSize = 0.5f;        // (later)
};

struct HumanParameters {
    std::string name = "Unnamed";
    u64 seed = 1;

    // --- the five that describe a person in one sentence ---
    f32 height = 1.75f;      // metres
    f32 weight = 72.0f;      // kilograms
    f32 muscleMass = 0.5f;   // 0..1; 0.5 = untrained-average, 1 = heavily trained
    f32 bodyFat = 0.5f;      // 0..1; drives fat DEPTH (weight drives overall mass)
    f32 age = 30.0f;         // years
    f32 dimorphism = 0.5f;   // 0 feminine .. 1 masculine, continuous, never a mode switch

    BodyProportions body;
    MuscleTuning muscle;
    FatTuning fat;
    FaceParameters face;

    // Seeded left/right divergence. 0 = perfectly symmetric, which no real body is - but it
    // must be the default so that a generated human is reproducible and comparable before
    // anyone opts into asymmetry.
    f32 asymmetry = 0.0f;

    // FIXED FIELD ORDER, integer-only, over bit patterns. Changing the order or adding a
    // field in the middle changes every hash, which is correct: it means "these are
    // different inputs". Append new fields at the END and bump kGeneratorVersion.
    u64 ContentHash() const;
};

// Clamps every field into the range the generator is defined over. Called before generation
// rather than on edit, so a slider can be dragged past a soft limit in the UI and the
// generator still cannot be handed a body with a negative femur.
void Sanitize(HumanParameters& p);

} // namespace hbe::human
