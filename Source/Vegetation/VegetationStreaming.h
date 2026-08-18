// Vegetation/VegetationStreaming.h - the streamed shard generation pipeline (P8).
//
// GenerateShard is the WORKER-SIDE half of vegetation streaming: given a region's context
// + biomes, it scatters, generates each plant's skeleton, and fills a per-shard
// VegetationStore. It is pure CPU, DETERMINISTIC and job-safe - it touches only its inputs
// + Core/Rng, never the RHI / registry / tag-table (those are main-thread only) - so the
// streamer can kick it as a RunDetached job when a VegetationField becomes resident, and
// the main-thread finalize then uploads the meshes (design doc section 7). The generators
// are stateless, so many shards generate in parallel into their own stores with no locking.
#pragma once

#include "Vegetation/VegetationInterfaces.h"

namespace hbe::veg {

class VegetationWorld;
struct VegetationStore;

// Generates one shard's vegetation into `store` (appends trees). Deterministic from
// ctx.worldSeed + ctx.shardId + the species/biome inputs. Safe to call from a worker
// fiber. `world` is read-only (const): registries + stateless backends only.
void GenerateShard(const VegetationWorld& world, const VegShardContext& ctx,
                   const BiomeSet& biomes, VegetationStore& store);

// --test-vegstream: pins P8 - deterministic shard generation (same ctx -> identical store)
// and job-safety (many shards generated in PARALLEL match the serial reference). Headless.
bool StreamSelfTest();

} // namespace hbe::veg
