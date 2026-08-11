// Source/Volume/VolumeBaker.cpp - see the header.
#include "Volume/VolumeBaker.h"

#include "Volume/IVolumeSimulation.h"
#include "Volume/VolumeFormat.h"
#include "Volume/VolumeNano.h"
#include "Volume/VolumeSimController.h"

#include <filesystem>
#include <fstream>

namespace hbe::volume {

// FNV-1a over the config's identity-bearing fields, for stale-bake detection (a rebake with the same
// config reproduces the same hash; any authoring change flips it). Deterministic.
u64 HashVolumeConfig(const VolumeSimConfig& c) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    mix(c.model.data(), c.model.size());
    mix(&c.bounds.worldMin, sizeof(glm::vec3));
    mix(&c.bounds.worldMax, sizeof(glm::vec3));
    mix(&c.bounds.dim, sizeof(glm::ivec3));
    mix(&c.frameRate, sizeof(f32));
    mix(&c.substeps, sizeof(int));
    mix(&c.buoyancyAlpha, sizeof(f32));
    mix(&c.buoyancyBeta, sizeof(f32));
    mix(&c.densityDissipation, sizeof(f32));
    mix(&c.temperatureCooling, sizeof(f32));
    mix(&c.vorticityStrength, sizeof(f32));
    mix(&c.pressureIterations, sizeof(int));
    mix(&c.ambientTemperature, sizeof(f32));
    mix(&c.gravity, sizeof(glm::vec3));
    mix(&c.seed, sizeof(u32));
    // Field-by-field helpers. Hash shapes/curves FIELD BY FIELD, not as raw blobs: VolumeShape has
    // interior padding after the u8 `kind` (indeterminate bytes), so a raw memcpy hash would differ
    // between two logically-identical configs (default-constructed vs deserialized) and defeat the
    // stale-detection this hash exists for.
    auto mixShape = [&](const VolumeShape& s) {
        const u8 kind = static_cast<u8>(s.kind);
        mix(&kind, sizeof(u8));
        mix(&s.center, sizeof(glm::vec3));
        mix(&s.halfExtents, sizeof(glm::vec3));
        mix(&s.rotation, sizeof(glm::quat));
        mix(&s.coneHeight, sizeof(f32));
        mix(&s.edgeSoftness, sizeof(f32));
        mix(&s.meshId, sizeof(u32));
    };
    auto mixVec3Curve = [&](const VolumeVec3Curve& cv) {
        mix(&cv.constant, sizeof(glm::vec3));
        for (const VolumeVec3Key& k : cv.keys) { mix(&k.time, sizeof(f32)); mix(&k.value, sizeof(glm::vec3)); }
    };
    auto mixScalarCurve = [&](const VolumeScalarCurve& cv) {
        mix(&cv.constant, sizeof(f32));
        for (const VolumeScalarKey& k : cv.keys) { mix(&k.time, sizeof(f32)); mix(&k.value, sizeof(f32)); }
    };
    for (const VolumeEmitter& e : c.emitters) {
        mixShape(e.shape);
        mix(&e.densityRate, sizeof(f32));
        mix(&e.temperatureRate, sizeof(f32));
        mix(&e.temperatureTarget, sizeof(f32));
        mix(&e.fuelRate, sizeof(f32));
        mix(&e.velocity, sizeof(glm::vec3));
        // These change sim output (Burst vs Continuous, timing, inflow frame) but were omitted before,
        // so the live preview + stale-bake check ignored edits to them.
        const u8 mode = static_cast<u8>(e.mode);
        mix(&mode, sizeof(u8));
        mix(&e.startTime, sizeof(f32));
        mix(&e.endTime, sizeof(f32));
        mix(&e.burstDuration, sizeof(f32));
        const u8 wv = e.worldVelocity ? 1u : 0u;
        mix(&wv, sizeof(u8));
        mixVec3Curve(e.translationCurve);
        mixScalarCurve(e.densityRateCurve);
    }
    for (const VolumeObstacle& o : c.obstacles) {
        mix(o.name.data(), o.name.size());
        mixShape(o.shape);
        const u8 kind = static_cast<u8>(o.kind);
        mix(&kind, sizeof(u8));
        const u8 moving = o.moving ? 1u : 0u;
        mix(&moving, sizeof(u8));
        mixVec3Curve(o.translationCurve);
    }
    for (const std::string& fld : c.bakeFields) mix(fld.data(), fld.size());
    mix(&c.keyframeInterval, sizeof(u32));
    for (const auto& [k, v] : c.modelParams) { mix(k.data(), k.size()); mix(&v, sizeof(f32)); } // std::map: key order
    return h;
}

void VolumeBaker::Begin(const VolumeSimConfig& config, u32 /*frameCount*/, f32 frameRate) {
    bounds_ = config.bounds;
    fps_ = frameRate > 0.0f ? frameRate : config.frameRate;
    sourceHash_ = HashVolumeConfig(config);
    bakeFieldNames_ = config.bakeFields;
    fields_.clear();
    frames_.clear();
    fieldsResolved_ = false;
    ok_ = false;
    result_.clear();
}

