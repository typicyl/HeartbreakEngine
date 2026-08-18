// Vegetation/GrassGpu.h - the GPU-compute grass field driver.
//
// Owns the compute pipeline + the device-local blade buffer + the uploaded terrain
// heightfield, and each frame queues the generation dispatch and hands the blade buffer
// to the renderer (Renderer::SetGrass) for the lit grass draw. Mirrors particle::GpuSim's
// lifecycle (lazy resource creation, per-frame QueueCompute), but the blade COUNT is
// CPU-known so the draw needs no indirect. See Shaders/GrassGen.hlsl + Shaders/Grass.hlsl.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe {

class Renderer;

namespace veg {

class GrassGpuField {
public:
    struct Config {
        u32 gridDim = 256;       // cells per side of the camera-centered grid
        u32 bladesPerCell = 6;   // blades per cell
        f32 cellSize = 0.30f;    // world size of a cell (m)
        f32 maxDist = 42.0f;     // blades beyond this (from the camera) are culled
        f32 bladeHeight = 0.42f; // base blade height (m), varied per blade
        f32 bladeWidth = 0.055f; // blade base width (m)
        // Opt-in true GPU-driven path: a compaction compute appends only the visible blades
        // and writes an indirect draw-arg buffer, so the draw (ExecuteIndirect /
        // vkCmdDrawIndirect) submits one instance per surviving blade instead of 6*N verts
        // with the culled ones collapsed in the VS. Falls back to the fixed path when the
        // device lacks SupportsIndirectDraw() or the GrassIndirect pipeline is unavailable.
        bool useIndirect = false;
    };

    // Sets the terrain heightfield the compute samples for blade ground height. Copies
    // `heights` (row-major, GridN^2). Terrain is assumed centered at the world origin.
    void SetTerrain(const std::vector<f32>& heights, f32 extent, u32 gridN, f32 step);

    // Queues the generation compute for this frame and forwards the blade buffer to the
    // renderer. Call AFTER the terrain is set and BEFORE Renderer::RenderScene.
    void Update(Renderer& renderer, const glm::vec3& camPos, f32 timeSeconds);

    void Shutdown(Renderer& renderer);

    u32 BladeCount() const { return totalBlades_; }
    bool Failed() const { return failed_; }
    // True once the indirect/compaction resources are live (config asked for it AND the
    // device + pipeline support it). The editor reads this to show the active path.
    bool IndirectActive() const { return indirectReady_; }

    Config& Cfg() { return cfg_; }

private:
    bool EnsureResources(Renderer& renderer);
    bool EnsureIndirect(Renderer& renderer); // lazily builds the compaction pipeline + args

    Config cfg_;
    rhi::ComputePipelineHandle pipeline_{};
    rhi::ComputePipelineHandle pipelineIndirect_{}; // compaction generate (u0 blades, u1 args)
    rhi::GpuBufferHandle blades_{};   // device-local (compute writes, VS reads); also compacted
    rhi::GpuBufferHandle args_{};     // ShaderWrite | IndirectArgs (16B draw args)
    rhi::GpuBufferHandle heights_{};  // ShaderRead | CpuWrite (uploaded per frame)
    bool indirectReady_ = false;
    bool indirectFailed_ = false;

    std::vector<f32> heightData_;
    f32 terrainExtent_ = 0.0f;
    u32 terrainGridN_ = 0;
    f32 terrainStep_ = 0.0f;
    bool haveTerrain_ = false;

    u32 totalBlades_ = 0;
    bool failed_ = false;
    bool warned_ = false;
};

} // namespace veg
} // namespace hbe
