// Editor/SequencerEditor.cpp - the Sequencer NLE panel (editor-only).
//
// A dockable, artist-facing timeline for authoring .hbseq cinematic Sequences: a
// binding + track hierarchy, a multi-lane timeline with sections and keyframes, a
// scrubbable playhead, an inspector, and a LIVE PREVIEW that drives the scene and
// camera through the SAME runtime evaluator the shipped game uses (cine::Evaluate /
// FireEvents), so what you preview is what plays. Its own translation unit so it
// does not grow Editor.cpp past /bigobj (the DialogueEditor/UIEditor precedent).
#include "Editor/Editor.h"

#include "Cinematics/Evaluator.h"
#include "Cinematics/SequenceAsset.h"
#include "Cinematics/TrackRegistry.h"
#include "Core/Curve.h"
#include "Core/Log.h"
#include "Engine/Engine.h"
#include "Project/Project.h"
#include "Renderer/Camera.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h" // scene::SaveSceneToString

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace hbe {

namespace {
namespace fs = std::filesystem;

// A stable-ish id for a new track/section/binding (monotonic per editor session).
u64 g_seqIdCounter = 1;
u64 NextSeqId() { return ++g_seqIdCounter; }

const char* SectionKindName(cine::SectionKind k) {
    switch (k) {
        case cine::SectionKind::Keyframe:    return "Keyframe";
        case cine::SectionKind::Asset:       return "Asset";
        case cine::SectionKind::Event:       return "Event";
        case cine::SectionKind::CameraCut:   return "Camera Cut";
        case cine::SectionKind::SubSequence: return "Sub-Sequence";
    }
    return "?";
}

// Section param accessors (mutable).
f32& SecFloatRef(cine::Section& s, const char* key, f32 def) {
    for (auto& p : s.floatParams)
        if (p.first == key) return p.second;
    s.floatParams.push_back({key, def});
    return s.floatParams.back().second;
}
std::string& SecStrRef(cine::Section& s, const char* key) {
    for (auto& p : s.stringParams)
        if (p.first == key) return p.second;
    s.stringParams.push_back({key, std::string()});
    return s.stringParams.back().second;
}
} // namespace

// ---------------------------------------------------------------------------
std::filesystem::path Editor::CreateSequenceAsset(const std::filesystem::path& dir,
                                                  const std::string& name) {
    fs::path base = dir;
    if (base.empty()) {
        base = Project::HasActive() ? (Project::Active().AssetsDir() / "Sequences")
                                    : fs::path("Sequences");
    }
    std::error_code ec;
    fs::create_directories(base, ec);
    std::string stem = name.empty() ? "NewSequence" : name;
    fs::path path = base / (stem + cine::kSequenceExtension);
    for (int i = 1; fs::exists(path); ++i)
        path = base / (stem + std::to_string(i) + cine::kSequenceExtension);

    cine::Sequence seq;
    seq.name = path.stem().string();
    seq.duration = 10.0f;
    cine::SaveSequence(path, seq);
    return path;
}

void Editor::OpenSequence(Engine& engine, const std::filesystem::path& path) {
    if (seqPreview_) SequencerPreviewEnd(engine);
    auto loaded = cine::LoadSequence(path);
    if (!loaded) {
        SetSaveStatus("Could not open sequence '" + path.filename().string() + "'.", true);
        return;
    }
    editedSequence_ = std::move(*loaded);
    editedSequencePath_ = path;
    sequenceOpen_ = true;
    editedSequenceDirty_ = false;
    seqPlayhead_ = 0.0f;
    seqScroll_ = 0.0f;
    seqSelTrack_ = seqSelSection_ = seqSelBinding_ = -1;
    // Drop the previous document's undo history so Ctrl+Z cannot paste a different
    // asset over this one (the AssetHistory::Clear rule the cutscene panel documents).
    seqHistory_.Clear();
    cine::RegisterBuiltinTrackKinds();
    panelOpen_[Panel_Sequencer] = true;
    ImGui::SetWindowFocus("Sequencer");
}

bool Editor::SaveSequenceAsset() {
    if (!sequenceOpen_ || editedSequencePath_.empty()) return false;
    if (cine::SaveSequence(editedSequencePath_, editedSequence_)) {
        editedSequenceDirty_ = false;
        return true;
    }
    return false;
}

void Editor::SequencerPreviewBegin(Engine& engine) {
    if (seqPreview_ || playMode_ || !sequenceOpen_) return;
    cine::RegisterBuiltinTrackKinds();
    // Materialise streamed zones before snapshotting (see CutscenePreviewBegin: a
    // snapshot taken with a zone streamed out would delete it permanently on restore).
    CaptureSceneSettle(engine);
    seqPreviewSnapshot_ = scene::SaveSceneToString(engine.GetScene());
    seqPreviewInstance_ = cine::SequenceInstance{};
    seqPrevTime_ = seqPlayhead_;
    seqPreview_ = true;
}

void Editor::SequencerPreviewEnd(Engine& engine) {
    if (!seqPreview_) return;
    seqPreview_ = false;
    seqPlaying_ = false;
    seqPreviewInstance_.Release(engine.GetScene());
    if (!seqPreviewSnapshot_.empty()) RestoreSnapshot(engine, seqPreviewSnapshot_);
    seqPreviewSnapshot_.clear();
}

// ---------------------------------------------------------------------------
void Editor::DrawSequencer(Engine& engine) {
    if (!panelOpen_[Panel_Sequencer]) {
        if (seqPreview_) SequencerPreviewEnd(engine);
        return;
    }
    const bool visible = ImGui::Begin("Sequencer", &panelOpen_[Panel_Sequencer]);
    // Claim the Ctrl+S / Ctrl+Z chords unconditionally so a focused-but-empty panel
    // says "nothing open" instead of letting the chord fall through and write the level.
    ClaimFocus(editor::SaveSurface::Sequence);
    if (!visible) {
        if (seqPreview_) SequencerPreviewEnd(engine);
        ImGui::End();
        return;
    }
    cine::RegisterBuiltinTrackKinds();

    auto pushUndo = [&]() {
        seqHistory_.Push(editedSequence_);
        editedSequenceDirty_ = true;
    };

    // --- No sequence open: New + an Open list ---------------------------------
    if (!sequenceOpen_) {
        ImGui::TextWrapped("No sequence open. Create a new .hbseq or open an existing one.");
        if (ImGui::Button("New Sequence")) {
            fs::path p = CreateSequenceAsset({});
            OpenSequence(engine, p);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Open:");
        if (Project::HasActive()) {
            const fs::path root = Project::Active().AssetsDir();
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                if (it->path().extension() != cine::kSequenceExtension) continue;
                const std::string rel = fs::relative(it->path(), root, ec).string();
                if (ImGui::Selectable(rel.c_str())) OpenSequence(engine, it->path());
            }
        }
        ImGui::End();
        return;
    }

    cine::Sequence& seq = editedSequence_;

    // --- Toolbar --------------------------------------------------------------
    if (ImGui::Button("Save")) {
        if (SaveSequenceAsset())
            SetSaveStatus("Saved sequence '" + editedSequencePath_.filename().string() + "'.", false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        if (seqPreview_) SequencerPreviewEnd(engine);
        sequenceOpen_ = false;
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(seqHistory_.undo.empty());
    if (ImGui::Button("Undo")) { seqHistory_.Undo(editedSequence_); seqSelTrack_ = seqSelSection_ = -1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(seqHistory_.redo.empty());
    if (ImGui::Button("Redo")) { seqHistory_.Redo(editedSequence_); seqSelTrack_ = seqSelSection_ = -1; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    if (!seqPreview_) {
        if (ImGui::Button("Preview")) SequencerPreviewBegin(engine);
    } else {
        if (ImGui::Button(seqPlaying_ ? "Pause" : "Play")) seqPlaying_ = !seqPlaying_;
        ImGui::SameLine();
        if (ImGui::Button("Stop")) { SequencerPreviewEnd(engine); seqPlayhead_ = 0.0f; }
    }
    ImGui::SameLine();
    ImGui::Text("| %.2f / %.2f s%s", seqPlayhead_, seq.Length(), editedSequenceDirty_ ? " *" : "");

    // Playback range + frame rate.
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputFloat("Duration", &seq.duration, 0.0f, 0.0f, "%.2f")) editedSequenceDirty_ = true;
    if (ImGui::IsItemActivated()) pushUndo();
    ImGui::SameLine();
    bool suppress = seq.suppressGameplay;
    if (ImGui::Checkbox("Cinematic mode (freeze gameplay)", &suppress)) { pushUndo(); seq.suppressGameplay = suppress; }

    ImGui::Separator();

    // --- Left: bindings + tracks | Right: timeline ----------------------------
    const float leftW = 260.0f;
    ImGui::BeginChild("seq_left", ImVec2(leftW, 0), true);

    // Bindings.
    ImGui::SeparatorText("Bindings");
    if (ImGui::SmallButton("+ Binding")) {
        pushUndo();
        cine::Binding b;
        b.id = 1;
        for (const auto& eb : seq.bindings) b.id = std::max(b.id, eb.id + 1);
        b.label = "Actor";
        seq.bindings.push_back(b);
        seqSelBinding_ = static_cast<int>(seq.bindings.size()) - 1;
    }
    for (int i = 0; i < static_cast<int>(seq.bindings.size()); ++i) {
        cine::Binding& b = seq.bindings[i];
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "[%d] %s##bind%d", b.id,
                      b.label.empty() ? "(binding)" : b.label.c_str(), i);
        if (ImGui::Selectable(lbl, seqSelBinding_ == i)) {
            seqSelBinding_ = i;
            seqSelTrack_ = seqSelSection_ = -1;
        }
    }

    // Tracks (top-level lanes).
    ImGui::SeparatorText("Tracks");
    if (ImGui::SmallButton("+ Track")) ImGui::OpenPopup("add_track");
    if (ImGui::BeginPopup("add_track")) {
        for (const cine::TrackKind& k : cine::TrackKinds()) {
            if (ImGui::MenuItem(k.display.c_str())) {
                pushUndo();
                cine::Track t;
                t.id = NextSeqId();
                t.kind = k.id;
                t.name = k.display;
                seq.tracks.push_back(t);
                seqSelTrack_ = static_cast<int>(seq.tracks.size()) - 1;
                seqSelSection_ = seqSelBinding_ = -1;
            }
        }
        ImGui::EndPopup();
    }
    for (int i = 0; i < static_cast<int>(seq.tracks.size()); ++i) {
        cine::Track& t = seq.tracks[i];
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "%s%s##trk%d", t.mute ? "[M] " : "",
                      t.name.empty() ? t.kind.c_str() : t.name.c_str(), i);
        if (ImGui::Selectable(lbl, seqSelTrack_ == i && seqSelSection_ < 0)) {
            seqSelTrack_ = i;
            seqSelSection_ = -1;
            seqSelBinding_ = -1;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Timeline lanes.
    ImGui::BeginChild("seq_timeline", ImVec2(0, -180.0f), true);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float rulerH = 20.0f;
        const float laneH = 24.0f;

        auto timeToX = [&](f32 t) { return origin.x + (t - seqScroll_) * seqZoom_; };
        auto xToTime = [&](float x) { return seqScroll_ + (x - origin.x) / seqZoom_; };

        // Interaction surface (ruler + lanes).
        const int nLanes = static_cast<int>(seq.tracks.size());
        const float totalH = rulerH + nLanes * laneH + laneH;
        ImGui::InvisibleButton("seq_canvas", ImVec2(width, totalH),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                                   ImGuiButtonFlags_MouseButtonRight);
        const bool canvasHovered = ImGui::IsItemHovered();
        ImGuiIO& io = ImGui::GetIO();

        // Wheel = zoom to cursor; middle-drag = pan.
        if (canvasHovered && io.MouseWheel != 0.0f) {
            const f32 tCursor = xToTime(io.MousePos.x);
            seqZoom_ = std::clamp(seqZoom_ * (io.MouseWheel > 0 ? 1.1f : 0.9f), 8.0f, 2000.0f);
            seqScroll_ = tCursor - (io.MousePos.x - origin.x) / seqZoom_;
        }
        if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            seqScroll_ -= io.MouseDelta.x / seqZoom_;
        seqScroll_ = std::max(0.0f, seqScroll_);

        // Ruler background + ticks.
        dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + rulerH), IM_COL32(40, 40, 46, 255));
        const f32 t0 = std::max(0.0f, xToTime(origin.x));
        const f32 t1 = xToTime(origin.x + width);
        f32 step = 1.0f;
        while ((step * seqZoom_) < 60.0f) step *= 2.0f;
        while ((step * seqZoom_) > 200.0f) step *= 0.5f;
        for (f32 t = std::floor(t0 / step) * step; t <= t1; t += step) {
            const float x = timeToX(t);
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + totalH), IM_COL32(60, 60, 68, 120));
            char tb[32];
            std::snprintf(tb, sizeof(tb), "%.2f", t);
            dl->AddText(ImVec2(x + 2, origin.y + 3), IM_COL32(170, 170, 180, 255), tb);
        }

        // Lanes + sections.
        for (int i = 0; i < nLanes; ++i) {
            const float ly = origin.y + rulerH + i * laneH;
            const bool sel = (seqSelTrack_ == i);
            dl->AddRectFilled(ImVec2(origin.x, ly), ImVec2(origin.x + width, ly + laneH - 1),
                              sel ? IM_COL32(48, 52, 64, 255) : IM_COL32(34, 34, 40, 255));
            cine::Track& t = seq.tracks[i];
            for (int s = 0; s < static_cast<int>(t.sections.size()); ++s) {
                cine::Section& sec = t.sections[s];
                const float x0 = timeToX(sec.start);
                const float x1 = timeToX(sec.End());
                const bool secSel = sel && seqSelSection_ == s;
                dl->AddRectFilled(ImVec2(x0, ly + 2), ImVec2(std::max(x1, x0 + 3), ly + laneH - 3),
                                  secSel ? IM_COL32(90, 130, 200, 230) : IM_COL32(70, 90, 140, 200), 3.0f);
                // Keyframe diamonds for keyframe sections.
                for (const auto& ch : sec.channels) {
                    for (const auto& k : ch.curve.keys) {
                        const float kx = timeToX(sec.start + (k.time - sec.innerStart) / std::max(0.001f, sec.timeScale));
                        const float ky = ly + laneH * 0.5f;
                        dl->AddTriangleFilled(ImVec2(kx, ky - 4), ImVec2(kx + 4, ky), ImVec2(kx, ky + 4),
                                              IM_COL32(230, 220, 120, 255));
                        dl->AddTriangleFilled(ImVec2(kx, ky - 4), ImVec2(kx - 4, ky), ImVec2(kx, ky + 4),
                                              IM_COL32(230, 220, 120, 255));
                    }
                }
                if (x1 > x0 + 20)
                    dl->AddText(ImVec2(x0 + 4, ly + 4), IM_COL32(230, 230, 235, 255),
                                SectionKindName(sec.kind));
            }
        }

        // Markers on the ruler.
        for (const auto& m : seq.markers) {
            const float x = timeToX(m.time);
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + totalH),
                        IM_COL32((m.color >> 16) & 0xFF, (m.color >> 8) & 0xFF, m.color & 0xFF, 200), 1.5f);
        }

        // Playhead.
        const float px = timeToX(seqPlayhead_);
        dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + totalH), IM_COL32(230, 90, 90, 255), 2.0f);

        // Click handling: ruler = scrub; lane = select/add section.
        if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const float my = io.MousePos.y - origin.y;
            if (my < rulerH) {
                seqPlayhead_ = std::clamp(xToTime(io.MousePos.x), 0.0f, seq.Length());
            } else {
                const int lane = static_cast<int>((my - rulerH) / laneH);
                if (lane >= 0 && lane < nLanes) {
                    seqSelTrack_ = lane;
                    seqSelBinding_ = -1;
                    seqSelSection_ = -1;
                    const f32 ct = xToTime(io.MousePos.x);
                    cine::Track& t = seq.tracks[lane];
                    for (int s = 0; s < static_cast<int>(t.sections.size()); ++s)
                        if (ct >= t.sections[s].start && ct <= t.sections[s].End()) { seqSelSection_ = s; break; }
                }
            }
        }
        // Drag on the ruler scrubs continuously.
        if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
            (io.MousePos.y - origin.y) < rulerH)
            seqPlayhead_ = std::clamp(xToTime(io.MousePos.x), 0.0f, seq.Length());

        // Right-click a lane -> add a section at that time.
        if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const float my = io.MousePos.y - origin.y;
            const int lane = static_cast<int>((my - rulerH) / laneH);
            if (my >= rulerH && lane >= 0 && lane < nLanes) {
                seqSelTrack_ = lane;
                seqAddSectionTime_ = std::max(0.0f, xToTime(io.MousePos.x));
                ImGui::OpenPopup("seq_lane_ctx");
            }
        }
        if (ImGui::BeginPopup("seq_lane_ctx")) {
            if (ImGui::MenuItem("Add Section here") && seqSelTrack_ >= 0 &&
                seqSelTrack_ < static_cast<int>(seq.tracks.size())) {
                pushUndo();
                cine::Section sec;
                sec.id = NextSeqId();
                sec.start = seqAddSectionTime_;
                sec.duration = 2.0f;
                seq.tracks[seqSelTrack_].sections.push_back(sec);
                seqSelSection_ = static_cast<int>(seq.tracks[seqSelTrack_].sections.size()) - 1;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    // --- Inspector ------------------------------------------------------------
    ImGui::BeginChild("seq_inspector", ImVec2(0, 0), true);
    DrawSequencerInspector(engine);
    ImGui::EndChild();

    // --- Live preview evaluation (after the UI, so it can override the camera) -
    if (seqPreview_) {
        if (seqPlaying_) {
            seqPrevTime_ = seqPlayhead_;
            seqPlayhead_ += ImGui::GetIO().DeltaTime;
            if (seqPlayhead_ >= seq.Length()) { seqPlayhead_ = seq.Length(); seqPlaying_ = false; }
        }
        cine::EvalContext ctx;
        ctx.scene = &engine.GetScene();
        ctx.camera = &engine.GetRenderer().GetCamera();
        ctx.post = &engine.GetScene().Environment().post;
        ctx.assetsDir = Project::HasActive() ? Project::Active().AssetsDir() : fs::path();
        ctx.mode = cine::EvalMode::Preview;
        ctx.applyCamera = true;
        ctx.fireDeferred = false;  // editor preview must NOT queue game:: side effects
        ctx.applyGameplay = false;
        ctx.t = seqPlayhead_;
        ctx.prevT = seqPrevTime_;
        ctx.dt = ImGui::GetIO().DeltaTime;
        if (seqPlaying_) cine::FireEvents(seq, seqPreviewInstance_, ctx);
        cine::Evaluate(seq, seqPreviewInstance_, ctx);
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
void Editor::DrawSequencerInspector(Engine& engine) {
    cine::Sequence& seq = editedSequence_;
    auto pushUndo = [&]() { seqHistory_.Push(editedSequence_); editedSequenceDirty_ = true; };
    auto editStr = [](const char* label, std::string& s) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", s.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf))) { s = buf; return true; }
        return false;
    };

    // ---- Binding ----
    if (seqSelBinding_ >= 0 && seqSelBinding_ < static_cast<int>(seq.bindings.size())) {
        cine::Binding& b = seq.bindings[seqSelBinding_];
        ImGui::SeparatorText("Binding");
        if (editStr("Label", b.label)) editedSequenceDirty_ = true;
        if (ImGui::IsItemActivated()) pushUndo();
        int kind = static_cast<int>(b.kind);
        if (ImGui::Combo("Kind", &kind, "Possessable\0Spawnable\0")) { pushUndo(); b.kind = static_cast<cine::BindingKind>(kind); }
        if (b.kind == cine::BindingKind::Possessable) {
            std::vector<std::string> names;
            for (auto e : engine.GetScene().Registry().view<Name>())
                names.push_back(engine.GetScene().Registry().get<Name>(e).value);
            std::string out;
            if (SearchableStringCombo("Entity", b.name, names, out, "Bind to a scene entity by Name")) {
                pushUndo();
                b.name = out;
                entt::entity e = engine.GetScene().FindByName(out);
                if (e != entt::null && engine.GetScene().Registry().all_of<Guid>(e))
                    b.guid = engine.GetScene().Registry().get<Guid>(e).value;
            }
            ImGui::Text("guid: %016llx", static_cast<unsigned long long>(b.guid));
        } else {
            if (editStr("Spawn Asset", b.spawnAsset)) editedSequenceDirty_ = true;
            if (ImGui::IsItemActivated()) pushUndo();
        }
        if (ImGui::Button("Delete Binding")) {
            pushUndo();
            seq.bindings.erase(seq.bindings.begin() + seqSelBinding_);
            seqSelBinding_ = -1;
        }
        return;
    }

    // ---- Nothing / markers ----
    if (seqSelTrack_ < 0 || seqSelTrack_ >= static_cast<int>(seq.tracks.size())) {
        ImGui::TextDisabled("Select a binding, track, or section to edit.");
        ImGui::SeparatorText("Markers");
        if (ImGui::Button("Add Marker at playhead")) {
            pushUndo();
            cine::Marker m;
            m.time = seqPlayhead_;
            m.name = "Marker";
            seq.markers.push_back(m);
        }
        for (int i = 0; i < static_cast<int>(seq.markers.size()); ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(140);
            editStr("##mname", seq.markers[i].name);
            ImGui::SameLine();
            ImGui::Text("@ %.2f", seq.markers[i].time);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { pushUndo(); seq.markers.erase(seq.markers.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        return;
    }

    // ---- Track ----
    cine::Track& tr = seq.tracks[seqSelTrack_];
    ImGui::SeparatorText("Track");
    if (editStr("Name", tr.name)) editedSequenceDirty_ = true;
    if (ImGui::IsItemActivated()) pushUndo();
    ImGui::Text("Kind: %s", tr.kind.c_str());
    {
        std::string cur = "(none)";
        if (const cine::Binding* b = seq.FindBinding(tr.binding)) cur = b->label;
        if (ImGui::BeginCombo("Binding", cur.c_str())) {
            if (ImGui::Selectable("(none)", tr.binding < 0)) { pushUndo(); tr.binding = -1; }
            for (auto& b : seq.bindings)
                if (ImGui::Selectable(b.label.c_str(), tr.binding == b.id)) { pushUndo(); tr.binding = b.id; }
            ImGui::EndCombo();
        }
    }
    bool mute = tr.mute, solo = tr.solo, lock = tr.locked;
    if (ImGui::Checkbox("Mute", &mute)) { pushUndo(); tr.mute = mute; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Solo", &solo)) { pushUndo(); tr.solo = solo; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Lock", &lock)) { pushUndo(); tr.locked = lock; }
    if (ImGui::Button("Delete Track")) {
        pushUndo();
        seq.tracks.erase(seq.tracks.begin() + seqSelTrack_);
        seqSelTrack_ = seqSelSection_ = -1;
        return;
    }

    // ---- Section ----
    if (seqSelSection_ < 0 || seqSelSection_ >= static_cast<int>(tr.sections.size())) return;
    cine::Section& sec = tr.sections[seqSelSection_];
    ImGui::SeparatorText("Section");
    auto dragF = [&](const char* l, f32* v) {
        if (ImGui::DragFloat(l, v, 0.02f)) editedSequenceDirty_ = true;
        if (ImGui::IsItemActivated()) pushUndo();
    };
    dragF("Start", &sec.start);
    dragF("Duration", &sec.duration);
    dragF("Time Scale", &sec.timeScale);
    dragF("Inner Start", &sec.innerStart);
    dragF("Blend In", &sec.blendIn);
    dragF("Blend Out", &sec.blendOut);
    int sk = static_cast<int>(sec.kind);
    if (ImGui::Combo("Section Kind", &sk, "Keyframe\0Asset\0Event\0Camera Cut\0Sub-Sequence\0")) {
        pushUndo();
        sec.kind = static_cast<cine::SectionKind>(sk);
    }

    const std::string& kind = tr.kind;
    if (sec.kind == cine::SectionKind::Asset || sec.kind == cine::SectionKind::SubSequence ||
        kind == "audio" || kind == "dialogue") {
        if (editStr("Asset Ref (rel. Assets)", sec.assetRef)) editedSequenceDirty_ = true;
        if (ImGui::IsItemActivated()) pushUndo();
    }
    if (kind == "event") {
        editStr("Event", SecStrRef(sec, "event"));
        editStr("flag/id", SecStrRef(sec, "flag"));
        ImGui::DragFloat("value", &SecFloatRef(sec, "value", 1.0f), 0.1f);
    } else if (kind == "music") {
        editStr("state", SecStrRef(sec, "state"));
        editStr("parameter", SecStrRef(sec, "parameter"));
        ImGui::DragFloat("param value", &SecFloatRef(sec, "value", 1.0f), 0.05f);
    } else if (kind == "subtitle") {
        editStr("speaker", SecStrRef(sec, "speaker"));
        editStr("text", SecStrRef(sec, "text"));
    } else if (kind == "shake") {
        ImGui::SliderFloat("trauma", &SecFloatRef(sec, "trauma", 0.5f), 0.0f, 1.0f);
    } else if (kind == "spawn") {
        editStr("spawner id", SecStrRef(sec, "spawner"));
        editStr("op (spawn/despawn)", SecStrRef(sec, "op"));
    } else if (kind == "animation") {
        int ci = static_cast<int>(SecFloatRef(sec, "clip", 0.0f));
        if (ImGui::InputInt("clip index", &ci)) { SecFloatRef(sec, "clip", 0.0f) = static_cast<f32>(ci); editedSequenceDirty_ = true; }
    } else if (kind == "visibility") {
        bool v = SecFloatRef(sec, "visible", 0.0f) > 0.5f;
        if (ImGui::Checkbox("visible during section", &v)) SecFloatRef(sec, "visible", 0.0f) = v ? 1.0f : 0.0f;
    }
    if (sec.kind == cine::SectionKind::CameraCut) {
        std::string cur = "(none)";
        if (const cine::Binding* b = seq.FindBinding(sec.bindingRef)) cur = b->label;
        if (ImGui::BeginCombo("Camera Binding", cur.c_str())) {
            for (auto& b : seq.bindings)
                if (ImGui::Selectable(b.label.c_str(), sec.bindingRef == b.id)) { pushUndo(); sec.bindingRef = b.id; }
            ImGui::EndCombo();
        }
    }

    // Keyframe channels.
    if (sec.kind == cine::SectionKind::Keyframe) {
        ImGui::SeparatorText("Channels");
        const f32 contentT = sec.innerStart + (seqPlayhead_ - sec.start) * sec.timeScale;
        auto addKey = [&](const char* target, f32 val) {
            cine::Channel* ch = nullptr;
            for (auto& c : sec.channels)
                if (c.target == target) ch = &c;
            if (!ch) { sec.channels.push_back({target, {}}); ch = &sec.channels.back(); }
            curve::Insert(ch->curve, contentT, val);
        };
        if (kind == "camera" && ImGui::Button("Capture camera @ playhead")) {
            pushUndo();
            Camera& cam = engine.GetRenderer().GetCamera();
            const glm::vec3 eye = cam.Position();
            const glm::vec3 aim = cam.Position() + cam.Forward();
            addKey("location.x", eye.x); addKey("location.y", eye.y); addKey("location.z", eye.z);
            addKey("aim.x", aim.x); addKey("aim.y", aim.y); addKey("aim.z", aim.z);
            addKey("fov", glm::degrees(cam.FovY()));
        }
        static char chTarget[64] = "location.x";
        static float chVal = 0.0f;
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("target", chTarget, sizeof(chTarget));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputFloat("v", &chVal);
        ImGui::SameLine();
        if (ImGui::Button("Add Key")) { pushUndo(); addKey(chTarget, chVal); }
        for (auto& ch : sec.channels)
            ImGui::BulletText("%s (%zu keys)", ch.target.c_str(), ch.curve.keys.size());
    }

    if (ImGui::Button("Delete Section")) {
        pushUndo();
        tr.sections.erase(tr.sections.begin() + seqSelSection_);
        seqSelSection_ = -1;
    }
}

} // namespace hbe