void VolumeBaker::Accept(u32 /*frameIndex*/, const VolumeFrame& frame) {
    // Resolve the field list from the FIRST frame: the requested bakeFields that actually exist here
    // and are SCALAR (only scalars become float grids for now). If bakeFields was empty, take every
    // scalar field present. Also adopt this frame's bounds (fixed-domain sims => constant).
    if (!fieldsResolved_) {
        bounds_ = frame.bounds;
        auto addScalar = [&](const std::string& name) {
            const VolumeField* f = frame.field(name);
            if (f && f->type == FieldType::Scalar) fields_.push_back(FieldDecl{name, f->type});
        };
        if (bakeFieldNames_.empty()) {
            for (const VolumeField& f : frame.fields)
                if (f.type == FieldType::Scalar) fields_.push_back(FieldDecl{f.name, f.type});
        } else {
            for (const std::string& n : bakeFieldNames_) addScalar(n);
        }
        fieldsResolved_ = true;
    }

    Frame out;
    out.time = frame.time;
    out.slots.resize(fields_.size());
    for (usize i = 0; i < fields_.size(); ++i) {
        const VolumeField* f = frame.field(fields_[i].name);
        Slot& slot = out.slots[i];
        // Require the SAME type resolved on frame 0: a later frame that changed a field's type (e.g.
        // Scalar -> Vector3) would otherwise be mis-read as interleaved scalar. Mismatch -> empty slot.
        if (f && f->type == fields_[i].type) {
            GridBuildStats stats;
            if (BuildScalarGridBlob(*f, frame.bounds, slot.blob, stats, pruneThreshold_)) {
                slot.active = stats.activeVoxels;
                slot.vmin = stats.vmin;
                slot.vmax = stats.vmax;
            }
        }
        // A missing/failed field leaves an empty blob (size 0) - the reader returns background.
    }
    frames_.push_back(std::move(out));
}

void VolumeBaker::End() {
    if (frames_.empty() || fields_.empty()) { ok_ = false; return; }

    // Lay out the payload first so the seek index can carry absolute (payload-relative) offsets.
    std::vector<u8> payload;
    struct Loc { u64 off, size; u32 active; f32 vmin, vmax; };
    std::vector<std::vector<Loc>> locs(frames_.size());
    for (usize fi = 0; fi < frames_.size(); ++fi) {
        locs[fi].reserve(fields_.size());
        for (const Slot& s : frames_[fi].slots) {
            Loc l{static_cast<u64>(payload.size()), static_cast<u64>(s.blob.size()), s.active, s.vmin,
                  s.vmax};
            payload.insert(payload.end(), s.blob.begin(), s.blob.end());
            locs[fi].push_back(l);
        }
    }

    ByteWriter w;
    w.raw(kHbvolMagic, sizeof(kHbvolMagic));
    w.u32v(kHbvolVersion);
    w.u32v(0); // flags
    w.u32v(static_cast<u32>(frames_.size()));
    w.f32v(fps_);
    w.u32v(static_cast<u32>(HbvolCodec::None));
    w.u32v(static_cast<u32>(fields_.size()));
    w.f32v(bounds_.worldMin.x); w.f32v(bounds_.worldMin.y); w.f32v(bounds_.worldMin.z);
    w.f32v(bounds_.worldMax.x); w.f32v(bounds_.worldMax.y); w.f32v(bounds_.worldMax.z);
    w.i32v(bounds_.dim.x); w.i32v(bounds_.dim.y); w.i32v(bounds_.dim.z);
    w.u64v(sourceHash_);
    for (const FieldDecl& f : fields_) {
        w.str(f.name);
        w.u32v(static_cast<u32>(f.type));
        w.u32v(static_cast<u32>(HbvolGridType::Float));
    }
    for (usize fi = 0; fi < frames_.size(); ++fi) {
        w.f32v(frames_[fi].time);
        for (const Loc& l : locs[fi]) {
            w.u64v(l.off);
            w.u64v(l.size);
            w.u32v(l.active);
            w.f32v(l.vmin);
            w.f32v(l.vmax);
        }
    }
    w.raw(payload.data(), payload.size());

    result_ = std::move(w.buf);
    ok_ = true;
}

bool BakeSimulation(IVolumeSimulation& sim, const VolumeSimConfig& config, u32 startFrame,
                    u32 endFrame, std::vector<u8>& outHbvol, float pruneThreshold,
                    const std::function<bool(u32, u32)>& onProgress) {
    VolumeBaker baker(pruneThreshold);
    VolumeSimController ctrl(sim, config);
    ctrl.Record(baker, startFrame, endFrame, onProgress);
    if (!baker.Ok()) return false; // includes a cancel (Record aborted before End())
    outHbvol = baker.TakeResult();
    return true;
}

bool WriteHbvolFile(const std::string& path, const std::vector<u8>& bytes) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush(); // surface a disk-full / write error now, not silently in the destructor
    return static_cast<bool>(f);
}

} // namespace hbe::volume
