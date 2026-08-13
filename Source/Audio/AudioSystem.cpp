// Audio/AudioSystem.cpp - miniaudio-backed implementation.
#include "Audio/AudioSystem.h"
#include "Audio/SpatializerResonance.h" // optional binaural backend (HDS Resonance fork)

#include "Assets/AudioEvent.h"
#include "Assets/MusicGraph.h"
#include "Assets/UAF.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING // playback only
#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstdlib> // std::getenv (HBE_RESONANCE gate)
#include <deque>
#include <functional>
#include <list>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

std::vector<AudioBusDesc> DefaultAudioBuses() {
    return {{"Music", "Master", 1.0f, false},
            {"SFX", "Master", 1.0f, false},
            {"Ambience", "Master", 1.0f, false}};
}

struct AudioSystem::Impl {
    ma_engine engine{};
    bool ready = false;

    // -- Mixer bus tree (ma_sound_group). "Master" routes to the endpoint. ----
    struct Bus {
        ma_sound_group group{};
        std::string parent;
        f32 volume = 1.0f;
        bool muted = false;
    };
    std::unordered_map<std::string, Bus> buses;
    std::vector<std::string> busOrder; // Master first, then declaration order

    ma_sound_group* GroupOf(const std::string& name) {
        if (auto it = buses.find(name); it != buses.end()) return &it->second.group;
        if (auto it = buses.find("Master"); it != buses.end()) return &it->second.group;
        return nullptr; // pre-ConfigureBuses: route to the engine endpoint
    }

    void ApplyBusGain(Bus& b) {
        ma_sound_group_set_volume(&b.group, b.muted ? 0.0f : b.volume);
    }

    void DestroyBuses() {
        // Children before parents (reverse declaration order).
        for (auto it = busOrder.rbegin(); it != busOrder.rend(); ++it) {
            if (auto bit = buses.find(*it); bit != buses.end()) {
                ma_sound_group_uninit(&bit->second.group);
            }
        }
        buses.clear();
        busOrder.clear();
    }

    // One playing fire-and-forget sound. Addresses must be stable while the
    // voice plays (miniaudio keeps pointers into these), hence the std::list.
    //
    // PINNED IN MEMORY, ENFORCED BY THE COMPILER. `ma_sound` and `ma_audio_buffer`
    // are self-referential: ma_sound_init_from_data_source links the sound's
    // ma_node_base into the engine's node graph AT ITS ADDRESS (ma_node_base::
    // pOutputBuses points at its own _outputBuses, the destination group's input bus
    // keeps a linked-list pointer to that same storage, and pSound->pDataSource
    // points at the sibling ma_audio_buffer). Relocating one copies the bytes and
    // leaves the graph pointing at the ORIGINAL address; the device thread then
    // walks a dangling ma_node_output_bus* every callback.
    //
    // That is not hypothetical - it shipped. Building a music layer in a local and
    // then push_back-ing a MOVE of it left the node graph pointing into the dead
    // stack frame, and the audio thread faulted inside
    // ma_node_input_bus_read_pcm_frames reading 0xFFFF'FFFF'FFFF'FFFF (symbolized
    // from HeartbreakEditor.exe+0xF7F32D). It was a use-after-free on the audio
    // thread, so it presented as a random crash seconds after an unrelated action.
    //
    // Deleting the copy/move members turns that entire class of mistake into a
    // COMPILE ERROR rather than a heisenbug. Every owner must therefore construct
    // in place in a node-stable container (std::list / unordered_map, never vector)
    // - see `voices`, `musicLayers` and `spatial`, all of which emplace.
    struct Voice {
        Voice() = default;
        Voice(const Voice&) = delete;
        Voice& operator=(const Voice&) = delete;
        Voice(Voice&&) = delete;
        Voice& operator=(Voice&&) = delete;

        std::vector<u8> data;
        ma_audio_buffer buffer{};
        ma_sound sound{};
        u32 id = 0; // 0 = anonymous (PlayPCM); event voices get an id
        // Spatial occlusion: a positional voice routes sound -> lpf -> bus so
        // obstruction can MUFFLE (not just quieten). Updated in UpdateScene.
        ma_lpf_node lpf{};
        bool hasLpf = false;
        bool spatial = false;         // participates in the occlusion pass
        glm::vec3 worldPos{0.0f};      // last known emitter position (one-shots)
        f32 baseVolume = 1.0f;         // volume before occlusion attenuation
        f32 minDist = 1.0f, maxDist = 30.0f;
        f32 occ = 0.0f;                // smoothed occlusion 0..1 (0 = clear)
        f32 curCutoff = 20000.0f;      // current LPF cutoff (Hz), to skip no-op reinits
        int resSlot = -1;              // Resonance source slot (>=0 = routed binaurally)
    };
    std::list<Voice> voices;
    u32 nextVoiceId = 1;

    // Spatial occlusion config (set by the engine from project settings).
    OcclusionConfig occlusion;
    static constexpr f32 kOpenCutoff = 20000.0f; // "transparent" LPF (no muffle)

    // Optional binaural spatial backend (HDS Resonance fork). OFF unless HBE_RESONANCE=1 in
    // the environment. When on, a spatial voice claims a source slot and routes
    // sound -> lpf -> this node -> Master for HRTF rendering instead of miniaudio's amplitude
    // panning; when off, everything below behaves exactly as before (resSlot stays -1). Gated
    // so it cannot change shipping behaviour until enabled + playtested. See
    // Audio/SpatializerResonance.h.
    ResonanceSpatializer resonance;
    bool useResonance = false;

    // Insert a per-voice low-pass node between a positional sound and its bus so
    // occlusion can darken the tone. No-op (leaves hasLpf=false) if unavailable;
    // the voice then still gets occlusion ATTENUATION, just no muffle.
    void AttachOcclusionLpf(Voice& v, const std::string& bus) {
        ma_node_graph* ng = ma_engine_get_node_graph(&engine);
        if (!ng) return;
        const ma_uint32 ch = ma_engine_get_channels(&engine);
        const ma_uint32 sr = ma_engine_get_sample_rate(&engine);
        ma_lpf_node_config cfg = ma_lpf_node_config_init(ch, sr, kOpenCutoff, 2);
        if (ma_lpf_node_init(ng, &cfg, nullptr, &v.lpf) != MA_SUCCESS) return;
        // When this voice claimed a Resonance slot, its LPF feeds the binaural node's INPUT
        // bus (== the slot index); Resonance mixes it into Master. Otherwise the normal
        // sound -> lpf -> bus routing. The LPF stays in front either way so the bespoke
        // occlusion muffle still applies before the signal reaches Resonance.
        ma_node* dst;
        ma_uint32 dstBus = 0;
        if (v.resSlot >= 0 && resonance.IsReady() && resonance.InputNode() != nullptr) {
            dst = static_cast<ma_node*>(resonance.InputNode());
            dstBus = static_cast<ma_uint32>(v.resSlot);
        } else {
            dst = GroupOf(bus) ? reinterpret_cast<ma_node*>(GroupOf(bus))
                               : ma_node_graph_get_endpoint(ng);
        }
        // sound -> lpf -> dst (attach the destination first, then reroute the sound).
        ma_node_attach_output_bus(&v.lpf, 0, dst, dstBus);
        ma_node_attach_output_bus(&v.sound, 0, &v.lpf, 0);
        v.hasLpf = true;
        v.curCutoff = kOpenCutoff;
    }
    void DestroyLpf(Voice& v) {
        if (v.resSlot >= 0) {
            resonance.ReleaseSource(v.resSlot);
            v.resSlot = -1;
        }
        if (v.hasLpf) {
            ma_lpf_node_uninit(&v.lpf, nullptr);
            v.hasLpf = false;
        }
    }

