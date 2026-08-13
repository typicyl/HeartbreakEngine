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
// The HDS-Resonance multi-environment reverb (library component). Its tails are mixed into this
// node's output; the node already receives every spatial voice's audio, so no extra routing.
#include "hdsr/environment_reverb.h"

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

    // miniaudio node process callback. A STATIC MEMBER (not a free function) so it can name
    // the private nested Node, and so the vtable that points at it can be built inside a
    // member function - keeping the private Impl out of namespace scope entirely.
    static void OnProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
                          float** ppFramesOut, ma_uint32* pFrameCountOut);

    ma_engine* engine = nullptr;
    vraudio::ResonanceAudioApi* api = nullptr;
    int sampleRate = 48000;
    int inCh = 2; // node input-bus channel count == engine channels; downmixed to mono per source
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

    // --- Multi-environment reverb (hdsr::EnvironmentReverb) ---
    // The active-environment table + per-slot environment assignment are written from the main
    // thread and read in OnProcess (audio thread). Like the per-voice LPF, this is a benign,
    // bounded race - a torn read of a POD table entry is a one-block transient, never a crash; the
    // FDN state itself is only touched on the audio thread (in Process). Off by default: when
    // disabled, Process is byte-identical to the pre-environment-reverb path.
    static const int kMaxEnv = 16;
    static const int kReverbEnvCapacity = 8; // environments mixed simultaneously (perf capacity)
    hdsr::EnvironmentReverb envReverb;
    bool envReverbEnabled = false;
    AcousticEnvironment envTable[kMaxEnv];
    int envCount = 0;
    int slotEnvId[kMaxSources]; // environment id per source slot; -1 = unassigned
    std::vector<float> envOut;  // one Process block of interleaved stereo reverb
    std::vector<float> envMono; // per-call mono downmix of one source's input (for env reverb)

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

        // 1) Append this call's frames per source, DOWNMIXED to mono (vraudio sources are mono; the
        //    node's input buses are `inCh`-channel to match the LPF that feeds them). Silence for
        //    inactive / null inputs.
        const int ch = inCh > 0 ? inCh : 1;
        for (int s = 0; s < kMaxSources; ++s) {
            float* a = accum[s].data() + accumFill;
            if (active[s].load(std::memory_order_relaxed) && in != nullptr && in[s] != nullptr) {
                for (ma_uint32 i = 0; i < frames; ++i) {
                    float sum = 0.0f;
                    for (int c = 0; c < ch; ++c) sum += in[s][i * ch + c];
                    a[i] = sum / static_cast<float>(ch);
                }
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

        // 4) Multi-environment reverb: route each active source into the room it occupies and mix
        //    EVERY active environment's tail into the output, alongside the vraudio binaural. Each
        //    environment's tail is weighted AND spectrally shaped by its PER-BAND coupling (a distant
        //    room bleeds in darkened). `coupling` is a 9-band array -> the per-band SetEnvironment
        //    overload. Off by default -> this block is skipped entirely.
        if (envReverbEnabled && envReverb.IsReady()) {
            envReverb.BeginBlock();
            for (int e = 0; e < envCount; ++e) {
                envReverb.SetEnvironment(envTable[e].id, envTable[e].rt60, envTable[e].coupling,
                                         envTable[e].gain);
                envReverb.SetEnvironmentPreDelay(envTable[e].id, envTable[e].preDelaySec);
                envReverb.SetEnvironmentPan(envTable[e].id, envTable[e].pan);
            }
            const int ech = inCh > 0 ? inCh : 1;
            for (int s = 0; s < kMaxSources; ++s)
                if (active[s].load(std::memory_order_relaxed) && in != nullptr &&
                    in[s] != nullptr && slotEnvId[s] >= 0) {
                    for (ma_uint32 i = 0; i < frames; ++i) {
                        float sum = 0.0f;
                        for (int c = 0; c < ech; ++c) sum += in[s][i * ech + c];
                        envMono[static_cast<size_t>(i)] = sum / static_cast<float>(ech);
                    }
                    envReverb.AddInput(slotEnvId[s], envMono.data(), static_cast<int>(frames));
                }
            envReverb.Process(envOut.data(), static_cast<int>(frames));
            for (ma_uint32 i = 0; i < frames * 2; ++i) dst[i] += envOut[i];
        }

        *frameCountIn = frames;
        *frameCountOut = frames;
    }
};

