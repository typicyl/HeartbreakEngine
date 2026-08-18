// Vegetation/VegetationBackends.h - registration of the built-in vegetation backends.
//
// P1 ships small, in-house, deterministic backends behind every interface so the
// factory wiring is exercised end to end and the self-test has real behaviour to pin.
// Later phases add first-class backends (space colonization / L-system generators in
// P4, FastNoise2 noise in P2, hierarchical GPU wind in P7) and, optionally, wrapped
// LIBRARY backends - registered exactly the same way, which is the whole point of the
// interface seam.
#pragma once

#include <memory>

namespace hbe::veg {

class VegetationWorld;
class IVegetationDistribution;
class INoiseField;
class IPlantGenerator;

// Registers the built-in backends into `world` (called from its constructor).
void RegisterBuiltinBackends(VegetationWorld& world);

// Makers for the larger backends that live in their own translation units.
std::unique_ptr<IVegetationDistribution> MakePoissonDiscDistribution(); // "poisson"
std::unique_ptr<IPlantGenerator> MakeSpaceColonizationGenerator();       // "spacecol"
std::unique_ptr<IPlantGenerator> MakeLSystemGenerator();                 // "lsystem"
// FastNoise2 SIMD noise ("fastnoise2"). Returns nullptr when FastNoise2 is not compiled
// in (HBE_HAVE_FASTNOISE2=0), so the built-in value noise stays the default.
std::unique_ptr<INoiseField> MakeFastNoise2Field();

} // namespace hbe::veg