    // Fraction of rays [0,1] from `src` to `listener` blocked by geometry. The
    // direct ray plus a ring of offset rays around the source: if some offset ray
    // finds a clear path (a doorway/gap), occlusion drops and the sound leaks.
    f32 ComputeOcclusion(const glm::vec3& src, const glm::vec3& listener,
                         const std::function<bool(const glm::vec3&, const glm::vec3&)>& blocked) const {
        if (!blocked) return 0.0f;
        glm::vec3 dir = listener - src;
        const f32 len = glm::length(dir);
        if (len < 1e-3f) return 0.0f;
        dir /= len;
        // Nudge the origin off the source so its own collider doesn't self-occlude,
        // but never past the listener (clamp for sources closer than the nudge).
        const glm::vec3 o0 = src + dir * glm::min(0.3f, len * 0.5f);
        const glm::vec3 up = std::fabs(dir.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 t1 = glm::normalize(glm::cross(dir, up));
        const glm::vec3 t2 = glm::cross(dir, t1);
        const int rays = glm::clamp(occlusion.rays, 1, 16);
        int hit = 0;
        if (blocked(o0, listener)) ++hit;
        for (int i = 1; i < rays; ++i) {
            const f32 a = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(glm::max(rays - 1, 1));
            const glm::vec3 off = (t1 * std::cos(a) + t2 * std::sin(a)) * occlusion.spread;
            if (blocked(o0 + off, listener)) ++hit;
        }
        return static_cast<f32>(hit) / static_cast<f32>(rays);
    }

    // Glide a voice's occlusion toward `target` and apply attenuation + LPF cutoff.
    void ApplyOcclusion(Voice& v, f32 target, f32 dt) {
        const f32 rate = dt > 0.0f ? dt / 0.15f : 1.0f; // ~150ms glide (snap if dt<=0)
        v.occ += glm::clamp(target - v.occ, -rate, rate);
        const f32 o = glm::clamp(v.occ, 0.0f, 1.0f);
        const f32 gain = glm::mix(1.0f, glm::clamp(occlusion.attenuation, 0.0f, 1.0f), o);
        ma_sound_set_volume(&v.sound, v.baseVolume * gain); // set_volume is thread-safe
        if (v.hasLpf) {
            const f32 cutoff = glm::mix(kOpenCutoff, glm::max(occlusion.cutoffHz, 80.0f), o);
            // ma_lpf_node_reinit rewrites biquad coefficients in place while the audio
            // thread may read them: a data race whose worst case is one torn-coefficient
            // buffer (a brief transient), never a crash/realloc. Reinit only on a coarse
            // change so steady state does ZERO reinits (no race) and a transition does a
            // handful, keeping the window tiny. (A fully lock-free fix would double-buffer
            // two LPF nodes and swap via the thread-safe attach; not worth it for a glitch.)
            if (std::fabs(cutoff - v.curCutoff) > 250.0f) {
                ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&engine),
                                                      ma_engine_get_sample_rate(&engine), cutoff, 2);
                ma_lpf_node_reinit(&lc, &v.lpf);
                v.curCutoff = cutoff;
            }
        }
    }

    // Restore a spatial voice to un-occluded (base volume + open LPF). Used when
    // occlusion is disabled/stops while a voice is mid-flight, so it doesn't stay
    // muffled forever (matters for looping spatial one-shots).
    void ClearOcclusion(Voice& v) {
        ma_sound_set_volume(&v.sound, v.baseVolume);
        if (v.hasLpf && v.curCutoff < kOpenCutoff - 20.0f) {
            ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&engine),
                                                  ma_engine_get_sample_rate(&engine), kOpenCutoff, 2);
            ma_lpf_node_reinit(&lc, &v.lpf);
            v.curCutoff = kOpenCutoff;
        }
        v.occ = 0.0f;
    }

    std::mt19937 rng{0x5EEDu};

    // Persistent spatial voices for AudioSource entities, keyed by voice id.
    struct SpatialVoice {
        Voice voice;
        entt::entity entity = entt::null;
        std::string asset; // recreated when the component's asset changes
        std::string bus;   // recreated when the bus changes
    };
    std::unordered_map<u32, SpatialVoice> spatial;
    u32 nextSpatialId = 1;

    void DestroySpatial(SpatialVoice& sv) {
        ma_sound_uninit(&sv.voice.sound); // detach upstream node first
        DestroyLpf(sv.voice);
        ma_audio_buffer_uninit(&sv.voice.buffer);
    }

    void DestroyVoices() {
        for (Voice& v : voices) {
            ma_sound_uninit(&v.sound);
            DestroyLpf(v);
            ma_audio_buffer_uninit(&v.buffer);
        }
        voices.clear();
    }

    // -- Adaptive music director ------------------------------------------------
    // A music STATE's layers play as looping non-spatial voices on the Music bus,
    // started together (sample-aligned) so they stay in sync. Each layer's gain
    // lerps toward its target (param-driven, or 0 when its state is leaving).
    struct MusicLayerVoice {
        Voice voice;
        f32 baseVolume = 1.0f;
        std::string parameter; // empty = always at baseVolume
        f32 paramLo = 0.0f, paramHi = 1.0f;
        f32 current = 0.0f;    // current gain (lerped each UpdateMusic)
        bool fadingOut = false; // its state was left -> fade to 0, then reap
    };
    MusicGraph musicGraph;
    std::filesystem::path musicAssets;
    std::unordered_map<std::string, f32> musicParams; // name -> live value
    std::list<MusicLayerVoice> musicLayers;
    std::string musicState;     // current state ("" = stopped)
    f32 musicFade = 2.0f;       // active crossfade seconds
    bool musicHasGraph = false;

    // --- Musical clock + deferred (quantized) transitions --------------------
    // A state switch requested mid-phrase sounds like a mistake. The clock runs
    // from the moment the current state started, and a request whose outgoing
    // state has a sync rule waits here until the next beat/bar boundary.
    f32 musicClock = 0.0f;          // seconds since the current state started
    bool musicPendingValid = false;
    std::string musicPendingState;  // "" with musicPendingValid = a deferred Stop
    f32 musicPendingFade = -1.0f;
    f32 musicPendingAt = 0.0f;      // musicClock value to switch at
    // Set while UpdateMusic fires a due transition, so the PlayMusicState /
    // StopMusic call it makes performs the switch instead of re-deferring it
    // against the very boundary that just elapsed (which would never resolve).
    bool musicForceImmediate = false;

    // --- Dialogue ducking ----------------------------------------------------
    // While speech plays, the music layers drop by duckDecibels so the line stays
    // intelligible. `duckTarget` is set from outside each frame; `duckGain` is the
    // smoothed multiplier actually applied on top of every layer's own gain.
    bool duckActive = false;
    f32 duckGain = 1.0f;

    // Linear gain for the graph's duck depth (0 dB = 1.0, no change).
    f32 DuckFloor() const {
        if (musicGraph.duckDecibels <= 0.0f) return 1.0f;
        return std::pow(10.0f, -musicGraph.duckDecibels / 20.0f);
    }

    // Subtitles / closed captions: structured lines enqueued when a captioned
    // sound starts. The engine drains these into its subtitle::Stack, which does
    // the per-kind gating and formatting.
    bool captionsEnabled = false;
    std::deque<subtitle::Line> captions;

    // Builds the subtitle line for a .uaf that carries a caption, mapping the
    // asset's AudioKind onto the subtitle kind so speech and non-speech stay
    // distinguishable all the way to the screen. Returns false when the asset has
    // no caption (the common case for UI/SFX clips).
    bool MakeCaption(const uaf::Audio& a, subtitle::Line& out) const {
        if (!captionsEnabled || a.caption.empty()) return false;
        out.speaker = a.speaker;
        out.text = a.caption;
        switch (a.kind) {
            case uaf::AudioKind::Voiceline: out.kind = subtitle::Kind::Voiceline; break;
            case uaf::AudioKind::Ambience:  out.kind = subtitle::Kind::Ambient;   break;
            case uaf::AudioKind::Music:     out.kind = subtitle::Kind::Ambient;   break;
            case uaf::AudioKind::Sfx:       out.kind = subtitle::Kind::Sound;     break;
        }
        // Speech outranks ambience so a noisy scene can't evict a spoken line.
        out.priority = subtitle::IsSpeech(out.kind) ? 10 : 0;
        return true;
    }

    // Decoded-PCM cache for PlayUAF one-shots (path -> Audio). Cleared when an
    // asset is re-imported / edited so stale PCM or captions aren't served.
    std::unordered_map<std::string, uaf::Audio> uafCache;

    void DestroyMusic() {
        for (MusicLayerVoice& m : musicLayers) {
            ma_sound_uninit(&m.voice.sound);
            ma_audio_buffer_uninit(&m.voice.buffer);
        }
        musicLayers.clear();
        musicState.clear();
    }

    // Starts a looping, non-spatial voice on the Music bus at volume 0 (the director
    // fades it in). Returns false if the audio is unusable.
    bool StartMusicVoice(Voice& v, const uaf::Audio& audio) {
        ma_format format;
        switch (audio.bitsPerSample) {
            case 8:  format = ma_format_u8;  break;
            case 16: format = ma_format_s16; break;
            case 32: format = ma_format_f32; break;
            default: return false;
        }
        const usize bytesPerFrame = (audio.bitsPerSample / 8) * audio.channels;
        if (bytesPerFrame == 0 || audio.pcm.size() < bytesPerFrame) return false;
        v.data = audio.pcm;
        ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
            format, audio.channels, audio.pcm.size() / bytesPerFrame, v.data.data(), nullptr);
        cfg.sampleRate = audio.sampleRate;
        if (ma_audio_buffer_init(&cfg, &v.buffer) != MA_SUCCESS) return false;
        if (ma_sound_init_from_data_source(&engine, &v.buffer, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                           GroupOf("Music"), &v.sound) != MA_SUCCESS) {
            ma_audio_buffer_uninit(&v.buffer);
            return false;
        }
        ma_sound_set_looping(&v.sound, MA_TRUE);
        ma_sound_set_volume(&v.sound, 0.0f);
        ma_sound_start(&v.sound);
        return true;
    }

    // The layer's parameter-driven target gain (1 when unbound).
    f32 LayerGain(const MusicLayerVoice& m) const {
        if (m.parameter.empty()) return m.baseVolume;
        f32 v = 0.0f;
        if (const auto it = musicParams.find(m.parameter); it != musicParams.end()) v = it->second;
        const f32 lo = m.paramLo, hi = m.paramHi;
        const f32 t = (std::fabs(hi - lo) < 1e-5f) ? (v >= lo ? 1.0f : 0.0f)
                                                   : glm::smoothstep(lo, hi, v);
        return m.baseVolume * t;
    }

    // Starts a voice from raw PCM on a bus. Returns nullptr on failure.
    Voice* StartVoice(const void* pcm, usize bytes, u32 channels, u32 sampleRate,
                      u32 bitsPerSample, const std::string& bus, f32 volume,
                      f32 pitch, bool loop, const glm::vec3* position,
                      f32 minDist, f32 maxDist) {
        if (!ready || !pcm || bytes == 0 || channels == 0 || sampleRate == 0) {
            return nullptr;
        }
        ma_format format;
        switch (bitsPerSample) {
            case 8:  format = ma_format_u8;  break;
            case 16: format = ma_format_s16; break;
            case 32: format = ma_format_f32; break;
            default:
                HBE_WARN("Audio: unsupported bit depth {}.", bitsPerSample);
                return nullptr;
        }
        const usize bytesPerFrame = (bitsPerSample / 8) * channels;
        const u64 frames = bytes / bytesPerFrame;
        if (frames == 0) return nullptr;

        Voice& v = voices.emplace_back();
        v.data.assign(static_cast<const u8*>(pcm), static_cast<const u8*>(pcm) + bytes);

        ma_audio_buffer_config cfg =
            ma_audio_buffer_config_init(format, channels, frames, v.data.data(), nullptr);
        cfg.sampleRate = sampleRate;
        if (ma_audio_buffer_init(&cfg, &v.buffer) != MA_SUCCESS) {
            voices.pop_back();
            return nullptr;
        }
        // Positional voices claim a Resonance slot up front when the binaural backend is on;
        // that decides the sound flags (Resonance spatializes, so miniaudio must not pan).
        if (position && useResonance && resonance.IsReady()) v.resSlot = resonance.AcquireSource();
        const ma_uint32 flags =
            (position && v.resSlot < 0) ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (ma_sound_init_from_data_source(&engine, &v.buffer, flags, GroupOf(bus),
                                           &v.sound) != MA_SUCCESS) {
            if (v.resSlot >= 0) resonance.ReleaseSource(v.resSlot);
            ma_audio_buffer_uninit(&v.buffer);
            voices.pop_back();
            return nullptr;
        }
        ma_sound_set_volume(&v.sound, volume);
        ma_sound_set_pitch(&v.sound, glm::max(pitch, 0.05f));
        ma_sound_set_looping(&v.sound, loop ? MA_TRUE : MA_FALSE);
        if (position) {
            // Positional -> occlusion-capable: route through a per-voice LPF and remember its
            // world position so the occlusion pass can process it.
            v.spatial = true;
            v.worldPos = *position;
            v.baseVolume = volume;
            v.minDist = minDist;
            v.maxDist = maxDist;
            if (v.resSlot < 0) {
                // miniaudio panning path (Resonance owns distance/position when slotted).
                ma_sound_set_position(&v.sound, position->x, position->y, position->z);
                ma_sound_set_attenuation_model(&v.sound, ma_attenuation_model_inverse);
                ma_sound_set_min_distance(&v.sound, glm::max(minDist, 0.01f));
                ma_sound_set_max_distance(&v.sound, glm::max(maxDist, minDist + 0.01f));
            }
            AttachOcclusionLpf(v, bus); // -> Resonance node when v.resSlot >= 0, else -> bus
            if (v.resSlot >= 0) resonance.SetSource(v.resSlot, *position, volume, 0.0f);
        }
        ma_sound_start(&v.sound);
        return &v;
    }

    ~Impl() {
        DestroyVoices();
        DestroyMusic();
        for (auto& [id, sv] : spatial) DestroySpatial(sv);
        spatial.clear();
        resonance.Shutdown(); // uninit the node while the engine + Master are still alive
        DestroyBuses();
        if (ready) ma_engine_uninit(&engine);
    }
};

