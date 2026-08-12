// Audio/SpatializerResonance.cpp - see SpatializerResonance.h.
#include "Audio/SpatializerResonance.h"

#include "Core/Log.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp> // glm::quatLookAt (needs GLM_ENABLE_EXPERIMENTAL, set globally)

#if HBE_HAVE_RESONANCE

// miniaudio: DECLARATIONS ONLY. MINIAUDIO_IMPLEMENTATION lives in AudioSystem.cpp; this TU
// links against it. Same arrangement as Editor/Importer.cpp.
#include <miniaudio.h>
// The fork's public renderer API (vraudio). The include root is the fork's repo root, added
// PRIVATE to this target in HeartbreakEngine/CMakeLists.txt.
#include "resonance_audio/api/resonance_audio_api.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace hbe {

namespace {
constexpr int    kMaxSources = 32;    // simultaneous 3D voices routed through Resonance
constexpr size_t kBlockSize  = 256;   // Resonance renders in fixed blocks of this many frames
constexpr size_t kMaxProcess = 8192;  // safety clamp on a single onProcess frame count
} // namespace

struct ResonanceSpatializer::Impl {
    // The custom miniaudio node. ma_node_base MUST be the first member so the node pointer
    // miniaudio hands back in onProcess can be cast straight to this.
    struct Node {
        ma_node_base base;
        Impl* self;
    };

    ma_engine* engine = nullptr;
    vraudio::ResonanceAudioApi* api = nullptr;
    int sampleRate = 48000;
    Node node{};
    bool nodeReady = false;

    // Fixed pool of Resonance sources, created once at Init. Creating/destroying sources per
    // voice would race the audio thread; instead a slot is claimed/released and inactive
    // slots are fed silence. slot index == node input-bus index == pool index.
    vraudio::ResonanceAudioApi::SourceId sourceId[kMaxSources] = {};
    std::atomic<bool> active[kMaxSources];

    // --- audio-thread only (onProcess) re-block state ---
    std::vector<std::vector<float>> accum; // per source, mono, kBlockSize + kMaxProcess long
    size_t accumFill = 0;
    std::vector<float> stereoTmp;          // one rendered block, interleaved L,R

    // Stereo output ring (interleaved), decouples Resonance's fixed block from miniaudio's
    // variable onProcess frame count.
    std::vector<float> ring;
    size_t ringCap = 0, ringHead = 0, ringCount = 0;

    void RingPush(const float* stereo, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            const size_t w = (ringHead + ringCount) % ringCap;
            ring[w * 2 + 0] = stereo[i * 2 + 0];
            ring[w * 2 + 1] = stereo[i * 2 + 1];
            if (ringCount < ringCap) {
                ++ringCount;
            } else {
                ringHead = (ringHead + 1) % ringCap; // overflow: drop oldest (should not happen)
            }
        }
    }
    size_t RingPop(float* out, size_t frames) {
        const size_t n = frames < ringCount ? frames : ringCount;
        for (size_t i = 0; i < n; ++i) {
            out[i * 2 + 0] = ring[ringHead * 2 + 0];
            out[i * 2 + 1] = ring[ringHead * 2 + 1];
            ringHead = (ringHead + 1) % ringCap;
        }
        ringCount -= n;
        return n;
    }

    void Process(const float** in, ma_uint32* frameCountIn, float** out,
                 ma_uint32* frameCountOut) {
        // 1:1 node (no DIFFERENT_PROCESSING_RATES): consume == produce == frames.
        const ma_uint32 cap = *frameCountOut < *frameCountIn ? *frameCountOut : *frameCountIn;
        ma_uint32 frames = cap > kMaxProcess ? static_cast<ma_uint32>(kMaxProcess) : cap;

        float* dst = out[0];
        if (api == nullptr) {
            std::memset(dst, 0, sizeof(float) * 2 * frames);
            *frameCountIn = frames;
            *frameCountOut = frames;
            return;
        }

        // 1) Append this call's mono frames per source (silence for inactive / null inputs).
        for (int s = 0; s < kMaxSources; ++s) {
            float* a = accum[s].data() + accumFill;
            if (active[s].load(std::memory_order_relaxed) && in != nullptr && in[s] != nullptr) {
                std::memcpy(a, in[s], sizeof(float) * frames);
            } else {
                std::memset(a, 0, sizeof(float) * frames);
            }
        }
        accumFill += frames;

        // 2) Render as many full Resonance blocks as we have input for.
        while (accumFill >= kBlockSize) {
            for (int s = 0; s < kMaxSources; ++s) {
                api->SetInterleavedBuffer(sourceId[s], accum[s].data(), 1, kBlockSize);
            }
            api->FillInterleavedOutputBuffer(2, kBlockSize, stereoTmp.data());
            RingPush(stereoTmp.data(), kBlockSize);

            const size_t rem = accumFill - kBlockSize;
            if (rem > 0) {
                for (int s = 0; s < kMaxSources; ++s) {
                    std::memmove(accum[s].data(), accum[s].data() + kBlockSize,
                                 sizeof(float) * rem);
                }
            }
            accumFill = rem;
        }

        // 3) Emit `frames` stereo from the ring, zero-filling any startup deficit.
        const size_t got = RingPop(dst, frames);
        if (got < frames) {
            std::memset(dst + got * 2, 0, sizeof(float) * 2 * (frames - got));
        }
        *frameCountIn = frames;
        *frameCountOut = frames;
    }
};

