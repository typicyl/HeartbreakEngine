// Source/Volume/VolumeCache.h - loads baked `.hbvol` assets (async) and maps a playhead time to the
// current frame's density grid, for the runtime VolumeComponent.
//
// One VolumeAsset is held resident per unique path (whole-file for now). Acquire() never blocks: it
// kicks a fiber-job that reads + parses the file on a worker; until it finishes, the component simply
// isn't rendered (its playhead still advances, so it's already at the right frame when the load lands).
// Per-frame reads (TimeToFrame / DensityGrid) are resident lookups - no I/O. Per-frame streaming of
// individual frames around the playhead is a documented follow-up (the Entry reserves the spot).
#pragma once

#include "Volume/VolumeAsset.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::volume {

class VolumeCache {
public:
    enum class State : u8 { Empty, Loading, Ready, Failed };

    static constexpr u32 kInvalid = 0xFFFFFFFFu;

    // Stable handle for `path`; kicks an async load on first request (synchronous fallback if the job
    // system is not running). Never blocks. Safe to call every frame with the same path. Main-thread.
    u32   Acquire(const std::string& path);
    State GetState(u32 handle) const;

    // Playhead seconds -> frame index using the asset's Fps()/FrameCount(). loop wraps, else clamps.
    // Returns -1 when the handle is not Ready or empty.
    i32 TimeToFrame(u32 handle, f32 timeSeconds, bool loop) const;

    // Density GridView for `frame` (bytes owned by the resident VolumeAsset; valid while cached).
    VolumeAsset::GridView DensityGrid(u32 handle, i32 frame) const;

    VolumeBounds Bounds(u32 handle) const;
    f32          Fps(u32 handle) const;
    i32          FrameCount(u32 handle) const;

private:
    struct Entry {
        std::string                  path;
        std::atomic<State>           state{State::Empty};
        std::unique_ptr<VolumeAsset> asset; // published (with release) only once state == Ready
    };
    Entry*              Get(u32 handle) const; // main-thread; nullptr if out of range
    static void         LoadJob(void* arg);    // fiber-job entry: loads one Entry off-disk

    mutable std::mutex                    mtx_; // guards byPath_/entries_ mutation (Acquire only)
    std::unordered_map<std::string, u32>  byPath_;
    std::vector<std::unique_ptr<Entry>>   entries_;
};

// Engine-lifetime shared cache (function-local static; thread-safe init).
VolumeCache& GlobalVolumeCache();

} // namespace hbe::volume