AudioSystem::AudioSystem() : impl_(std::make_unique<Impl>()) {
    if (ma_engine_init(nullptr, &impl_->engine) == MA_SUCCESS) {
        impl_->ready = true;
        HBE_INFO("Audio: miniaudio engine ready ({} Hz).",
                 ma_engine_get_sample_rate(&impl_->engine));
    } else {
        HBE_WARN("Audio: no playback device available; audio disabled.");
    }
    // Opt-in binaural spatial backend (HDS Resonance fork). Off unless HBE_RESONANCE=1, so
    // existing builds behave identically until it is explicitly enabled and playtested. The
    // node itself is created in ConfigureBuses, once the Master bus exists.
#if HBE_HAVE_RESONANCE
    // std::getenv trips MSVC's C4996 "unsafe" deprecation; it is fine for a read-only lookup.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* resEnv = std::getenv("HBE_RESONANCE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (resEnv != nullptr && resEnv[0] == '1') {
        impl_->useResonance = true;
        HBE_INFO("Audio: HDS Resonance binaural spatializer requested (HBE_RESONANCE=1).");
    }
#endif
}

AudioSystem::~AudioSystem() = default;

bool AudioSystem::IsAvailable() const {
    return impl_ && impl_->ready;
}

void AudioSystem::ConfigureBuses(const std::vector<AudioBusDesc>& buses) {
    if (!IsAvailable()) return;
    // Rebuilding the tree invalidates every attachment: stop voices first.
    impl_->DestroyVoices();
    impl_->DestroyMusic();
    for (auto& [id, sv] : impl_->spatial) impl_->DestroySpatial(sv);
    impl_->spatial.clear();
    impl_->resonance.Shutdown(); // node feeds Master; tear it down before the buses go
    impl_->DestroyBuses();

    // Master always exists, attached to the engine endpoint.
    {
        Impl::Bus& master = impl_->buses["Master"];
        if (ma_sound_group_init(&impl_->engine, 0, nullptr, &master.group) != MA_SUCCESS) {
            impl_->buses.clear();
            HBE_WARN("Audio: master bus creation failed; mixing disabled.");
            return;
        }
        impl_->busOrder.push_back("Master");
    }

    // Bring up the optional binaural backend now that Master exists (its stereo output feeds
    // the Master group, so master volume still applies). No-op unless HBE_RESONANCE=1.
    if (impl_->useResonance) {
        impl_->resonance.Init(&impl_->engine, &impl_->buses["Master"].group,
                              ma_engine_get_sample_rate(&impl_->engine));
    }

    // Children attach to their parent; unknown parents fall back to Master.
    // Multiple passes resolve out-of-order declarations.
    std::vector<AudioBusDesc> pending(buses.begin(), buses.end());
    bool progressed = true;
    while (!pending.empty() && progressed) {
        progressed = false;
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->name.empty() || it->name == "Master" ||
                impl_->buses.count(it->name)) {
                it = pending.erase(it); // invalid or duplicate
                continue;
            }
            const std::string parent =
                impl_->buses.count(it->parent) ? it->parent : std::string();
            const bool parentPendingLater =
                parent.empty() && std::any_of(pending.begin(), pending.end(),
                                              [&](const AudioBusDesc& d) {
                                                  return d.name == it->parent;
                                              });
            if (parentPendingLater) {
                ++it; // wait for the parent to be created
                continue;
            }
            Impl::Bus bus;
            bus.parent = parent.empty() ? "Master" : parent;
            bus.volume = glm::clamp(it->volume, 0.0f, 2.0f);
            bus.muted = it->muted;
            Impl::Bus& slot = impl_->buses[it->name];
            slot = bus;
            if (ma_sound_group_init(&impl_->engine, 0,
                                    &impl_->buses[slot.parent].group,
                                    &slot.group) != MA_SUCCESS) {
                impl_->buses.erase(it->name);
            } else {
                impl_->ApplyBusGain(slot);
                impl_->busOrder.push_back(it->name);
            }
            it = pending.erase(it);
            progressed = true;
        }
    }
    // Anything left has an unresolvable parent (cycle): attach to Master.
    for (const AudioBusDesc& d : pending) {
        if (d.name.empty() || impl_->buses.count(d.name)) continue;
        Impl::Bus& slot = impl_->buses[d.name];
        slot.parent = "Master";
        slot.volume = glm::clamp(d.volume, 0.0f, 2.0f);
        slot.muted = d.muted;
        if (ma_sound_group_init(&impl_->engine, 0, &impl_->buses["Master"].group,
                                &slot.group) != MA_SUCCESS) {
            impl_->buses.erase(d.name);
        } else {
            impl_->ApplyBusGain(slot);
            impl_->busOrder.push_back(d.name);
        }
    }
    HBE_INFO("Audio: mixer configured ({} buses).", impl_->busOrder.size());
}