namespace {

void ResonanceOnProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
                        float** ppFramesOut, ma_uint32* pFrameCountOut) {
    auto* node = reinterpret_cast<ResonanceSpatializer::Impl::Node*>(pNode);
    node->self->Process(ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut);
}

ma_node_vtable g_resonanceVtable = {
    ResonanceOnProcess,
    nullptr, // onGetRequiredInputFrameCount
    static_cast<ma_uint8>(kMaxSources), // input bus count
    1,                                  // output bus count
    MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT,
};

} // namespace

ResonanceSpatializer::ResonanceSpatializer() : impl_(std::make_unique<Impl>()) {}
ResonanceSpatializer::~ResonanceSpatializer() { Shutdown(); }

bool ResonanceSpatializer::Init(void* maEngine, void* destNode, u32 sampleRate) {
    if (maEngine == nullptr) return false;
    impl_->engine = static_cast<ma_engine*>(maEngine);
    impl_->sampleRate = static_cast<int>(sampleRate);
    ma_node_graph* graph = ma_engine_get_node_graph(impl_->engine);

    impl_->api = vraudio::CreateResonanceAudioApi(2, kBlockSize, static_cast<int>(sampleRate));
    if (impl_->api == nullptr) {
        HBE_ERROR("ResonanceSpatializer: CreateResonanceAudioApi failed (sr={}).", sampleRate);
        return false;
    }

    for (int s = 0; s < kMaxSources; ++s) {
        impl_->sourceId[s] = impl_->api->CreateSoundObjectSource(vraudio::kBinauralHighQuality);
        impl_->active[s].store(false, std::memory_order_relaxed);
        impl_->api->SetSourceVolume(impl_->sourceId[s], 0.0f);
    }

    impl_->accum.assign(kMaxSources, std::vector<float>(kBlockSize + kMaxProcess, 0.0f));
    impl_->stereoTmp.assign(kBlockSize * 2, 0.0f);
    impl_->ringCap = kBlockSize * 8;
    impl_->ring.assign(impl_->ringCap * 2, 0.0f);
    impl_->ringHead = impl_->ringCount = impl_->accumFill = 0;

    ma_uint32 inChannels[kMaxSources];
    for (int s = 0; s < kMaxSources; ++s) inChannels[s] = 1;
    ma_uint32 outChannels[1] = {2};

    ma_node_config cfg = ma_node_config_init();
    cfg.vtable = &g_resonanceVtable;
    cfg.pInputChannels = inChannels;
    cfg.pOutputChannels = outChannels;

    impl_->node.self = impl_.get();
    const ma_result r = ma_node_init(graph, &cfg, nullptr, &impl_->node.base);
    if (r != MA_SUCCESS) {
        HBE_ERROR("ResonanceSpatializer: ma_node_init failed ({}).", static_cast<int>(r));
        delete impl_->api;
        impl_->api = nullptr;
        return false;
    }

    ma_node* dst = destNode != nullptr ? static_cast<ma_node*>(destNode)
                                       : ma_node_graph_get_endpoint(graph);
    ma_node_attach_output_bus(&impl_->node.base, 0, dst, 0);

    impl_->nodeReady = true;
    HBE_INFO("ResonanceSpatializer: ready ({} sources, {}Hz, block {}).", kMaxSources,
             sampleRate, static_cast<int>(kBlockSize));
    return true;
}

