// Vegetation/GrassGpu.cpp - GPU-compute grass field driver.
#include "Vegetation/GrassGpu.h"
#include "Renderer/Renderer.h"
#include "Core/Log.h"

#include <cstring>

namespace hbe::veg {

namespace {
// Must match GrassGenCB in Shaders/GrassGen.hlsl + GrassGenIndirect.hlsl (64 bytes;
// float3+float rows). `resetOnly` is read only by the indirect kernel (GrassGen ignores it).
struct GrassGenCB {
    glm::vec3 camPos; f32 time;
    f32 cellSize; u32 gridDim; u32 bladesPerCell; f32 maxDist;
    f32 terrainExtent; u32 terrainGridN; f32 terrainStep; f32 bladeHeight;
    f32 bladeWidth; u32 totalBlades; u32 resetOnly; f32 pad;
};
static_assert(sizeof(GrassGenCB) == 64, "GrassGenCB must match the HLSL cbuffer layout");

constexpr u32 kBladeStride = 32; // bytes per blade record (Grass.hlsl / GrassGen.hlsl)
} // namespace

void GrassGpuField::SetTerrain(const std::vector<f32>& heights, f32 extent, u32 gridN, f32 step) {
    heightData_ = heights;
    terrainExtent_ = extent;
    terrainGridN_ = gridN;
    terrainStep_ = step;
    haveTerrain_ = !heightData_.empty() && gridN >= 2 && step > 0.0f &&
                   heightData_.size() == static_cast<usize>(gridN) * gridN;
    if (!haveTerrain_ && !heightData_.empty())
        HBE_WARN("[GrassGpu] terrain heightfield size mismatch; grass disabled");
}

bool GrassGpuField::EnsureResources(Renderer& renderer) {
    if (failed_) return false;
    if (!renderer.SupportsGpuCompute()) return false; // clear-only backend (GL): no grass
    if (pipeline_.IsValid() && blades_.IsValid()) return true;

    totalBlades_ = cfg_.gridDim * cfg_.gridDim * cfg_.bladesPerCell;
    if (totalBlades_ == 0) { failed_ = true; return false; }

    rhi::ComputePipelineDesc cd{};
    cd.shaderName = "GrassGen";
    cd.entryPoint = "CSMain";
    cd.constantBytes = sizeof(GrassGenCB);
    cd.uavCount = 1; // u0 = blade records
    cd.srvCount = 1; // t0 = terrain heights
    cd.debugName = "GrassGen";
    pipeline_ = renderer.CreateComputePipeline(cd);

    rhi::GpuBufferDesc bd{};
    bd.elementCount = totalBlades_;
    bd.elementStride = kBladeStride;
    bd.usage = rhi::GpuBufferUsage::ShaderWrite | rhi::GpuBufferUsage::ShaderRead;
    bd.maxBindElements = 0; // always bound at offset 0 (whole buffer visible)
    bd.debugName = "GrassBlades";
    blades_ = renderer.CreateGpuBuffer(bd);

    rhi::GpuBufferDesc hd{};
    hd.elementCount = static_cast<u32>(heightData_.size());
    hd.elementStride = sizeof(f32);
    hd.usage = rhi::GpuBufferUsage::ShaderRead | rhi::GpuBufferUsage::CpuWrite;
    hd.maxBindElements = 0;
    hd.debugName = "GrassHeights";
    heights_ = renderer.CreateGpuBuffer(hd);

    if (!pipeline_.IsValid() || !blades_.IsValid() || !heights_.IsValid()) {
        HBE_WARN("[GrassGpu] resource creation failed; grass disabled");
        failed_ = true;
        return false;
    }
    HBE_INFO("[GrassGpu] {} blades ({:.1f} MB) over a {} grid", totalBlades_,
             (totalBlades_ * kBladeStride) / (1024.0 * 1024.0), cfg_.gridDim);
    return true;
}

// Lazily builds the compaction pipeline + the indirect draw-arg buffer the first time the
// indirect path is requested on a device that supports it. Sticky-fails so a device without
// the GrassIndirect pipeline just stays on the fixed path (no per-frame retry spam).
bool GrassGpuField::EnsureIndirect(Renderer& renderer) {
    if (indirectReady_) return true;
    if (indirectFailed_) return false;
    if (!renderer.SupportsIndirectDraw() || !blades_.IsValid()) { indirectFailed_ = true; return false; }

    rhi::ComputePipelineDesc cd{};
    cd.shaderName = "GrassGenIndirect";
    cd.entryPoint = "CSMain";
    cd.constantBytes = sizeof(GrassGenCB);
    cd.uavCount = 2; // u0 = compacted blades, u1 = draw args
    cd.srvCount = 1; // t0 = terrain heights
    cd.debugName = "GrassGenIndirect";
    pipelineIndirect_ = renderer.CreateComputePipeline(cd);

    rhi::GpuBufferDesc ad{};
    ad.elementCount = 1;
    ad.elementStride = 16; // {vtxPerInstance, instanceCount, startVtx, startInstance}
    ad.usage = rhi::GpuBufferUsage::ShaderWrite | rhi::GpuBufferUsage::IndirectArgs;
    ad.maxBindElements = 0;
    ad.debugName = "GrassDrawArgs";
    args_ = renderer.CreateGpuBuffer(ad);

    if (!pipelineIndirect_.IsValid() || !args_.IsValid()) {
        HBE_WARN("[GrassGpu] indirect resources unavailable; staying on the fixed grass path.");
        if (args_.IsValid()) renderer.DestroyGpuBuffer(args_);
        args_ = {};
        pipelineIndirect_ = {};
        indirectFailed_ = true;
        return false;
    }
    indirectReady_ = true;
    HBE_INFO("[GrassGpu] indirect/compaction path active ({} blade budget)", totalBlades_);
    return true;
}

void GrassGpuField::Update(Renderer& renderer, const glm::vec3& camPos, f32 timeSeconds) {
    if (!haveTerrain_) return;
    if (!EnsureResources(renderer)) return;

    const bool indirect = cfg_.useIndirect && EnsureIndirect(renderer);

    // Upload the heightfield into this frame's CpuWrite slot (static data, but a CpuWrite
    // ring rotates slots, so write it every frame - cheap for a demo-sized field).
    if (void* dst = renderer.MapGpuBuffer(heights_))
        std::memcpy(dst, heightData_.data(), heightData_.size() * sizeof(f32));

    GrassGenCB cb{};
    cb.camPos = camPos;
    cb.time = timeSeconds;
    cb.cellSize = cfg_.cellSize;
    cb.gridDim = cfg_.gridDim;
    cb.bladesPerCell = cfg_.bladesPerCell;
    cb.maxDist = cfg_.maxDist;
    cb.terrainExtent = terrainExtent_;
    cb.terrainGridN = terrainGridN_;
    cb.terrainStep = terrainStep_;
    cb.bladeHeight = cfg_.bladeHeight;
    cb.bladeWidth = cfg_.bladeWidth;
    cb.totalBlades = totalBlades_;

    if (indirect) {
        // Two dispatches against the compaction pipeline (the engine's inter-dispatch UAV
        // barrier serializes them): pass 0 resets the draw args {6,0,0,0}; pass 1 appends
        // only the visible blades and bumps the instance count. Then the draw is indirect.
        GrassGenCB reset = cb;
        reset.resetOnly = 1u;
        rhi::ComputeDispatch dr{};
        dr.pipeline = pipelineIndirect_;
        dr.uavs[0] = blades_; dr.uavs[1] = args_; dr.uavCount = 2;
        dr.srvs[0] = heights_; dr.srvCount = 1;
        dr.constants = &reset;
        dr.constantBytes = sizeof(reset);
        dr.groupsX = 1;
        renderer.QueueCompute(dr);

        cb.resetOnly = 0u;
        rhi::ComputeDispatch dg{};
        dg.pipeline = pipelineIndirect_;
        dg.uavs[0] = blades_; dg.uavs[1] = args_; dg.uavCount = 2;
        dg.srvs[0] = heights_; dg.srvCount = 1;
        dg.constants = &cb;
        dg.constantBytes = sizeof(cb);
        dg.groupsX = (totalBlades_ + 63u) / 64u;
        renderer.QueueCompute(dg);

        renderer.SetGrassIndirect(blades_, args_, totalBlades_);
        return;
    }

    rhi::ComputeDispatch d{};
    d.pipeline = pipeline_;
    d.uavs[0] = blades_;   d.uavCount = 1;
    d.srvs[0] = heights_;  d.srvCount = 1;
    d.constants = &cb;
    d.constantBytes = sizeof(cb);
    d.groupsX = (totalBlades_ + 63u) / 64u;
    renderer.QueueCompute(d);

    renderer.SetGrass(blades_, totalBlades_);
}

void GrassGpuField::Shutdown(Renderer& renderer) {
    if (blades_.IsValid()) renderer.DestroyGpuBuffer(blades_);
    if (heights_.IsValid()) renderer.DestroyGpuBuffer(heights_);
    if (args_.IsValid()) renderer.DestroyGpuBuffer(args_);
    blades_ = {};
    heights_ = {};
    args_ = {};
    pipeline_ = {};
    pipelineIndirect_ = {};
    indirectReady_ = false;
    indirectFailed_ = false;
    totalBlades_ = 0;
}

} // namespace hbe::veg
