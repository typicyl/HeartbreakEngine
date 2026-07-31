// Editor/SaveDispatch.cpp - the Ctrl+S decision, and its proof.
#include "Editor/SaveDispatch.h"

#include <cstdio>
#include <cstring>

namespace hbe::editor {

SaveAction DecideSave(const SaveContext& ctx) {
    // 1. NOTHING IS SAVABLE WITHOUT A PROJECT. Mirrors SaveAll's own first guard;
    //    every path below resolves a file under Assets/.
    if (!ctx.projectOpen) return SaveAction::NoProject;

    // 2. A TEXT FIELD IS ACTIVE -> COMMIT FIRST, SAVE NEXT FRAME.
    //    The old code suppressed the GLOBAL handler on io.WantTextInput but not the
    //    panel handlers, so typing a node name and pressing Ctrl+S saved the graph
    //    and not the scene - the arbitration inverted based on whether the author
    //    happened to be typing. Both alternatives lose data: suppressing the chord
    //    means a save the author believes happened, and saving immediately means
    //    every buffered-commit widget writes its PREVIOUS value. Deferring one
    //    frame (the caller clears the active id, the widget's own
    //    IsItemDeactivatedAfterEdit path commits, then this re-runs) is the only
    //    variant that cannot drop an in-flight rename.
    if (ctx.textFieldActive && !ctx.afterCommitFrame) return SaveAction::Defer;

    // 3. Resolve the focused surface.
    switch (ctx.focused) {
        // NOBODY CLAIMED. Nothing focused, the dockspace void is focused, or a panel
        // that owns no savable content (Hierarchy, Inspector, Stats, Tags, the
        // auto-saving settings panels...). The scene is the single fallback - which
        // is also today's behaviour, so a panel that forgets to claim degrades to a
        // missing feature and never to a wrong write.
        //
        // Deliberately NOT ImGuiInputFlags_RouteUnlessBgFocused on the caller's side:
        // that would make Ctrl+S do NOTHING when the void is focused, i.e. a silent
        // no-op on the most-pressed key in the editor.
        case SaveSurface::None:
        case SaveSurface::Scene:
            // The Play world is a copy that gameplay may destroy entities in and
            // strip MeshInstance/RigidBody from. It is not the authored level and
            // must never be written over it.
            return ctx.playMode ? SaveAction::RefusedPlayMode : SaveAction::Scene;

        // A focused Asset Viewer with no live sub-editor. Never falls through to the
        // scene: the author is looking at an asset, not at the level.
        case SaveSurface::AssetViewer:
            return SaveAction::NothingOpen;

        case SaveSurface::UIDocument:
            // Content check FIRST: "the UI editor has no document open" is a more
            // useful answer than "you are playing".
            if (!ctx.surfaceHasContent) return SaveAction::NothingOpen;
            // ui::UpdateInteraction writes value/toggled/selected/scrollPos on the
            // live components during Play, and all four are AUTHORED, serialized
            // initial state. SaveUIDocument already leaves the editor's interact
            // preview before capturing for exactly this reason; Play is the same
            // hazard through a different door.
            return ctx.playMode ? SaveAction::RefusedPlayMode : SaveAction::UIDocument;

        // The file-backed asset editors. Play does not mutate any of them, and
        // keeping the chord live through a playtest is genuinely useful.
        case SaveSurface::Schematic:
            return ctx.surfaceHasContent ? SaveAction::Schematic : SaveAction::NothingOpen;
        case SaveSurface::Dialogue:
            return ctx.surfaceHasContent ? SaveAction::Dialogue : SaveAction::NothingOpen;
        case SaveSurface::Cutscene:
            return ctx.surfaceHasContent ? SaveAction::Cutscene : SaveAction::NothingOpen;
        case SaveSurface::Music:
            return ctx.surfaceHasContent ? SaveAction::Music : SaveAction::NothingOpen;
        case SaveSurface::Character:
            return ctx.surfaceHasContent ? SaveAction::Character : SaveAction::NothingOpen;
        case SaveSurface::Material:
            return ctx.surfaceHasContent ? SaveAction::Material : SaveAction::NothingOpen;
        case SaveSurface::AudioEvent:
            return ctx.surfaceHasContent ? SaveAction::AudioEvent : SaveAction::NothingOpen;
        case SaveSurface::MeshSlots:
            return ctx.surfaceHasContent ? SaveAction::MeshSlots : SaveAction::NothingOpen;

        case SaveSurface::Count:
            break;
    }
    // Unreachable for a valid enumerator. Refusing to write is the safe direction.
    return SaveAction::NothingOpen;
}

const char* SaveSurfaceName(SaveSurface s) {
    switch (s) {
        case SaveSurface::None:        return "(nothing focused)";
        case SaveSurface::Scene:       return "Scene";
        case SaveSurface::UIDocument:  return "UI Editor";
        case SaveSurface::Schematic:   return "Schematic Editor";
        case SaveSurface::Dialogue:    return "Dialogue Editor";
        case SaveSurface::Cutscene:    return "Cutscene Timeline";
        case SaveSurface::Music:       return "Music";
        case SaveSurface::Character:   return "Character Editor";
        case SaveSurface::Material:    return "Asset Viewer (material)";
        case SaveSurface::AudioEvent:  return "Asset Viewer (audio event)";
        case SaveSurface::MeshSlots:   return "Asset Viewer (mesh slots)";
        case SaveSurface::AssetViewer: return "Asset Viewer";
        case SaveSurface::Count:       break;
    }
    return "?";
}

const char* SaveActionName(SaveAction a) {
    switch (a) {
        case SaveAction::NoProject:       return "NoProject";
        case SaveAction::Defer:           return "Defer";
        case SaveAction::NothingOpen:     return "NothingOpen";
        case SaveAction::RefusedPlayMode: return "RefusedPlayMode";
        case SaveAction::Scene:           return "Scene";
        case SaveAction::UIDocument:      return "UIDocument";
        case SaveAction::Schematic:       return "Schematic";
        case SaveAction::Dialogue:        return "Dialogue";
        case SaveAction::Cutscene:        return "Cutscene";
        case SaveAction::Music:           return "Music";
        case SaveAction::Character:       return "Character";
        case SaveAction::Material:        return "Material";
        case SaveAction::AudioEvent:      return "AudioEvent";
        case SaveAction::MeshSlots:       return "MeshSlots";
        case SaveAction::Count:           break;
    }
    return "?";
}

const char* SaveActionNoun(SaveAction a) {
    switch (a) {
        case SaveAction::Scene:      return "scene";
        case SaveAction::UIDocument: return "UI document";
        case SaveAction::Schematic:  return "schematic";
        case SaveAction::Dialogue:   return "dialogue graph";
        case SaveAction::Cutscene:   return "cutscene";
        case SaveAction::Music:      return "music graph";
        case SaveAction::Character:  return "character";
        case SaveAction::Material:   return "material";
        case SaveAction::AudioEvent: return "audio event";
        case SaveAction::MeshSlots:  return "mesh material slots";
        default:                     return "";
    }
}

// --- `--test-savedispatch` ---------------------------------------------------

namespace {

int g_fails = 0;

void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_fails;
    std::printf("savedispatch FAIL: %s\n", what);
}

// Every surface an author can focus, paired with the action it must produce when it
// has content, in edit mode, with a project open and no text field active. This
// table IS the "each surface maps to its own target" claim.
struct Row {
    SaveSurface surface;
    SaveAction expected;
};

constexpr Row kRows[] = {
    {SaveSurface::None,        SaveAction::Scene},
    {SaveSurface::Scene,       SaveAction::Scene},
    {SaveSurface::UIDocument,  SaveAction::UIDocument},
    {SaveSurface::Schematic,   SaveAction::Schematic},
    {SaveSurface::Dialogue,    SaveAction::Dialogue},
    {SaveSurface::Cutscene,    SaveAction::Cutscene},
    {SaveSurface::Music,       SaveAction::Music},
    {SaveSurface::Character,   SaveAction::Character},
    {SaveSurface::Material,    SaveAction::Material},
    {SaveSurface::AudioEvent,  SaveAction::AudioEvent},
    {SaveSurface::MeshSlots,   SaveAction::MeshSlots},
    // The Asset Viewer with no live sub-editor writes nothing at all.
    {SaveSurface::AssetViewer, SaveAction::NothingOpen},
};

SaveContext Ctx(SaveSurface s, bool content = true) {
    SaveContext c;
    c.focused = s;
    c.projectOpen = true;
    c.surfaceHasContent = content;
    return c;
}

} // namespace

