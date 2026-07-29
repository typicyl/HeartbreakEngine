// UI/Subtitles.cpp
#include "UI/Subtitles.h"

#include <algorithm>

namespace hbe::subtitle {

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Dialogue:  return "Dialogue";
        case Kind::Voiceline: return "Voiceline";
        case Kind::Sound:     return "Sound";
        case Kind::Ambient:   return "Ambient";
    }
    return "Dialogue";
}

f32 Stack::AutoDuration(const std::string& text) {
    // ~200 wpm reading speed with a floor for very short lines ("Go!") and a
    // ceiling so one long line can't hold the screen for an age.
    return std::clamp(1.8f + 0.05f * static_cast<f32>(text.size()), 2.5f, 9.0f);
}

std::string Stack::Format(const Line& line, bool speakerNames) {
    if (line.text.empty()) return {};
    if (IsSpeech(line.kind)) {
        if (speakerNames && !line.speaker.empty()) return line.speaker + ": " + line.text;
        return line.text;
    }
    // Non-speech closed captions use the bracketed convention. Text authored with
    // its own brackets is left alone so "[distant siren]" doesn't become
    // "[[distant siren]]".
    if (line.text.front() == '[' && line.text.back() == ']') return line.text;
    return "[" + line.text + "]";
}

void Stack::Push(Line line) {
    if (line.text.empty()) return;
    // Accessibility gating: speech follows the Subtitles setting, non-speech the
    // Closed Captions setting. They are independent on purpose.
    const bool allowed = IsSpeech(line.kind) ? settings_.subtitles : settings_.captions;
    if (!allowed) return;

    if (line.duration <= 0.0f) line.duration = AutoDuration(line.text);

    // Re-triggering a line already on screen (a looping emitter, a repeated bark)
    // refreshes its dwell instead of stacking a duplicate.
    for (Active& a : active_) {
        if (a.line.kind == line.kind && a.line.text == line.text &&
            a.line.speaker == line.speaker) {
            a.timer = std::max(a.timer, line.duration);
            return;
        }
    }

    active_.push_back({std::move(line), 0.0f});
    active_.back().timer = active_.back().line.duration;

    // Over the cap, drop the WEAKEST line rather than blindly the oldest, so a
    // flurry of ambient captions can't push a story line off the screen.
    const usize cap = static_cast<usize>(std::max(settings_.maxLines, 1));
    while (active_.size() > cap) {
        auto weakest = active_.begin();
        for (auto it = active_.begin(); it != active_.end(); ++it) {
            if (it->line.priority < weakest->line.priority) weakest = it;
            // Same priority: the one closest to expiring goes first.
            else if (it->line.priority == weakest->line.priority && it->timer < weakest->timer)
                weakest = it;
        }
        active_.erase(weakest);
    }
    dirty_ = true;
}

void Stack::Push(const std::string& text, Kind kind, f32 duration) {
    Line l;
    l.text = text;
    l.kind = kind;
    l.duration = duration;
    Push(std::move(l));
}

void Stack::Update(f32 dt) {
    if (active_.empty()) {
        if (dirty_) Recompose();
        return;
    }
    const usize before = active_.size();
    for (Active& a : active_) a.timer -= dt;
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                 [](const Active& a) { return a.timer <= 0.0f; }),
                  active_.end());
    if (active_.size() != before) dirty_ = true;
    if (dirty_) Recompose();
}

f32 Stack::LongestRemaining() const {
    f32 longest = 0.0f;
    for (const Active& a : active_) longest = std::max(longest, a.timer);
    return longest;
}

void Stack::Recompose() {
    composed_.clear();
    for (const Active& a : active_) {
        const std::string s = Format(a.line, settings_.speakerNames);
        if (s.empty()) continue;
        if (!composed_.empty()) composed_ += '\n';
        composed_ += s;
    }
    dirty_ = false;
}

} // namespace hbe::subtitle
