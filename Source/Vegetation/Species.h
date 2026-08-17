// Vegetation/Species.h - the data-driven species / archetype / biome model.
//
// NOTHING about a species is hardcoded in C++. Every botanical and ecological
// parameter lives in these POD records, authored in a .hbspecies / .hbbiome asset
// (loaded in P3) and interned into the SpeciesRegistry / BiomeRegistry. Adding a new
// tree is authoring data, never a code change.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Core/Types.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace hbe::veg {

// Which structural generator a species is grown with. Selects an IPlantGenerator
// backend BY NAME from the VegetationWorld factory, so a library backend can be
// slotted in later without changing this enum's meaning.
enum class GenStrategy : u8 {
    SpaceColonization = 0, // natural, incremental branching (also the growth model)
    LSystem,               // rule-based branching
    Custom,                // a data-scriptable in-house generator
    External,              // an offline import (proctree / Blender), replayed
    Count
};

// A SPECIES: the full parameter set a generator + mesher + distribution reads.
// Defaults describe a generic mid-size broadleaf so a bare record still produces a
// plausible tree.
struct Species {
    std::string name = "species";
    GenStrategy strategy = GenStrategy::SpaceColonization;

    // --- Growth / structure ---
    f32 maxHeight        = 12.0f;  // m at maturity
    f32 trunkRadius      = 0.25f;  // m, base radius at maturity
    f32 taper            = 0.85f;  // child/parent radius ratio per branch order
    u8  maxBranchOrder   = 4;      // trunk(0) .. twig(N)
    f32 branchAngleDeg   = 45.0f;  // mean branching angle off the parent
    f32 branchAngleJitter= 12.0f;  // +/- deg per branch
    f32 branchDensity    = 0.6f;   // 0..1 relative count of child branches
    f32 crownWidth       = 6.0f;   // m, canopy diameter (space-colonization envelope)
    f32 growthRateMPerYr = 1.0f;   // m/year (growth sim)
    f32 lifespanYears    = 120.0f;

    // --- Foliage ---
    f32 leafSize     = 0.12f;      // m
    f32 leafDensity  = 1.0f;       // 0..1 relative leaves per twig / cluster
    glm::vec4 leafColor   {0.28f, 0.50f, 0.18f, 1.0f};
    glm::vec4 leafColorVar{0.05f, 0.06f, 0.04f, 0.0f}; // per-instance tint jitter
    glm::vec4 barkColor   {0.30f, 0.22f, 0.15f, 1.0f};

    // --- Environment preferences (scored by biome distribution; see IVegetationDistribution) ---
    glm::vec2 altitudeRange    {-1000.0f, 2000.0f}; // preferred world-Y band (m)
    glm::vec2 slopeToleranceDeg{0.0f, 35.0f};       // ground slope it will grow on
    glm::vec2 moistureRange    {0.0f, 1.0f};        // preferred soil moisture 0..1
    glm::vec2 temperatureRange {-10.0f, 40.0f};     // preferred temperature (C)
    f32 sunlightRequirement = 0.6f;                 // 0 shade-tolerant .. 1 full-sun

    // --- Dynamics ---
    f32 windResistance  = 0.5f;    // 0 flimsy .. 1 stiff (scales sway amplitude)
    f32 breakResistance = 0.6f;    // 0 fragile .. 1 tough (branch/trunk breakage)
    f32 regenRate       = 0.3f;    // 0..1 regrowth speed after damage

    // --- Seasonal ---
    bool deciduous = true;         // sheds leaves seasonally
    bool flowers   = false;
    bool fruits    = false;

    // --- Asset references (resolved on load; P3). Kept as literal strings so the pack
    //     closure can walk them - a runtime-concatenated name is invisible to it. ---
    std::string barkMaterial;      // .hbmat
    std::string leafMaterial;      // .hbmat
    std::string authoredMesh;      // optional .uaf override (proctree / Blender import)
};

// A parameterized VARIANT of a species: one species can present several silhouettes
// (young/old, storm-bent) without duplicating the whole record.
struct Archetype {
    SpeciesId   species;
    std::string name = "default";
    f32 ageBias = 1.0f;               // multiplies age01 at generation
    glm::vec2 scaleRange{0.9f, 1.1f}; // per-instance uniform scale jitter
    u64 seedSalt = 0;                 // folded into the per-instance seed
};

// One species' abundance rule inside a biome.
struct BiomeSpeciesRule {
    SpeciesId species;
    f32 weight       = 1.0f;   // relative abundance among the biome's species
    f32 densityMul   = 1.0f;   // scatter-density multiplier for this species
    i32 requireSplatLayer = -1; // -1 = any; else only where terrain splat layer L dominates
};

// A BIOME is a RULE, not a spatial field. HB terrain exposes no biome/moisture/
// temperature map - only a dominant splat layer + slope + altitude + water proximity
// (see the terrain audit) - so a biome scores those into a weighted species set. A
// scene can overlap several biomes; the distribution picks per candidate point.
struct Biome {
    std::string name = "biome";
    glm::vec2 altitudeRange{-1000.0f, 3000.0f};
    glm::vec2 slopeRangeDeg {0.0f, 40.0f};
    glm::vec2 moistureRange {0.0f, 1.0f};
    f32 baseDensity   = 0.05f; // plants per m^2 before per-species multipliers
    f32 waterExclusion = 0.5f; // required height above the water surface (m)
    std::vector<BiomeSpeciesRule> species;
};

} // namespace hbe::veg