std::vector<std::string> AudioSystem::BusNames() const {
    return impl_ ? impl_->busOrder : std::vector<std::string>{};
}

void AudioSystem::SetBusVolume(const std::string& bus, f32 volume) {
    if (!IsAvailable()) return;
    if (auto it = impl_->buses.find(bus); it != impl_->buses.end()) {
        it->second.volume = glm::clamp(volume, 0.0f, 2.0f);
        impl_->ApplyBusGain(it->second);
    }
}

f32 AudioSystem::BusVolume(const std::string& bus) const {
    if (!impl_) return 1.0f;
    const auto it = impl_->buses.find(bus);
    return it != impl_->buses.end() ? it->second.volume : 1.0f;
}

void AudioSystem::SetCaptionsEnabled(bool on) {
    if (impl_) impl_->captionsEnabled = on;
}

bool AudioSystem::PopCaption(subtitle::Line& out) {
    if (!impl_ || impl_->captions.empty()) return false;
    out = std::move(impl_->captions.front());
    impl_->captions.pop_front();
    return true;
}

void AudioSystem::SetBusMuted(const std::string& bus, bool muted) {
    if (!IsAvailable()) return;
    if (auto it = impl_->buses.find(bus); it != impl_->buses.end()) {
        it->second.muted = muted;
        impl_->ApplyBusGain(it->second);
    }
}