void ResonanceSpatializer::Impl::OnProcess(ma_node* pNode, const float** ppFramesIn,
                                           ma_uint32* pFrameCountIn, float** ppFramesOut,
                                           ma_uint32* pFrameCountOut) {
    auto* node = reinterpret_cast<Node*>(pNode);
    node->self->Process(ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut);
}

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

    // Multi-environment reverb: an FDN bank per simultaneously-mixed environment (off until the
    // integration enables it; when off, Process skips it entirely).
    impl_->envReverb.Init(static_cast<int>(sampleRate), Impl::kReverbEnvCapacity);
    impl_->envOut.assign(kMaxProcess * 2, 0.0f);
    impl_->envMono.assign(kMaxProcess, 0.0f);
    for (int s = 0; s < kMaxSources; ++s) impl_->slotEnvId[s] = -1;
    impl_->envCount = 0;

    // The node's input buses MUST match the channel count of what feeds them (the per-voice LPF,
    // created at ma_engine_get_channels): miniaudio's ma_node_attach_output_bus requires matching
    // channels and silently fails otherwise - which left every spatial voice unconnected and the
    // whole binaural path SILENT. The mono vraudio sources are fed by downmixing in Process.
    impl_->inCh = static_cast<int>(ma_engine_get_channels(impl_->engine));
    if (impl_->inCh < 1) impl_->inCh = 1;
    ma_uint32 inChannels[kMaxSources];
    for (int s = 0; s < kMaxSources; ++s) inChannels[s] = static_cast<ma_uint32>(impl_->inCh);
    ma_uint32 outChannels[1] = {2};

    // Static storage duration (the node keeps a pointer to this vtable for its lifetime).
    // Built here, inside a member function, so &Impl::OnProcess and the private Impl stay
    // accessible without naming Impl at namespace scope.
    static ma_node_vtable vtable = {
        &Impl::OnProcess,
        nullptr, // onGetRequiredInputFrameCount
        static_cast<ma_uint8>(kMaxSources), // input bus count
        1,                                  // output bus count
        MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT,
    };

    ma_node_config cfg = ma_node_config_init();
    cfg.vtable = &vtable;
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
    impl_->slotEnvId[slot] = -1;
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
                                     f32 occlusion01, f32 minDist, f32 maxDist) {
    if (!IsReady() || slot < 0 || slot >= kMaxSources) return;
    const auto id = impl_->sourceId[slot];
    impl_->api->SetSourcePosition(id, pos.x, pos.y, pos.z);
    impl_->api->SetSourceVolume(id, volume);
    impl_->api->SetSoundObjectOcclusionIntensity(id, glm::clamp(occlusion01, 0.0f, 1.0f));
    // Forward Heartbreak's authored distance range (logarithmic rolloff) so a spatial voice
    // attenuates over its own minDistance..maxDistance instead of Resonance's defaults.
    const f32 lo = glm::max(minDist, 0.1f);
    const f32 hi = glm::max(maxDist, lo + 0.2f);
    impl_->api->SetSourceDistanceModel(id, vraudio::kLogarithmic, lo, hi);
}

void ResonanceSpatializer::SetSpeakerMode(bool speakers) {
    if (!IsReady()) return;
    impl_->api->SetStereoSpeakerMode(speakers);
}

void ResonanceSpatializer::SetSourceParams(int slot, f32 reverbSend, f32 spreadDeg) {
    if (!IsReady() || slot < 0 || slot >= kMaxSources) return;
    const auto id = impl_->sourceId[slot];
    impl_->api->SetSourceRoomEffectsGain(id, glm::max(reverbSend, 0.0f));
    impl_->api->SetSoundObjectSpread(id, glm::clamp(spreadDeg, 0.0f, 360.0f));
}

void ResonanceSpatializer::SetEnvironmentReverbEnabled(bool enabled) {
    if (impl_) impl_->envReverbEnabled = enabled;
}

void ResonanceSpatializer::SetEnvironments(const AcousticEnvironment* envs, int count) {
    if (!impl_) return;
    if (envs == nullptr || count <= 0) {
        impl_->envCount = 0;
        return;
    }
    if (count > Impl::kMaxEnv) count = Impl::kMaxEnv;
    for (int i = 0; i < count; ++i) impl_->envTable[i] = envs[i];
    impl_->envCount = count;
}

void ResonanceSpatializer::SetSourceEnvironment(int slot, int environmentId) {
    if (impl_ && slot >= 0 && slot < kMaxSources) impl_->slotEnvId[slot] = environmentId;
}

void ResonanceSpatializer::SetRoom(const AcousticRoom& room, bool enabled) {
    if (!IsReady()) return;
    if (!enabled) {
        impl_->api->EnableRoomEffects(false);
        return;
    }
    vraudio::ReflectionProperties refl; // zero-initialized by its ctor
    refl.room_position[0] = room.position.x;
    refl.room_position[1] = room.position.y;
    refl.room_position[2] = room.position.z;
    refl.room_rotation[0] = room.rotation.x; // vraudio wants (x,y,z,w)
    refl.room_rotation[1] = room.rotation.y;
    refl.room_rotation[2] = room.rotation.z;
    refl.room_rotation[3] = room.rotation.w;
    refl.room_dimensions[0] = room.dimensions.x;
    refl.room_dimensions[1] = room.dimensions.y;
    refl.room_dimensions[2] = room.dimensions.z;
    refl.cutoff_frequency = room.reflectionCutoffHz;
    for (int i = 0; i < 6; ++i) refl.coefficients[i] = room.reflectionCoeff[i];
    refl.gain = room.reflectionGain;
    impl_->api->SetReflectionProperties(refl);

    vraudio::ReverbProperties rev; // zero-initialized by its ctor
    for (int i = 0; i < 9; ++i) rev.rt60_values[i] = room.rt60[i];
    rev.gain = room.reverbGain;
    impl_->api->SetReverbProperties(rev);

    impl_->api->EnableRoomEffects(true);
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
void ResonanceSpatializer::SetSource(int, const glm::vec3&, f32, f32, f32, f32) {}
void ResonanceSpatializer::SetSpeakerMode(bool) {}
void ResonanceSpatializer::SetRoom(const AcousticRoom&, bool) {}
void ResonanceSpatializer::SetSourceParams(int, f32, f32) {}
void ResonanceSpatializer::SetEnvironmentReverbEnabled(bool) {}
void ResonanceSpatializer::SetEnvironments(const AcousticEnvironment*, int) {}
void ResonanceSpatializer::SetSourceEnvironment(int, int) {}
int ResonanceSpatializer::Capacity() { return 0; }

} // namespace hbe

#endif // HBE_HAVE_RESONANCE
