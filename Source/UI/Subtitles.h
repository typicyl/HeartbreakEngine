// UI/Subtitles.h - the ONE subtitle / closed-caption pipeline.
//
// Everything that puts words on screen because of AUDIO goes through here:
// conversation lines from the dialogue graph, cutscene dialogue markers,
// schematic PlayVoiceline, and the baked captions on .uaf audio assets played by
// an AudioSource. They used to be two unrelated paths that each hand-built a
// "Speaker: text" string and threw the audio's KIND away, so a spoken story line
// and a door creak were indistinguishable by the time they reached the screen -
// which made it impossible to gate or style them separately.
//
// A Line keeps its parts (speaker / text / kind) intact until composition, so:
//   * ACCESSIBILITY GATING is correct. The convention these follow:
//       - "Subtitles"       = speech only (Dialogue, Voiceline)
//       - "Closed captions" = speech PLUS non-speech audio (Sound, Ambient)
//     They are independent toggles, so a player can have story dialogue without
//     a running commentary of every footstep.
//   * FORMATTING is per-kind and applied in one place ("Ana: over here" vs the
//     bracketed "[door creaks]" convention for non-speech).
//   * PRIORITY is meaningful: a burst of ambient captions can never evict the
//     story line the player actually needs to read.
#pragma once

#include "Core/Types.h"

#include <string>
#include <vector>

namespace hbe::subtitle {

// What produced this line. Drives both which user setting gates it and how it
// renders.
enum class Kind : u8 {
    Dialogue = 0, // a conversation line (dialogue graph / cutscene)
    Voiceline,    // an incidental spoken one-shot (bark, radio call)
    Sound,        // non-speech sound effect caption ("[door creaks]")
    Ambient,      // non-speech ambient bed ("[rain on metal]")
};

// True when `k` is speech (gated by the Subtitles setting rather than Captions).
inline bool IsSpeech(Kind k) { return k == Kind::Dialogue || k == Kind::Voiceline; }

const char* KindName(Kind k);

// One line queued for display. `duration` 0 means "derive a readable dwell from
// the text length" (see Stack::Push).
struct Line {
    std::string speaker; // "" = no speaker prefix
    std::string text;
    Kind kind = Kind::Dialogue;
    f32 duration = 0.0f;
    int priority = 0; // higher survives when the stack is full; see Push
};

// Which categories the player wants on screen. Mirrors UserSettings; passed in
// so this module stays free of engine/global state.
struct Settings {
    bool subtitles = true; // speech (Dialogue / Voiceline)
    bool captions = false; // non-speech (Sound / Ambient)
    // Show "Speaker:" in front of dialogue lines. Off = bare text.
    bool speakerNames = true;
    // Max lines on screen at once; the lowest-priority (then oldest) line drops.
    int maxLines = 4;
};

// The active on-screen stack. Owned by the Engine, ticked once a frame.
class Stack {
public:
    void SetSettings(const Settings& s) { settings_ = s; }
    const Settings& GetSettings() const { return settings_; }

    // Queues a line. Silently drops it when its category is disabled. Repeating
    // the SAME line while it is still on screen (a looping emitter re-triggering,
    // an NPC repeating a bark) REFRESHES its timer instead of stacking duplicates.
    void Push(Line line);

    // Convenience for plain text with no speaker.
    void Push(const std::string& text, Kind kind, f32 duration = 0.0f);

    // Ages out expired lines.
    void Update(f32 dt);

    void Clear() { active_.clear(); }
    bool Empty() const { return active_.empty(); }

    // The composed on-screen block: oldest first, one line each, formatted by
    // kind. Rebuilt only when the set of visible lines actually changes, so the
    // per-frame cost of an unchanged stack is a bool test.
    const std::string& Composed() const { return composed_; }

    // Seconds until the LAST line expires. The block fades on this rather than on
    // the newest line, so an old line still on screen is never faded out early.
    f32 LongestRemaining() const;

    // Readable dwell time for a piece of text (~200 wpm, clamped).
    static f32 AutoDuration(const std::string& text);

    // How `line` renders, e.g. "Ana: over here" or "[door creaks]".
    static std::string Format(const Line& line, bool speakerNames);

private:
    struct Active {
        Line line;
        f32 timer = 0.0f;
    };
    void Recompose();

    std::vector<Active> active_;
    Settings settings_;
    std::string composed_;
    bool dirty_ = false;
};

} // namespace hbe::subtitle