bool AudioSystem::BusMuted(const std::string& bus) const {
    if (!impl_) return false;
    const auto it = impl_->buses.find(bus);
    return it != impl_->buses.end() && it->second.muted;
}

std::string AudioSystem::DeviceName() const {
    if (!impl_ || !impl_->ready) return "No audio device";
    std::string name;
    if (ma_device* dev = ma_engine_get_device(&impl_->engine))
        name = dev->playback.name; // device name (empty on some backends)
    if (name.empty()) name = "Default device";
    return name + " - " + std::to_string(ma_engine_get_sample_rate(&impl_->engine)) + " Hz";
}

u32 AudioSystem::PostEvent(const AudioEvent& ev, const std::filesystem::path& assetsDir,
                           const glm::vec3* position) {
    if (!IsAvailable() || ev.sounds.empty()) return 0;

    // Weighted random pick from the event's sound pool.
    f32 total = 0.0f;
    for (const AudioEventSound& s : ev.sounds) total += glm::max(s.weight, 0.0f);
    const AudioEventSound* pick = &ev.sounds.front();
    if (total > 0.0f) {
        std::uniform_real_distribution<f32> dist(0.0f, total);
        f32 r = dist(impl_->rng);
        for (const AudioEventSound& s : ev.sounds) {
            r -= glm::max(s.weight, 0.0f);
            if (r <= 0.0f) { pick = &s; break; }
        }
    }
    if (pick->asset.empty()) return 0;

    const std::optional<uaf::Audio> audio = uaf::ReadAudio(assetsDir / pick->asset);
    if (!audio) {
        HBE_WARN("Audio: event sound '{}' failed to load.", pick->asset);
        return 0;
    }

    std::uniform_real_distribution<f32> unit(-1.0f, 1.0f);
    const f32 volume =
        glm::max(0.0f, ev.volume + unit(impl_->rng) * ev.volumeVariance);
    const f32 pitch =
        glm::max(0.05f, ev.pitch + unit(impl_->rng) * ev.pitchVariance);

    Impl::Voice* v = impl_->StartVoice(
        audio->pcm.data(), audio->pcm.size(), audio->channels, audio->sampleRate,
        audio->bitsPerSample, ev.bus, volume, pitch, ev.loop,
        ev.spatial ? position : nullptr, ev.minDistance, ev.maxDistance);
    if (!v) return 0;
    v->id = impl_->nextVoiceId++;
    return v->id;
}

void AudioSystem::StopEvent(u32 voiceId) {
    if (!IsAvailable() || voiceId == 0) return;
    for (auto it = impl_->voices.begin(); it != impl_->voices.end(); ++it) {
        if (it->id == voiceId) {
            ma_sound_uninit(&it->sound);
            impl_->DestroyLpf(*it);
            ma_audio_buffer_uninit(&it->buffer);
            impl_->voices.erase(it);
            return;
        }
    }
}

void AudioSystem::StopAllVoices() {
    if (!IsAvailable()) return;
    impl_->DestroyVoices();
}

bool AudioSystem::PlayPCM(const void* pcm, usize bytes, u32 channels, u32 sampleRate,
                          u32 bitsPerSample, const std::string& bus) {
    if (!IsAvailable()) return false;
    return impl_->StartVoice(pcm, bytes, channels, sampleRate, bitsPerSample, bus,
                             1.0f, 1.0f, false, nullptr, 1.0f, 30.0f) != nullptr;
}

bool AudioSystem::PlayUAF(const std::filesystem::path& uafPath, const std::string& bus,
                          bool caption) {
    // Decoded-PCM cache: UI one-shots (hover/click) can fire several times a
    // second as the cursor sweeps a menu; without this each edge re-reads and
    // re-decodes the whole .uaf on the main thread (a frame hitch). Keyed by
    // path; cleared by ClearUAFCache on re-import/edit so stale PCM/captions
    // aren't served. Main-thread only (frame loop / editor).
    if (!impl_) return false;
    const std::string key = uafPath.string();
    auto it = impl_->uafCache.find(key);
    if (it == impl_->uafCache.end()) {
        std::optional<uaf::Audio> audio = uaf::ReadAudio(uafPath);
        if (!audio) {
            HBE_WARN("Audio: failed to read '{}'.", uafPath.string());
            return false;
        }
        it = impl_->uafCache.emplace(key, std::move(*audio)).first;
    }
    const uaf::Audio& a = it->second;
    // Surface a baked caption so a voiceline played this way (e.g. a schematic
    // PlayVoiceline) subtitles just like a scene AudioSource. UI/SFX clips have
    // empty captions, so this is a no-op for them.
    if (subtitle::Line cap; caption && impl_->MakeCaption(a, cap))
        impl_->captions.push_back(std::move(cap));
    return PlayPCM(a.pcm.data(), a.pcm.size(), a.channels, a.sampleRate,
                   a.bitsPerSample, bus);
}

