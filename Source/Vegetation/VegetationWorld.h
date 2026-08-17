// Vegetation/VegetationWorld.h - the vegetation subsystem container.
//
// Owned by the Engine BY VALUE (like precip_/ocean_/tagStream_). It holds:
//   * the data-driven registries (Species / Biome),
//   * the plug-in FACTORIES (backends registered BY NAME - a library slots in here),
//   * the per-shard side stores (created on residency, released on evict; P8).
// It owns NO new infrastructure: no second ECS, streamer, job pool, RNG or GPU-buffer
// abstraction. See docs/Design-Vegetation.md.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Vegetation/Species.h"
#include "Vegetation/VegetationInterfaces.h"
#include "Vegetation/VegetationStore.h"
#include "Core/Types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::veg {

// Interns Species by name -> stable SpeciesId. Records come from .hbspecies assets (P3);
// re-adding a name returns the existing id (idempotent load).
class SpeciesRegistry {
public:
    SpeciesId Add(const Species& s);
    SpeciesId Find(const std::string& name) const;
    const Species& Get(SpeciesId id) const; // returns a shared default when id is invalid
    bool Valid(SpeciesId id) const { return id.Valid() && id.v < species_.size(); }
    u32 Count() const { return static_cast<u32>(species_.size()); }
    void Clear() { species_.clear(); byName_.clear(); }

private:
    std::vector<Species> species_;
    std::unordered_map<std::string, u32> byName_;
};

class BiomeRegistry {
public:
    BiomeId Add(const Biome& b);
    BiomeId Find(const std::string& name) const;
    const Biome& Get(BiomeId id) const;
    bool Valid(BiomeId id) const { return id.Valid() && id.v < biomes_.size(); }
    u32 Count() const { return static_cast<u32>(biomes_.size()); }
    void Clear() { biomes_.clear(); byName_.clear(); }

private:
    std::vector<Biome> biomes_;
    std::unordered_map<std::string, u32> byName_;
};

class VegetationWorld {
public:
    VegetationWorld();  // registers the built-in backends (P1)
    VegetationWorld(const VegetationWorld&) = delete;
    VegetationWorld& operator=(const VegetationWorld&) = delete;

    SpeciesRegistry&       Species()       { return species_; }
    const SpeciesRegistry& Species() const { return species_; }
    BiomeRegistry&         Biomes()        { return biomes_; }
    const BiomeRegistry&   Biomes() const  { return biomes_; }

    // --- Backend factories (nullptr if the named backend is not registered) ----------
    IPlantGenerator*         Generator(const std::string& name) const;
    IVegetationDistribution* Distribution(const std::string& name) const;
    INoiseField*             Noise(const std::string& name) const;
    IWindModel*              Wind(const std::string& name) const;
    IPlantGrowthModel*       Growth(const std::string& name) const;
    IVegetationSimulation*   Simulation(const std::string& name) const;

    // The default backend of each kind (the first registered, or an explicitly chosen
    // one). Lets callers ask for "the noise field" without knowing which lib backs it.
    IPlantGenerator*         DefaultGenerator() const { return defaultGenerator_; }
    INoiseField*             DefaultNoise() const { return defaultNoise_; }
    IWindModel*              DefaultWind() const { return defaultWind_; }

    void RegisterGenerator(std::unique_ptr<IPlantGenerator>);
    void RegisterDistribution(std::unique_ptr<IVegetationDistribution>);
    void RegisterNoise(std::unique_ptr<INoiseField>);
    void RegisterWind(std::unique_ptr<IWindModel>);
    void RegisterGrowth(std::unique_ptr<IPlantGrowthModel>);
    void RegisterSimulation(std::unique_ptr<IVegetationSimulation>);

    // --- Per-shard stores (keyed by shard id) ----------------------------------------
    VegetationStore& StoreFor(u64 shardId);       // creates on first use
    VegetationStore* FindStore(u64 shardId);      // nullptr if not resident
    void ReleaseStore(u64 shardId);
    void ClearStores() { stores_.clear(); }
    u32 ResidentShardCount() const { return static_cast<u32>(stores_.size()); }
    // Visit every resident store (const) - used by veg::Update and reporting.
    template <class Fn> void ForEachStore(Fn&& fn) const {
        for (const auto& kv : stores_) fn(kv.first, *kv.second);
    }
    template <class Fn> void ForEachStore(Fn&& fn) {
        for (auto& kv : stores_) fn(kv.first, *kv.second);
    }

private:
    SpeciesRegistry species_;
    BiomeRegistry   biomes_;

    std::unordered_map<std::string, std::unique_ptr<IPlantGenerator>> generators_;
    std::unordered_map<std::string, std::unique_ptr<IVegetationDistribution>> distributions_;
    std::unordered_map<std::string, std::unique_ptr<INoiseField>> noises_;
    std::unordered_map<std::string, std::unique_ptr<IWindModel>> winds_;
    std::unordered_map<std::string, std::unique_ptr<IPlantGrowthModel>> growths_;
    std::unordered_map<std::string, std::unique_ptr<IVegetationSimulation>> sims_;
    std::unordered_map<u64, std::unique_ptr<VegetationStore>> stores_;

    IPlantGenerator* defaultGenerator_ = nullptr;
    INoiseField*     defaultNoise_     = nullptr;
    IWindModel*      defaultWind_      = nullptr;
};

} // namespace hbe::veg