void ResonanceSpatializer::Shutdown() {
    if (!impl_) return;
    if (impl_->nodeReady) {
        // Detaches from the graph and stops further onProcess calls. Call only when the
        // graph is quiescent (AudioSystem does this in its dtor / bus rebuild).
        ma_node_uninit(&impl_->node.base, nullptr);
        impl_->nodeReady = false;
    }
    if (impl_->api != nullptr) {
        delete impl_->api; // frees every pooled source with it
        impl_->api = nullptr;
    }
}

bool ResonanceSpatializer::IsReady() const {
    return impl_ && impl_->nodeReady && impl_->api != nullptr;
}

void* ResonanceSpatializer::InputNode() const {
    return (impl_ && impl_->nodeReady) ? &impl_->node.base : nullptr;
}

int ResonanceSpatializer::AcquireSource() {
    if (!IsReady()) return -1;
    for (int s = 0; s < kMaxSources; ++s) {
        bool expected = false;
        if (impl_->active[s].compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel)) {
            impl_->api->SetSourceVolume(impl_->sourceId[s], 1.0f);
            impl_->api->SetSoundObjectOcclusionIntensity(impl_->sourceId[s], 0.0f);
            return s;
        }
    }
    return -1;
}

void ResonanceSpatializer::ReleaseSource(int slot) {
    if (!impl_ || slot < 0 || slot >= kMaxSources) return;
    if (impl_->api != nullptr) impl_->api->SetSourceVolume(impl_->sourceId[slot], 0.0f);
    impl_->active[slot].store(false, std::memory_order_release);
}

void ResonanceSpatializer::SetListener(const glm::vec3& pos, const glm::vec3& forward,
                                       const glm::vec3& up) {
    if (!IsReady()) return;
    impl_->api->SetHeadPosition(pos.x, pos.y, pos.z);
    const glm::vec3 f =
        glm::length(forward) > 1e-6f ? glm::normalize(forward) : glm::vec3(0, 0, -1);
    const glm::vec3 u = glm::length(up) > 1e-6f ? glm::normalize(up) : glm::vec3(0, 1, 0);
    const glm::quat q = glm::quatLookAt(f, u); // rotates the -Z axis onto `forward`
    impl_->api->SetHeadRotation(q.x, q.y, q.z, q.w);
}

void ResonanceSpatializer::SetSource(int slot, const glm::vec3& pos, f32 volume,
                                     f32 occlusion01) {
    if (!IsReady() || slot < 0 || slot >= kMaxSources) return;
    const auto id = impl_->sourceId[slot];
    impl_->api->SetSourcePosition(id, pos.x, pos.y, pos.z);
    impl_->api->SetSourceVolume(id, volume);
    impl_->api->SetSoundObjectOcclusionIntensity(id, glm::clamp(occlusion01, 0.0f, 1.0f));
}

int ResonanceSpatializer::Capacity() { return kMaxSources; }

} // namespace hbe

#else // HBE_HAVE_RESONANCE == 0 : the fork is not present; compile to no-ops.

namespace hbe {

struct ResonanceSpatializer::Impl {};

ResonanceSpatializer::ResonanceSpatializer() = default;
ResonanceSpatializer::~ResonanceSpatializer() = default;
bool ResonanceSpatializer::Init(void*, void*, u32) { return false; }
void ResonanceSpatializer::Shutdown() {}
bool ResonanceSpatializer::IsReady() const { return false; }
void* ResonanceSpatializer::InputNode() const { return nullptr; }
int ResonanceSpatializer::AcquireSource() { return -1; }
void ResonanceSpatializer::ReleaseSource(int) {}
void ResonanceSpatializer::SetListener(const glm::vec3&, const glm::vec3&, const glm::vec3&) {}
void ResonanceSpatializer::SetSource(int, const glm::vec3&, f32, f32) {}
int ResonanceSpatializer::Capacity() { return 0; }

} // namespace hbe

#endif // HBE_HAVE_RESONANCE