bool AudioSystem::PlayUAFAt(const std::filesystem::path& uafPath, const glm::vec3& position,
                            const std::string& bus, f32 minDist, f32 maxDist, bool caption) {
    if (!IsAvailable()) return false;
    const std::string key = uafPath.string();
    auto it = impl_->uafCache.find(key);
    if (it == impl_->uafCache.end()) {
        std::optional<uaf::Audio> audio = uaf::ReadAudio(uafPath);
        if (!audio) {
            HBE_WARN("Audio: failed to read '{}'.", uafPath.string());
            return false;
        }
        it = impl_->uafCache.emplace(key, std::move(*audio)).first;
    }
    const uaf::Audio& a = it->second;
    if (subtitle::Line cap; caption && impl_->MakeCaption(a, cap))
        impl_->captions.push_back(std::move(cap));
    return impl_->StartVoice(a.pcm.data(), a.pcm.size(), a.channels, a.sampleRate, a.bitsPerSample,
                             bus, 1.0f, 1.0f, false, &position, minDist, maxDist) != nullptr;
}

void AudioSystem::ClearUAFCache() {
    if (impl_) impl_->uafCache.clear();
}

void AudioSystem::SetOcclusion(const OcclusionConfig& cfg) {
    if (impl_) impl_->occlusion = cfg;
}

void AudioSystem::SetMusicGraph(const MusicGraph& graph,
                                const std::filesystem::path& assetsDir) {
    if (!impl_) return;
    impl_->DestroyMusic();
    impl_->musicGraph = graph;
    impl_->musicAssets = assetsDir;
    impl_->musicHasGraph = true;
    impl_->musicParams.clear();
    for (const MusicParameter& p : graph.parameters)
        impl_->musicParams[p.name] = glm::clamp(p.defaultValue, p.min, p.max);
    impl_->musicFade = glm::max(graph.defaultFade, 0.01f);
}

void AudioSystem::PlayMusicState(const std::string& state, f32 fadeSeconds) {
    if (!IsAvailable() || !impl_->musicHasGraph) return;
    if (state == impl_->musicState) return; // already there
    const MusicState* st = impl_->musicGraph.FindState(state);
    if (!st) {
        HBE_WARN("Music: no state '{}'.", state);
        return;
    }
    // Quantized transition: the state currently PLAYING owns the boundary we wait
    // for (it is its bar line we are mid-way through). Defer until then; a later
    // request before the boundary simply replaces the pending one, so rapid
    // gameplay changes collapse to the last intent instead of queueing a burst.
    if (!impl_->musicForceImmediate) {
        if (const MusicState* cur = impl_->musicGraph.FindState(impl_->musicState)) {
            const f32 interval = SyncInterval(cur->sync, cur->bpm, cur->beatsPerBar);
            if (interval > 0.0f) {
                const f32 next = std::ceil((impl_->musicClock + 1e-4f) / interval) * interval;
                impl_->musicPendingValid = true;
                impl_->musicPendingState = state;
                impl_->musicPendingFade = fadeSeconds;
                impl_->musicPendingAt = next;
                HBE_INFO("Music: '{}' -> '{}' queued for {} ({:.2f}s away).", impl_->musicState,
                         state, SyncName(cur->sync), next - impl_->musicClock);
                return;
            }
        }
    }
    impl_->musicPendingValid = false;
    impl_->musicFade =
        glm::max(fadeSeconds < 0.0f ? impl_->musicGraph.defaultFade : fadeSeconds, 0.01f);
    // The outgoing state's layers fade to silence and get reaped.
    for (Impl::MusicLayerVoice& m : impl_->musicLayers) m.fadingOut = true;
    // Bring the new state's layers in together (started this tick = in sync).
    for (const MusicLayer& layer : st->layers) {
        if (layer.asset.empty()) continue;
        const std::optional<uaf::Audio> audio =
            uaf::ReadAudio(impl_->musicAssets / layer.asset);
        if (!audio) {
            HBE_WARN("Music: layer '{}' failed to load.", layer.asset);
            continue;
        }
        // CONSTRUCT IN THE LIST FIRST, THEN INITIALIZE. `ma_sound` and
        // `ma_audio_buffer` are not relocatable: ma_sound_init_from_data_source
        // links the sound's ma_node_base into the engine's node graph AT ITS
        // ADDRESS (ma_node_base::pOutputBuses points at its own _outputBuses, and
        // the "Music" group's input bus keeps a linked-list pointer to that same
        // storage), and the device thread walks that list every callback. Building
        // the voice in a local and then push_back-ing a *move* of it copies the
        // bytes to the heap but leaves the graph pointing at the stack frame; the
        // instant this function returns, that frame is reused and the audio thread
        // dereferences a garbage ma_node_output_bus* (the observed crash: a read of
        // 0xFFFF'FFFF'FFFF'FFFF inside ma_node_input_bus_read_pcm_frames).
        // This is why `voices` is a std::list and StartVoice uses emplace_back too.
        Impl::MusicLayerVoice& mv = impl_->musicLayers.emplace_back();
        mv.baseVolume = glm::max(layer.volume, 0.0f);
        mv.parameter = layer.parameter;
        mv.paramLo = layer.paramLo;
        mv.paramHi = layer.paramHi;
        mv.current = 0.0f;
        if (!impl_->StartMusicVoice(mv.voice, *audio)) {
            impl_->musicLayers.pop_back(); // nothing was attached to the graph
            continue;
        }
    }
    impl_->musicState = state;
    impl_->musicClock = 0.0f; // the new state's bar grid starts now
    HBE_INFO("Music: -> '{}' ({} layers, {:.1f}s fade).", state, st->layers.size(),
             impl_->musicFade);
}

void AudioSystem::SetMusicDucking(bool active) {
    if (impl_) impl_->duckActive = active;
}

usize AudioSystem::MusicLayerCount() const {
    return impl_ ? impl_->musicLayers.size() : 0;
}

bool AudioSystem::MusicTransitionPending(std::string& outState, f32& outSeconds) const {
    if (!impl_ || !impl_->musicPendingValid) return false;
    outState = impl_->musicPendingState;
    outSeconds = glm::max(impl_->musicPendingAt - impl_->musicClock, 0.0f);
    return true;
}

