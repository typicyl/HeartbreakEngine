// Audio/AudioSystem.cpp - miniaudio-backed implementation.
#include "Audio/AudioSystem.h"

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
#include <deque>
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
    struct Voice {
        std::vector<u8> data;
        ma_audio_buffer buffer{};
        ma_sound sound{};
        u32 id = 0; // 0 = anonymous (PlayPCM); event voices get an id
    };
    std::list<Voice> voices;
    u32 nextVoiceId = 1;

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
        ma_sound_uninit(&sv.voice.sound);
        ma_audio_buffer_uninit(&sv.voice.buffer);
    }

    void DestroyVoices() {
        for (Voice& v : voices) {
            ma_sound_uninit(&v.sound);
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

    // Closed captions: pending texts enqueued when a captioned sound starts.
    bool captionsEnabled = false;
    std::deque<std::string> captions;

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
        const ma_uint32 flags = position ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (ma_sound_init_from_data_source(&engine, &v.buffer, flags, GroupOf(bus),
                                           &v.sound) != MA_SUCCESS) {
            ma_audio_buffer_uninit(&v.buffer);
            voices.pop_back();
            return nullptr;
        }
        ma_sound_set_volume(&v.sound, volume);
        ma_sound_set_pitch(&v.sound, glm::max(pitch, 0.05f));
        ma_sound_set_looping(&v.sound, loop ? MA_TRUE : MA_FALSE);
        if (position) {
            ma_sound_set_position(&v.sound, position->x, position->y, position->z);
            ma_sound_set_attenuation_model(&v.sound, ma_attenuation_model_inverse);
            ma_sound_set_min_distance(&v.sound, glm::max(minDist, 0.01f));
            ma_sound_set_max_distance(&v.sound, glm::max(maxDist, minDist + 0.01f));
        }
        ma_sound_start(&v.sound);
        return &v;
    }

    ~Impl() {
        DestroyVoices();
        DestroyMusic();
        for (auto& [id, sv] : spatial) DestroySpatial(sv);
        spatial.clear();
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

bool AudioSystem::PopCaption(std::string& out) {
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
    // Surface a baked caption ("Speaker: text") so a voiceline played this way
    // (e.g. a schematic PlayVoiceline) captions just like a scene AudioSource.
    // UI/SFX clips have empty captions, so this is a no-op for them.
    if (caption && impl_ && impl_->captionsEnabled && !a.caption.empty())
        impl_->captions.push_back(a.speaker.empty() ? a.caption
                                                    : a.speaker + ": " + a.caption);
    return PlayPCM(a.pcm.data(), a.pcm.size(), a.channels, a.sampleRate,
                   a.bitsPerSample, bus);
}

void AudioSystem::ClearUAFCache() {
    if (impl_) impl_->uafCache.clear();
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
        Impl::MusicLayerVoice mv;
        mv.baseVolume = glm::max(layer.volume, 0.0f);
        mv.parameter = layer.parameter;
        mv.paramLo = layer.paramLo;
        mv.paramHi = layer.paramHi;
        mv.current = 0.0f;
        if (!impl_->StartMusicVoice(mv.voice, *audio)) continue;
        impl_->musicLayers.push_back(std::move(mv));
    }
    impl_->musicState = state;
    HBE_INFO("Music: -> '{}' ({} layers, {:.1f}s fade).", state, st->layers.size(),
             impl_->musicFade);
}

void AudioSystem::StopMusic(f32 fadeSeconds) {
    if (!IsAvailable()) return;
    impl_->musicFade =
        glm::max(fadeSeconds < 0.0f ? impl_->musicGraph.defaultFade : fadeSeconds, 0.01f);
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
    const f32 step = dt / glm::max(impl_->musicFade, 0.01f); // gain change this frame
    for (auto it = impl_->musicLayers.begin(); it != impl_->musicLayers.end();) {
        Impl::MusicLayerVoice& m = *it;
        const f32 target = m.fadingOut ? 0.0f : impl_->LayerGain(m);
        m.current += glm::clamp(target - m.current, -step, step);
        ma_sound_set_volume(&m.voice.sound, m.current);
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
            ma_audio_buffer_uninit(&it->buffer);
            it = impl_->voices.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::UpdateScene(Scene& scene, const std::filesystem::path& assetsDir,
                              const glm::vec3& listenerPos,
                              const glm::vec3& listenerForward, bool gamePlaying) {
    if (!IsAvailable()) return;
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

            // Closed caption: surface the asset's caption when this voice starts,
            // prefixed with the speaker name when set ("Speaker: caption").
            if (impl_->captionsEnabled && !audio->caption.empty())
                impl_->captions.push_back(audio->speaker.empty()
                                              ? audio->caption
                                              : audio->speaker + ": " + audio->caption);

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
            if (ma_audio_buffer_init(&cfg, &sv.voice.buffer) != MA_SUCCESS ||
                ma_sound_init_from_data_source(&impl_->engine, &sv.voice.buffer, 0,
                                               impl_->GroupOf(src.bus),
                                               &sv.voice.sound) != MA_SUCCESS) {
                impl_->spatial.erase(id);
                continue;
            }
            ma_sound_set_attenuation_model(&sv.voice.sound, ma_attenuation_model_inverse);
            ma_sound_set_spatialization_enabled(&sv.voice.sound, MA_TRUE);
            src.voiceId = id;
            HBE_INFO("Audio: spatial voice for '{}' ready.", src.asset);
            it = impl_->spatial.find(id);
        }

        Impl::SpatialVoice& sv = it->second;
        ma_sound* snd = &sv.voice.sound;
        sv.entity = e;

        // Follow the entity, apply component settings.
        const glm::vec3 pos = glm::vec3(scene.WorldMatrix(e)[3]);
        ma_sound_set_position(snd, pos.x, pos.y, pos.z);
        ma_sound_set_volume(snd, src.volume);
        ma_sound_set_looping(snd, src.loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_min_distance(snd, glm::max(src.minDistance, 0.01f));
        ma_sound_set_max_distance(snd, glm::max(src.maxDistance, src.minDistance + 0.01f));

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
}

} // namespace hbe
