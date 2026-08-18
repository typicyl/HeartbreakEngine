// Vegetation/VegetationWorld.cpp - registries, backend factories, per-shard stores.
#include "Vegetation/VegetationWorld.h"
#include "Vegetation/VegetationBackends.h"

namespace hbe::veg {

// --- SpeciesRegistry -----------------------------------------------------------------
SpeciesId SpeciesRegistry::Add(const Species& s) {
    if (auto it = byName_.find(s.name); it != byName_.end()) {
        species_[it->second] = s; // update in place (idempotent re-load)
        return SpeciesId{ it->second };
    }
    const u32 id = static_cast<u32>(species_.size());
    species_.push_back(s);
    byName_.emplace(s.name, id);
    return SpeciesId{ id };
}

SpeciesId SpeciesRegistry::Find(const std::string& name) const {
    if (auto it = byName_.find(name); it != byName_.end()) return SpeciesId{ it->second };
    return SpeciesId{};
}

const Species& SpeciesRegistry::Get(SpeciesId id) const {
    static const Species kDefault{};
    return Valid(id) ? species_[id.v] : kDefault;
}

// --- BiomeRegistry -------------------------------------------------------------------
BiomeId BiomeRegistry::Add(const Biome& b) {
    if (auto it = byName_.find(b.name); it != byName_.end()) {
        biomes_[it->second] = b;
        return BiomeId{ it->second };
    }
    const u32 id = static_cast<u32>(biomes_.size());
    biomes_.push_back(b);
    byName_.emplace(b.name, id);
    return BiomeId{ id };
}

BiomeId BiomeRegistry::Find(const std::string& name) const {
    if (auto it = byName_.find(name); it != byName_.end()) return BiomeId{ it->second };
    return BiomeId{};
}

const Biome& BiomeRegistry::Get(BiomeId id) const {
    static const Biome kDefault{};
    return Valid(id) ? biomes_[id.v] : kDefault;
}

// --- VegetationWorld -----------------------------------------------------------------
VegetationWorld::VegetationWorld() {
    RegisterBuiltinBackends(*this); // the P1 in-house stub backends
}

void VegetationWorld::RegisterGenerator(std::unique_ptr<IPlantGenerator> g) {
    if (!g) return;
    IPlantGenerator* raw = g.get();
    generators_[g->Name()] = std::move(g);
    if (!defaultGenerator_) defaultGenerator_ = raw;
}
void VegetationWorld::RegisterDistribution(std::unique_ptr<IVegetationDistribution> d) {
    if (!d) return;
    distributions_[d->Name()] = std::move(d);
}
void VegetationWorld::RegisterNoise(std::unique_ptr<INoiseField> n) {
    if (!n) return;
    INoiseField* raw = n.get();
    noises_[n->Name()] = std::move(n);
    if (!defaultNoise_) defaultNoise_ = raw;
}
void VegetationWorld::RegisterWind(std::unique_ptr<IWindModel> w) {
    if (!w) return;
    IWindModel* raw = w.get();
    winds_[w->Name()] = std::move(w);
    if (!defaultWind_) defaultWind_ = raw;
}
void VegetationWorld::RegisterGrowth(std::unique_ptr<IPlantGrowthModel> g) {
    if (!g) return;
    growths_[g->Name()] = std::move(g);
}
void VegetationWorld::RegisterSimulation(std::unique_ptr<IVegetationSimulation> s) {
    if (!s) return;
    sims_[s->Name()] = std::move(s);
}

template <class Map>
static auto* LookupBackend(const Map& m, const std::string& name) {
    auto it = m.find(name);
    return it == m.end() ? nullptr : it->second.get();
}

IPlantGenerator* VegetationWorld::Generator(const std::string& n) const {
    return LookupBackend(generators_, n);
}
IVegetationDistribution* VegetationWorld::Distribution(const std::string& n) const {
    return LookupBackend(distributions_, n);
}
INoiseField* VegetationWorld::Noise(const std::string& n) const {
    return LookupBackend(noises_, n);
}
IWindModel* VegetationWorld::Wind(const std::string& n) const {
    return LookupBackend(winds_, n);
}
IPlantGrowthModel* VegetationWorld::Growth(const std::string& n) const {
    return LookupBackend(growths_, n);
}
IVegetationSimulation* VegetationWorld::Simulation(const std::string& n) const {
    return LookupBackend(sims_, n);
}

IPlantGenerator* VegetationWorld::GeneratorForStrategy(GenStrategy strategy) const {
    const char* name = "spacecol";
    switch (strategy) {
        case GenStrategy::SpaceColonization: name = "spacecol"; break;
        case GenStrategy::LSystem:           name = "lsystem"; break;
        // Custom/External have no built-in generator yet; External replays an authored
        // mesh instead. Both fall back to space colonization for a structural skeleton.
        case GenStrategy::Custom:
        case GenStrategy::External:
        default:                             name = "spacecol"; break;
    }
    IPlantGenerator* g = Generator(name);
    return g ? g : defaultGenerator_;
}

// --- Per-shard stores ----------------------------------------------------------------
VegetationStore& VegetationWorld::StoreFor(u64 shardId) {
    auto it = stores_.find(shardId);
    if (it == stores_.end())
        it = stores_.emplace(shardId, std::make_unique<VegetationStore>()).first;
    return *it->second;
}

VegetationStore* VegetationWorld::FindStore(u64 shardId) {
    auto it = stores_.find(shardId);
    return it == stores_.end() ? nullptr : it->second.get();
}

void VegetationWorld::ReleaseStore(u64 shardId) {
    stores_.erase(shardId);
}

} // namespace hbe::veg