void AudioSystem::StopMusic(f32 fadeSeconds) {
    if (!IsAvailable()) return;
    impl_->musicFade =
        glm::max(fadeSeconds < 0.0f ? impl_->musicGraph.defaultFade : fadeSeconds, 0.01f);
    // A Stop respects the same musical boundary a state change would.
    if (const MusicState* cur =
            impl_->musicForceImmediate ? nullptr : impl_->musicGraph.FindState(impl_->musicState)) {
        const f32 interval = SyncInterval(cur->sync, cur->bpm, cur->beatsPerBar);
        if (interval > 0.0f) {
            impl_->musicPendingValid = true;
            impl_->musicPendingState.clear(); // empty pending state = deferred stop
            impl_->musicPendingFade = fadeSeconds;
            impl_->musicPendingAt =
                std::ceil((impl_->musicClock + 1e-4f) / interval) * interval;
            return;
        }
    }
    impl_->musicPendingValid = false;
    for (Impl::MusicLayerVoice& m : impl_->musicLayers) m.fadingOut = true;
    impl_->musicState.clear();
}

void AudioSystem::SetMusicParameter(const std::string& name, f32 value) {
    if (!impl_) return;
    f32 v = value;
    for (const MusicParameter& p : impl_->musicGraph.parameters)
        if (p.name == name) { v = glm::clamp(value, p.min, p.max); break; }
    impl_->musicParams[name] = v;
}

f32 AudioSystem::MusicParameterValue(const std::string& name) const {
    if (!impl_) return 0.0f;
    const auto it = impl_->musicParams.find(name);
    return it != impl_->musicParams.end() ? it->second : 0.0f;
}

std::string AudioSystem::CurrentMusicState() const {
    return impl_ ? impl_->musicState : std::string();
}

std::vector<std::string> AudioSystem::MusicStateNames() const {
    std::vector<std::string> names;
    if (impl_)
        for (const MusicState& s : impl_->musicGraph.states) names.push_back(s.name);
    return names;
}

bool AudioSystem::HasMusicGraph() const { return impl_ && impl_->musicHasGraph; }

void AudioSystem::PostStinger(const std::filesystem::path& uafPath, const std::string& bus,
                              f32 volume) {
    if (!IsAvailable()) return;
    const std::optional<uaf::Audio> audio = uaf::ReadAudio(uafPath);
    if (!audio) {
        HBE_WARN("Music: stinger '{}' failed to load.", uafPath.string());
        return;
    }
    impl_->StartVoice(audio->pcm.data(), audio->pcm.size(), audio->channels,
                      audio->sampleRate, audio->bitsPerSample, bus, glm::max(volume, 0.0f),
                      1.0f, false, nullptr, 1.0f, 30.0f);
}

