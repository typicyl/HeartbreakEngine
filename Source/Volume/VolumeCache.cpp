// Source/Volume/VolumeCache.cpp - see the header.
#include "Volume/VolumeCache.h"

#include "Core/JobSystem.h"

#include <cmath>

namespace hbe::volume {

void VolumeCache::LoadJob(void* arg) {
    Entry* e = static_cast<Entry*>(arg);
    auto asset = std::make_unique<VolumeAsset>();
    const bool ok = asset->LoadFile(e->path);
    if (ok) {
        e->asset = std::move(asset);
        // Publish the asset pointer BEFORE flipping to Ready: a reader that observes Ready (acquire)
        // must see the fully-constructed asset (release pairs with the load-acquire in GetState).
        e->state.store(State::Ready, std::memory_order_release);
    } else {
        e->state.store(State::Failed, std::memory_order_release);
    }
}

u32 VolumeCache::Acquire(const std::string& path) {
    if (path.empty()) return kInvalid;
    std::lock_guard<std::mutex> lock(mtx_);
    if (auto it = byPath_.find(path); it != byPath_.end()) return it->second;

    auto entry = std::make_unique<Entry>();
    entry->path = path;
    entry->state.store(State::Loading, std::memory_order_relaxed);
    Entry* raw = entry.get();
    const u32 handle = static_cast<u32>(entries_.size());
    entries_.push_back(std::move(entry));
    byPath_.emplace(path, handle);

    // Kick the load on a worker fiber; if the job system isn't running (e.g. a headless test),
    // load synchronously on the caller so the handle becomes Ready right away.
    if (jobs::IsInitialized())
        jobs::RunDetached(&LoadJob, raw);
    else
        LoadJob(raw);
    return handle;
}

VolumeCache::Entry* VolumeCache::Get(u32 handle) const {
    if (handle >= entries_.size()) return nullptr; // main-thread; entries_ only grows via Acquire
    return entries_[handle].get();
}

VolumeCache::State VolumeCache::GetState(u32 handle) const {
    const Entry* e = Get(handle);
    return e ? e->state.load(std::memory_order_acquire) : State::Empty;
}

i32 VolumeCache::TimeToFrame(u32 handle, f32 timeSeconds, bool loop) const {
    const Entry* e = Get(handle);
    if (!e || e->state.load(std::memory_order_acquire) != State::Ready || !e->asset) return -1;
    const i32 n = static_cast<i32>(e->asset->FrameCount());
    if (n <= 0) return -1;
    const f32 fps = e->asset->Fps() > 0.0f ? e->asset->Fps() : 30.0f;
    i32 f = static_cast<i32>(std::floor(timeSeconds * fps));
    if (loop)
        f = ((f % n) + n) % n; // wrap, handling negative time
    else
        f = f < 0 ? 0 : (f >= n ? n - 1 : f); // clamp
    return f;
}

VolumeAsset::GridView VolumeCache::DensityGrid(u32 handle, i32 frame) const {
    const Entry* e = Get(handle);
    if (!e || e->state.load(std::memory_order_acquire) != State::Ready || !e->asset || frame < 0)
        return {};
    return e->asset->Grid(static_cast<u32>(frame), "density");
}

VolumeBounds VolumeCache::Bounds(u32 handle) const {
    const Entry* e = Get(handle);
    return (e && e->state.load(std::memory_order_acquire) == State::Ready && e->asset)
               ? e->asset->Bounds()
               : VolumeBounds{};
}

f32 VolumeCache::Fps(u32 handle) const {
    const Entry* e = Get(handle);
    return (e && e->state.load(std::memory_order_acquire) == State::Ready && e->asset)
               ? e->asset->Fps()
               : 0.0f;
}

i32 VolumeCache::FrameCount(u32 handle) const {
    const Entry* e = Get(handle);
    return (e && e->state.load(std::memory_order_acquire) == State::Ready && e->asset)
               ? static_cast<i32>(e->asset->FrameCount())
               : 0;
}

VolumeCache& GlobalVolumeCache() {
    static VolumeCache cache;
    return cache;
}

} // namespace hbe::volume
