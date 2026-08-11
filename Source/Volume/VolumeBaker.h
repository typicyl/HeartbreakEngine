// Source/Volume/VolumeBaker.h - turns a recorded VolumeFrame sequence into a `.hbvol` baked cache.
//
// The baker is an IVolumeFrameSink: VolumeSimController::Record drives ANY IVolumeSimulation and hands
// each frame here; the baker sparsifies every baked SCALAR field into a NanoVDB float grid (via
// VolumeNano) and packs the per-frame/per-field blobs into the `.hbvol` container (VolumeFormat.h). It
// has NO knowledge of the simulation that produced the frames - the whole point of the redesign.
//
// (Vector3 fields like velocity are not baked yet - only scalar fields become grids. FLOAT grids are
// exact; Fp16 quantization is a later size optimization paired with an Fp16-capable raymarch.)
#pragma once

#include "Volume/IVolumeFrameSink.h"
#include "Volume/VolumeFrame.h"
#include "Volume/VolumeSimConfig.h"

#include <functional>
#include <string>
#include <vector>

namespace hbe::volume {

class IVolumeSimulation;

class VolumeBaker final : public IVolumeFrameSink {
public:
    // `pruneThreshold`: a voxel within this of a field's background is dropped to sparsity.
    explicit VolumeBaker(float pruneThreshold = 0.005f) : pruneThreshold_(pruneThreshold) {}

    void Begin(const VolumeSimConfig& config, u32 frameCount, f32 frameRate) override;
    void Accept(u32 frameIndex, const VolumeFrame& frame) override;
    void End() override;

    bool                   Ok() const { return ok_; }
    const std::vector<u8>& Result() const { return result_; }      // finished .hbvol bytes
    std::vector<u8>        TakeResult() { return std::move(result_); }

private:
    struct FieldDecl { std::string name; FieldType type = FieldType::Scalar; };
    struct Slot { std::vector<u8> blob; u32 active = 0; f32 vmin = 0.0f, vmax = 0.0f; };
    struct Frame { f32 time = 0.0f; std::vector<Slot> slots; };

    float                    pruneThreshold_;
    VolumeBounds             bounds_;
    f32                      fps_ = 30.0f;
    u64                      sourceHash_ = 0;
    std::vector<std::string> bakeFieldNames_;
    std::vector<FieldDecl>   fields_;
    bool                     fieldsResolved_ = false;
    std::vector<Frame>       frames_;
    bool                     ok_ = false;
    std::vector<u8>          result_;
};

// FNV hash of a config's identity fields - the SAME value the baker stamps into the `.hbvol` header
// (read back via VolumeAsset::SourceHash), so a "config changed -> rebake" check is a hash compare.
u64 HashVolumeConfig(const VolumeSimConfig& config);

// Convenience: Reset `sim` and Record frames [startFrame, endFrame] into a VolumeBaker, returning the
// finished `.hbvol` bytes. Returns false on any failure (or a caller-requested cancel). `config`
// supplies timing + bakeFields. `onProgress(framesDone, framesTotal)` (optional) may return true to
// cancel mid-bake (then this returns false with no valid output).
bool BakeSimulation(IVolumeSimulation& sim, const VolumeSimConfig& config, u32 startFrame,
                    u32 endFrame, std::vector<u8>& outHbvol, float pruneThreshold = 0.005f,
                    const std::function<bool(u32 done, u32 total)>& onProgress = {});

// Write `.hbvol` bytes to a file (binary). Returns false on I/O error.
bool WriteHbvolFile(const std::string& path, const std::vector<u8>& bytes);

} // namespace hbe::volume