bool SaveDispatchSelfTest() {
    g_fails = 0;

    // 1. EACH SURFACE MAPS TO ITS OWN TARGET, and does so deterministically.
    for (const Row& r : kRows) {
        const SaveAction got = DecideSave(Ctx(r.surface));
        if (got != r.expected) {
            std::printf("savedispatch FAIL: %s -> %s (expected %s)\n",
                        SaveSurfaceName(r.surface), SaveActionName(got),
                        SaveActionName(r.expected));
            ++g_fails;
        }
        Check(DecideSave(Ctx(r.surface)) == got, "DecideSave is not deterministic");
    }
    // The table must cover the whole enum, or a new surface could ship untested.
    Check(static_cast<usize>(SaveSurface::Count) == sizeof(kRows) / sizeof(kRows[0]),
          "kRows does not cover every SaveSurface enumerator");

    // 2. EXACTLY ONE TARGET PER KEYPRESS. DecideSave returns a single enumerator, so
    //    the structural claim to prove is the one the old code violated: NO surface
    //    other than Scene/None may ever produce a scene write. That is what made
    //    Ctrl+S in the schematic editor save the graph AND the level.
    for (const Row& r : kRows) {
        if (r.surface == SaveSurface::Scene || r.surface == SaveSurface::None) continue;
        for (int content = 0; content < 2; ++content) {
            for (int play = 0; play < 2; ++play) {
                for (int commit = 0; commit < 2; ++commit) {
                    SaveContext c = Ctx(r.surface, content != 0);
                    c.playMode = play != 0;
                    c.afterCommitFrame = commit != 0;
                    Check(DecideSave(c) != SaveAction::Scene,
                          "a non-scene surface produced a SCENE write (the double-fire bug)");
                }
            }
        }
    }
    // ...and every write-action is produced by exactly ONE surface, so no keypress
    // is ambiguous about which file it lands in.
    for (const Row& a : kRows) {
        for (const Row& b : kRows) {
            if (&a == &b) continue;
            if (a.expected == SaveAction::Scene && b.expected == SaveAction::Scene) continue;
            if (a.expected == SaveAction::NothingOpen) continue;
            Check(a.expected != b.expected, "two surfaces share one write target");
        }
    }

    // 3. FALLBACK: nothing focused -> the scene, and ONLY that case falls back.
    Check(DecideSave(Ctx(SaveSurface::None, false)) == SaveAction::Scene,
          "nothing focused must fall back to the scene");
    Check(DecideSave(Ctx(SaveSurface::Scene, false)) == SaveAction::Scene,
          "the scene surface saves the scene even with no scene file yet (Save As)");

    // 4. A FOCUSED SURFACE WITH NOTHING OPEN WRITES NOTHING - it does NOT fall
    //    through to the scene. Falling through is the exact bug class being fixed:
    //    the author believes they saved a graph.
    for (const Row& r : kRows) {
        if (r.surface == SaveSurface::Scene || r.surface == SaveSurface::None) continue;
        Check(DecideSave(Ctx(r.surface, /*content*/ false)) == SaveAction::NothingOpen,
              "an empty surface must report NothingOpen, never fall back to the scene");
    }

    // 5. A TEXT FIELD DEFERS the chord (it is never dropped), and the deferred
    //    re-dispatch on the next frame produces the real target.
    for (const Row& r : kRows) {
        SaveContext c = Ctx(r.surface);
        c.textFieldActive = true;
        Check(DecideSave(c) == SaveAction::Defer,
              "a focused text field must defer the save, not drop or rush it");
        c.afterCommitFrame = true;
        Check(DecideSave(c) == r.expected,
              "the post-commit re-dispatch must reach the same target");
        // Deferral outranks everything except the no-project guard.
        SaveContext p = c;
        p.afterCommitFrame = false;
        p.playMode = true;
        Check(DecideSave(p) == SaveAction::Defer, "defer must precede the play-mode check");
    }

    // 6. NO PROJECT beats every other consideration, including the text field.
    for (const Row& r : kRows) {
        SaveContext c = Ctx(r.surface);
        c.projectOpen = false;
        Check(DecideSave(c) == SaveAction::NoProject, "no project must refuse every surface");
        c.textFieldActive = true;
        Check(DecideSave(c) == SaveAction::NoProject, "no project must outrank the text field");
    }

    // 7. PLAY MODE refuses the two surfaces Play mutates (the scene and `.hbui`
    //    documents) and leaves every file-backed asset editor working.
    {
        SaveContext c = Ctx(SaveSurface::Scene);
        c.playMode = true;
        Check(DecideSave(c) == SaveAction::RefusedPlayMode,
              "Play must refuse a scene save");
        c.focused = SaveSurface::None;
        Check(DecideSave(c) == SaveAction::RefusedPlayMode,
              "Play must refuse the scene fallback too");
        c.focused = SaveSurface::UIDocument;
        Check(DecideSave(c) == SaveAction::RefusedPlayMode,
              "Play must refuse a UI document save");
        // ...but NothingOpen is the more useful answer when there is no document.
        c.surfaceHasContent = false;
        Check(DecideSave(c) == SaveAction::NothingOpen,
              "an empty surface reports NothingOpen even in Play");
    }
    for (const Row& r : kRows) {
        if (r.surface == SaveSurface::Scene || r.surface == SaveSurface::None ||
            r.surface == SaveSurface::UIDocument)
            continue;
        SaveContext c = Ctx(r.surface);
        c.playMode = true;
        Check(DecideSave(c) == r.expected,
              "Play must not disturb the file-backed asset editors");
    }

    // 8. Names are total and distinct - status lines and logs read from them.
    for (usize i = 0; i < static_cast<usize>(SaveSurface::Count); ++i) {
        const char* n = SaveSurfaceName(static_cast<SaveSurface>(i));
        Check(n && n[0] && std::strcmp(n, "?") != 0, "a SaveSurface has no name");
    }
    for (usize i = 0; i < static_cast<usize>(SaveAction::Count); ++i) {
        const auto a = static_cast<SaveAction>(i);
        const char* n = SaveActionName(a);
        Check(n && n[0] && std::strcmp(n, "?") != 0, "a SaveAction has no name");
        for (usize j = 0; j < i; ++j)
            Check(std::strcmp(n, SaveActionName(static_cast<SaveAction>(j))) != 0,
                  "two SaveActions share a name");
        // Every WRITE action names the file kind it writes; the non-writing ones
        // deliberately do not (their status line names the surface instead).
        const bool writes = i >= static_cast<usize>(SaveAction::Scene);
        Check(writes == (SaveActionNoun(a)[0] != '\0'),
              "SaveActionNoun disagrees with the write/non-write split");
    }

    if (g_fails == 0) {
        std::printf("savedispatch: %d surfaces, one target each; no surface but the "
                    "scene can write the scene; empty surfaces write nothing; text "
                    "fields defer; Play refuses scene + .hbui\n",
                    static_cast<int>(sizeof(kRows) / sizeof(kRows[0])));
    }
    return g_fails == 0;
}

} // namespace hbe::editor