void AudioSystem::UpdateMusic(f32 dt) {
    if (!IsAvailable()) return;

    // Musical clock + any deferred (quantized) transition that is now due.
    impl_->musicClock += dt;
    if (impl_->musicPendingValid && impl_->musicClock >= impl_->musicPendingAt) {
        const std::string next = impl_->musicPendingState;
        const f32 fade = impl_->musicPendingFade;
        impl_->musicPendingValid = false; // clear FIRST: the calls below re-enter
        impl_->musicForceImmediate = true; // ...and must not re-defer themselves
        if (next.empty()) StopMusic(fade);
        else PlayMusicState(next, fade);
        impl_->musicForceImmediate = false;
    }

    // Dialogue ducking: attack fast (speech starts abruptly), release slow (a
    // quick release pumps audibly between lines).
    {
        const f32 floor = impl_->DuckFloor();
        const f32 target = impl_->duckActive ? floor : 1.0f;
        const f32 tau = impl_->duckActive ? impl_->musicGraph.duckAttack
                                          : impl_->musicGraph.duckRelease;
        if (tau <= 0.0f) {
            impl_->duckGain = target;
        } else {
            const f32 a = 1.0f - std::exp(-dt / tau);
            impl_->duckGain += (target - impl_->duckGain) * glm::clamp(a, 0.0f, 1.0f);
        }
    }

    const f32 step = dt / glm::max(impl_->musicFade, 0.01f); // gain change this frame
    for (auto it = impl_->musicLayers.begin(); it != impl_->musicLayers.end();) {
        Impl::MusicLayerVoice& m = *it;
        const f32 target = m.fadingOut ? 0.0f : impl_->LayerGain(m);
        m.current += glm::clamp(target - m.current, -step, step);
        // Ducking multiplies the layer's own gain, so a parameter-driven fade and
        // a duck compose instead of fighting.
        ma_sound_set_volume(&m.voice.sound, m.current * impl_->duckGain);
        if (m.fadingOut && m.current <= 0.001f) {
            ma_sound_uninit(&m.voice.sound);
            ma_audio_buffer_uninit(&m.voice.buffer);
            it = impl_->musicLayers.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::Update() {
    if (!IsAvailable()) return;
    for (auto it = impl_->voices.begin(); it != impl_->voices.end();) {
        if (ma_sound_at_end(&it->sound)) {
            ma_sound_uninit(&it->sound);
            impl_->DestroyLpf(*it);
            ma_audio_buffer_uninit(&it->buffer);
            it = impl_->voices.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::UpdateScene(Scene& scene, const std::filesystem::path& assetsDir,
                              const glm::vec3& listenerPos,
                              const glm::vec3& listenerForward, bool gamePlaying,
                              const std::function<bool(const glm::vec3&, const glm::vec3&)>& segmentBlocked,
                              f32 dt) {
    if (!IsAvailable()) return;
    const bool occlude = impl_->occlusion.enabled && static_cast<bool>(segmentBlocked);
    // Autoplay arms on the edge into "game running" (the runtime is playing from
    // frame 1; the editor only in play mode), so opening/viewing a scene in the
    // editor no longer triggers its audio.
    const bool gameStarted = gamePlaying && !prevScenePlaying_;
    prevScenePlaying_ = gamePlaying;
    auto& reg = scene.Registry();

    // The listener rides the camera.
    ma_engine_listener_set_position(&impl_->engine, 0, listenerPos.x, listenerPos.y,
                                    listenerPos.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, listenerForward.x,
                                     listenerForward.y, listenerForward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, 0.0f, 1.0f, 0.0f);
    if (impl_->useResonance && impl_->resonance.IsReady())
        impl_->resonance.SetListener(listenerPos, listenerForward, glm::vec3(0.0f, 1.0f, 0.0f));

    // Drive every AudioSource entity.
    for (const entt::entity e : reg.view<AudioSource>()) {
        AudioSource& src = reg.get<AudioSource>(e);
        if (src.asset.empty()) continue;

        // Start autoplay sources when the game begins (not when the scene is
        // merely shown in the editor). Manual playback (the inspector Play
        // button / scripts toggling `playing`) still works for preview.
        if (gameStarted && src.autoplay) src.playing = true;

        // (Re)create the voice on first sight or after an asset/bus change.
        auto it = impl_->spatial.find(src.voiceId);
        if (it != impl_->spatial.end() &&
            (it->second.asset != src.asset || it->second.bus != src.bus)) {
            impl_->DestroySpatial(it->second);
            impl_->spatial.erase(it);
            it = impl_->spatial.end();
            src.voiceId = AudioSource::kNoVoice;
        }
        if (it == impl_->spatial.end()) {
            const std::optional<uaf::Audio> audio = uaf::ReadAudio(assetsDir / src.asset);
            if (!audio) continue;

            // Closed caption: surface the asset's caption when this voice starts.
            if (subtitle::Line cap; impl_->MakeCaption(*audio, cap))
                impl_->captions.push_back(std::move(cap));

            ma_format format;
            switch (audio->bitsPerSample) {
                case 8:  format = ma_format_u8;  break;
                case 16: format = ma_format_s16; break;
                case 32: format = ma_format_f32; break;
                default: continue;
            }
            const usize bytesPerFrame = (audio->bitsPerSample / 8) * audio->channels;
            if (bytesPerFrame == 0 || audio->pcm.size() < bytesPerFrame) continue;

            const u32 id = impl_->nextSpatialId++;
            Impl::SpatialVoice& sv = impl_->spatial[id];
            sv.entity = e;
            sv.asset = src.asset;
            sv.bus = src.bus;
            sv.voice.data = audio->pcm;

            ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
                format, audio->channels, audio->pcm.size() / bytesPerFrame,
                sv.voice.data.data(), nullptr);
            cfg.sampleRate = audio->sampleRate;
            if (ma_audio_buffer_init(&cfg, &sv.voice.buffer) != MA_SUCCESS) {
                impl_->spatial.erase(id);
                continue;
            }
            // Claim a Resonance slot up front (it decides the sound flags), else miniaudio.
            if (impl_->useResonance && impl_->resonance.IsReady())
                sv.voice.resSlot = impl_->resonance.AcquireSource();
            const ma_uint32 sflags =
                sv.voice.resSlot >= 0 ? MA_SOUND_FLAG_NO_SPATIALIZATION : 0u;
            if (ma_sound_init_from_data_source(&impl_->engine, &sv.voice.buffer, sflags,
                                               impl_->GroupOf(src.bus),
                                               &sv.voice.sound) != MA_SUCCESS) {
                if (sv.voice.resSlot >= 0) impl_->resonance.ReleaseSource(sv.voice.resSlot);
                ma_audio_buffer_uninit(&sv.voice.buffer); // don't leak the buffer
                impl_->spatial.erase(id);
                continue;
            }
            if (sv.voice.resSlot < 0) {
                ma_sound_set_attenuation_model(&sv.voice.sound, ma_attenuation_model_inverse);
                ma_sound_set_spatialization_enabled(&sv.voice.sound, MA_TRUE);
            }
            sv.voice.spatial = true;
            impl_->AttachOcclusionLpf(sv.voice, src.bus); // -> Resonance node when slotted
            src.voiceId = id;
            HBE_INFO("Audio: spatial voice for '{}' ready.", src.asset);
            it = impl_->spatial.find(id);
        }

        Impl::SpatialVoice& sv = it->second;
        ma_sound* snd = &sv.voice.sound;
        sv.entity = e;

        // Follow the entity, apply component settings.
        const glm::vec3 pos = glm::vec3(scene.WorldMatrix(e)[3]);
        ma_sound_set_looping(snd, src.loop ? MA_TRUE : MA_FALSE);
        sv.voice.baseVolume = src.volume;
        sv.voice.worldPos = pos;
        if (sv.voice.resSlot >= 0) {
            // Resonance owns spatialization: feed position + volume + our multi-ray occlusion
            // scalar (the bespoke geometry probe is kept), no miniaudio panning/attenuation.
            const f32 occ =
                occlude ? impl_->ComputeOcclusion(pos, listenerPos, segmentBlocked) : 0.0f;
            impl_->resonance.SetSource(sv.voice.resSlot, pos, src.volume, occ);
        } else {
            // miniaudio panning: position + distance; occlusion attenuates + muffles when
            // geometry blocks the path, else base volume with the LPF transparent.
            ma_sound_set_position(snd, pos.x, pos.y, pos.z);
            ma_sound_set_min_distance(snd, glm::max(src.minDistance, 0.01f));
            ma_sound_set_max_distance(snd, glm::max(src.maxDistance, src.minDistance + 0.01f));
            if (occlude)
                impl_->ApplyOcclusion(
                    sv.voice, impl_->ComputeOcclusion(pos, listenerPos, segmentBlocked), dt);
            else
                impl_->ClearOcclusion(sv.voice); // occlusion off -> base volume + open LPF
        }

        const bool isPlaying = ma_sound_is_playing(snd) != MA_FALSE;
        if (src.playing && !isPlaying) {
            if (ma_sound_at_end(snd)) ma_sound_seek_to_pcm_frame(snd, 0);
            ma_sound_start(snd);
        } else if (!src.playing && isPlaying) {
            ma_sound_stop(snd);
            ma_sound_seek_to_pcm_frame(snd, 0);
        }
        // A finished non-looping sound flips the component back to stopped.
        if (src.playing && !src.loop && ma_sound_at_end(snd)) src.playing = false;
    }

    // Reap voices whose entity or AudioSource went away (or was re-created).
    for (auto vit = impl_->spatial.begin(); vit != impl_->spatial.end();) {
        const entt::entity e = vit->second.entity;
        const AudioSource* src = reg.valid(e) ? reg.try_get<AudioSource>(e) : nullptr;
        if (!src || src->voiceId != vit->first) {
            impl_->DestroySpatial(vit->second);
            vit = impl_->spatial.erase(vit);
        } else {
            ++vit;
        }
    }

    // Occlude one-shot spatial voices too (dialogue-actor voice lines, spatial
    // events): they don't move, so their emit position was captured at start. When
    // occlusion is off, restore any still-occluded voice to open (a looping spatial
    // event would otherwise stay muffled/quiet forever).
    for (Impl::Voice& v : impl_->voices) {
        if (!v.spatial) continue;
        if (v.resSlot >= 0) {
            const f32 occ =
                occlude ? impl_->ComputeOcclusion(v.worldPos, listenerPos, segmentBlocked) : 0.0f;
            impl_->resonance.SetSource(v.resSlot, v.worldPos, v.baseVolume, occ);
            continue;
        }
        if (occlude)
            impl_->ApplyOcclusion(v, impl_->ComputeOcclusion(v.worldPos, listenerPos, segmentBlocked),
                                  dt);
        else if (v.occ != 0.0f || (v.hasLpf && v.curCutoff < Impl::kOpenCutoff - 20.0f))
            impl_->ClearOcclusion(v);
    }
}

} // namespace hbe
