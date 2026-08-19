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
        // The collaboration panels own no file. Ctrl+S with one focused must not write
        // the level - the author is looking at a connection, not at the world.
        case SaveSurface::Collaborate:
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
        case SaveSurface::Sequence:
            return ctx.surfaceHasContent ? SaveAction::Sequence : SaveAction::NothingOpen;
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
        case SaveSurface::Sequence:    return "Sequencer";
        case SaveSurface::Music:       return "Music";
        case SaveSurface::Character:   return "Character Editor";
        case SaveSurface::Material:    return "Asset Viewer (material)";
        case SaveSurface::AudioEvent:  return "Asset Viewer (audio event)";
        case SaveSurface::MeshSlots:   return "Asset Viewer (mesh slots)";
        case SaveSurface::AssetViewer: return "Asset Viewer";
        case SaveSurface::Collaborate: return "Collaborate";
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
        case SaveAction::Sequence:        return "Sequence";
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
        case SaveAction::Sequence:   return "sequence";
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
    {SaveSurface::Sequence,    SaveAction::Sequence},
    {SaveSurface::Music,       SaveAction::Music},
    {SaveSurface::Character,   SaveAction::Character},
    {SaveSurface::Material,    SaveAction::Material},
    {SaveSurface::AudioEvent,  SaveAction::AudioEvent},
    {SaveSurface::MeshSlots,   SaveAction::MeshSlots},
    // The Asset Viewer with no live sub-editor writes nothing at all.
    {SaveSurface::AssetViewer, SaveAction::NothingOpen},
    // The collaboration panels own no file either.
    {SaveSurface::Collaborate, SaveAction::NothingOpen},
};
// kRows is HAND-MAINTAINED, and a surface simply missing from it is not a failure - the
// sweeps below just never exercise it, and the suite still says PASS. That is how a new
// surface gets added with no test at all, so make the omission a build error instead.
static_assert(sizeof(kRows) / sizeof(kRows[0]) == static_cast<usize>(SaveSurface::Count),
              "kRows must cover every SaveSurface - a missing row is a silently untested "
              "surface, not a smaller test");

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

// ============================================================================
// The EDIT chord decision (Ctrl+Z / Y / X / C / V / D)
// ============================================================================

namespace {
constexpr u8 kU = 1u << static_cast<u8>(EditVerb::Undo);
constexpr u8 kR = 1u << static_cast<u8>(EditVerb::Redo);
constexpr u8 kX = 1u << static_cast<u8>(EditVerb::Cut);
constexpr u8 kC = 1u << static_cast<u8>(EditVerb::Copy);
constexpr u8 kV = 1u << static_cast<u8>(EditVerb::Paste);
constexpr u8 kD = 1u << static_cast<u8>(EditVerb::Duplicate);

// ONE ENTRY PER SaveSurface, IN ENUM ORDER. This table is the entire policy: a 0
// means "swallow every edit chord in this panel", which is always safe because a
// swallowed chord does nothing, whereas a forwarded one edits the LEVEL.
//
// The asset editors currently grant only Undo/Redo. Cut/Copy/Paste/Duplicate stay
// off until each editor has a node/key clipboard of its own - granting them earlier
// would mean the executor either no-ops (a lie) or falls back to the scene (the very
// bug this replaces). See the phase note on AssetHistory in Editor.h.
constexpr u8 kCaps[static_cast<usize>(SaveSurface::Count)] = {
    /* None        */ kU | kR | kX | kC | kV | kD, // the scene fallback: today's behaviour
    /* Scene       */ kU | kR | kX | kC | kV | kD,
    /* UIDocument  */ kU | kR | kX | kC | kV | kD, // entities: the SCENE history/clipboard
    /* Schematic   */ kU | kR,
    /* Dialogue    */ kU | kR,
    /* Cutscene    */ kU | kR,
    /* Sequence    */ kU | kR,
    /* Music       */ kU | kR,
    /* Character   */ kU | kR,
    /* Material    */ kU | kR,
    /* AudioEvent  */ kU | kR,
    /* MeshSlots   */ kU | kR,
    /* AssetViewer */ 0, // a texture / read-only preview: swallow everything
    // 0 IS THE WHOLE POINT HERE. These panels are text boxes an invitation gets pasted
    // into; ImGui routes Ctrl+Z/Y/X/C/V to the ACTIVE InputText at score 300, so typing
    // behaves normally, and every chord that reaches this table instead is swallowed
    // rather than cutting the level's selection.
    /* Collaborate */ 0,
};
static_assert(sizeof(kCaps) / sizeof(kCaps[0]) == static_cast<usize>(SaveSurface::Count),
              "kCaps must have one entry per SaveSurface, in the same order");

// Verbs that require something selected, and verbs that pop a history stack.
constexpr u8 kNeedsSelection = kX | kC | kD;
constexpr u8 kNeedsHistory = kU | kR;

bool IsSceneDomain(SaveSurface s) {
    return s == SaveSurface::None || s == SaveSurface::Scene || s == SaveSurface::UIDocument;
}

constexpr EditAction kSceneAction[static_cast<usize>(EditVerb::Count)] = {
    EditAction::SceneUndo,  EditAction::SceneRedo,  EditAction::SceneCut,
    EditAction::SceneCopy,  EditAction::ScenePaste, EditAction::SceneDuplicate,
};
constexpr EditAction kAssetAction[static_cast<usize>(EditVerb::Count)] = {
    EditAction::AssetUndo,  EditAction::AssetRedo,  EditAction::AssetCut,
    EditAction::AssetCopy,  EditAction::AssetPaste, EditAction::AssetDuplicate,
};
} // namespace

bool SurfaceHandlesVerb(SaveSurface s, EditVerb v) {
    if (s >= SaveSurface::Count || v >= EditVerb::Count) return false;
    return (kCaps[static_cast<usize>(s)] & (1u << static_cast<u8>(v))) != 0;
}

EditAction DecideEdit(const EditContext& ctx) {
    const u8 verbBit = 1u << static_cast<u8>(ctx.verb);

    // 1. A TEXT FIELD KEEPS THE DESTRUCTIVE VERBS. Undo/Redo/Copy are deliberately
    //    NOT suppressed: ImGui's active InputText already owns those chords at a
    //    higher route score, so they never arrive here while typing. Ctrl+D has no
    //    ImGui route, hence this narrow gate rather than the old global blanking.
    if (ctx.textFieldActive &&
        (ctx.verb == EditVerb::Cut || ctx.verb == EditVerb::Paste ||
         ctx.verb == EditVerb::Duplicate))
        return EditAction::Ignored;

    // 2. WHICH HISTORY. Resolved from the surface alone.
    if (!IsSceneDomain(ctx.focused)) {
        // A FOCUSED ASSET EDITOR NEVER TOUCHES THE SCENE. This branch IS the bug:
        // Ctrl+X in the Dialogue Editor used to run DestroyRecursive on the level.
        if (!SurfaceHandlesVerb(ctx.focused, ctx.verb)) return EditAction::Ignored;
        if (!ctx.surfaceHasContent) return EditAction::NothingOpen;
        if ((verbBit & kNeedsSelection) && !ctx.hasSelection) return EditAction::Ignored;
        if (ctx.verb == EditVerb::Paste && ctx.clipboardEmpty) return EditAction::Ignored;
        if ((verbBit & kNeedsHistory) && ctx.historyEmpty) return EditAction::Ignored;
        return kAssetAction[static_cast<usize>(ctx.verb)];
    }

    // A scene-domain surface that does not implement the verb still swallows it.
    if (!SurfaceHandlesVerb(ctx.focused, ctx.verb)) return EditAction::Ignored;

    // 3. PAINT FIRST, SCENE AS ITS ELSE - the order the old global poll had, but now
    //    only while a SCENE-domain surface is focused.
    if (ctx.paintHistoryActive &&
        (ctx.verb == EditVerb::Undo || ctx.verb == EditVerb::Redo))
        return ctx.verb == EditVerb::Undo ? EditAction::PaintUndo : EditAction::PaintRedo;

    if ((verbBit & kNeedsSelection) && !ctx.hasSelection) return EditAction::Ignored;
    if (ctx.verb == EditVerb::Paste && ctx.clipboardEmpty) return EditAction::Ignored;
    if ((verbBit & kNeedsHistory) && ctx.historyEmpty) return EditAction::Ignored;
    return kSceneAction[static_cast<usize>(ctx.verb)];
}

const char* EditVerbName(EditVerb v) {
    switch (v) {
        case EditVerb::Undo: return "undo";
        case EditVerb::Redo: return "redo";
        case EditVerb::Cut: return "cut";
        case EditVerb::Copy: return "copy";
        case EditVerb::Paste: return "paste";
        case EditVerb::Duplicate: return "duplicate";
        case EditVerb::Count: break;
    }
    return "?";
}

const char* EditActionName(EditAction a) {
    switch (a) {
        case EditAction::Ignored: return "Ignored";
        case EditAction::NothingOpen: return "NothingOpen";
        case EditAction::RefusedPlayMode: return "RefusedPlayMode";
        case EditAction::SceneUndo: return "SceneUndo";
        case EditAction::SceneRedo: return "SceneRedo";
        case EditAction::SceneCut: return "SceneCut";
        case EditAction::SceneCopy: return "SceneCopy";
        case EditAction::ScenePaste: return "ScenePaste";
        case EditAction::SceneDuplicate: return "SceneDuplicate";
        case EditAction::PaintUndo: return "PaintUndo";
        case EditAction::PaintRedo: return "PaintRedo";
        case EditAction::AssetUndo: return "AssetUndo";
        case EditAction::AssetRedo: return "AssetRedo";
        case EditAction::AssetCut: return "AssetCut";
        case EditAction::AssetCopy: return "AssetCopy";
        case EditAction::AssetPaste: return "AssetPaste";
        case EditAction::AssetDuplicate: return "AssetDuplicate";
        case EditAction::Count: break;
    }
    return "?";
}

bool EditDispatchSelfTest() {
    g_fails = 0;

    const auto isSceneWrite = [](EditAction a) {
        return a == EditAction::SceneUndo || a == EditAction::SceneRedo ||
               a == EditAction::SceneCut || a == EditAction::SceneCopy ||
               a == EditAction::ScenePaste || a == EditAction::SceneDuplicate;
    };
    const auto isAssetWrite = [](EditAction a) {
        return a == EditAction::AssetUndo || a == EditAction::AssetRedo ||
               a == EditAction::AssetCut || a == EditAction::AssetCopy ||
               a == EditAction::AssetPaste || a == EditAction::AssetDuplicate;
    };

    // THE TEST'S OWN ORACLE, deliberately INDEPENDENT of the implementation's
    // IsSceneDomain(). Sharing that helper would make the headline invariant below
    // tautological: an injected fault in IsSceneDomain would silently change what the
    // check MEANS as well as what the code DOES, and the property would keep passing
    // for the wrong reason. (Verified by fault injection - the shared-helper version
    // did exactly that.) This list is written out by hand so a change to the
    // implementation's domain rule has to disagree with it.
    const auto expectedSceneDomain = [](SaveSurface s) {
        return s == SaveSurface::None || s == SaveSurface::Scene ||
               s == SaveSurface::UIDocument;
    };

    const usize nSurf = static_cast<usize>(SaveSurface::Count);
    const usize nVerb = static_cast<usize>(EditVerb::Count);
    u32 reached[static_cast<usize>(EditAction::Count)] = {};

    // The FULL cross product: 12 surfaces x 6 verbs x 2^6 boolean combinations.
    for (usize s = 0; s < nSurf; ++s) {
        for (usize v = 0; v < nVerb; ++v) {
            for (u32 bits = 0; bits < 64; ++bits) {
                EditContext c;
                c.focused = static_cast<SaveSurface>(s);
                c.verb = static_cast<EditVerb>(v);
                c.playMode = (bits & 1) != 0;
                c.textFieldActive = (bits & 2) != 0;
                c.surfaceHasContent = (bits & 4) != 0;
                c.paintHistoryActive = (bits & 8) != 0;
                c.hasSelection = (bits & 16) != 0;
                c.clipboardEmpty = (bits & 32) != 0;
                c.historyEmpty = (bits & 32) == 0; // exercise both stacks
                const EditAction a = DecideEdit(c);

                Check(a < EditAction::Count, "DecideEdit returned an out-of-range action");
                Check(IsSceneDomain(c.focused) == expectedSceneDomain(c.focused),
                      "the implementation's scene-domain rule diverged from the test's");
                reached[static_cast<usize>(a)]++;
                // DETERMINISM: the same facts must give the same answer.
                Check(DecideEdit(c) == a, "DecideEdit is not deterministic");

                // THE HEADLINE INVARIANT - the reported bug, as a property. A focused
                // ASSET editor can never produce a scene edit, under ANY combination
                // of facts. This is what makes Ctrl+X in the Dialogue Editor provably
                // unable to run DestroyRecursive on the level.
                if (!expectedSceneDomain(c.focused))
                    Check(!isSceneWrite(a),
                          "a non-scene surface produced a SCENE edit (the destructive bug)");

                // SWALLOW, NEVER FALL THROUGH: a verb the surface does not implement
                // is Ignored - never a scene action, never an asset action.
                if (!SurfaceHandlesVerb(c.focused, c.verb))
                    Check(a == EditAction::Ignored,
                          "an unimplemented verb did not resolve to Ignored");

                // DESTRUCTIVE VERBS ARE GATED: a cut/duplicate with nothing selected,
                // or a paste from an empty clipboard, must be unrepresentable.
                if (!c.hasSelection)
                    Check(a != EditAction::SceneCut && a != EditAction::AssetCut &&
                              a != EditAction::SceneDuplicate && a != EditAction::AssetDuplicate,
                          "a cut/duplicate was allowed with no selection");
                if (c.clipboardEmpty)
                    Check(a != EditAction::ScenePaste && a != EditAction::AssetPaste,
                          "a paste was allowed from an empty clipboard");

                // PAINT ARBITRATION IS BOUNDED: it may only ever affect Undo/Redo,
                // and only inside the scene domain. It used to divert Ctrl+Z from any
                // focused panel.
                if (a == EditAction::PaintUndo || a == EditAction::PaintRedo) {
                    Check(expectedSceneDomain(c.focused),
                          "paint history captured a chord outside the scene domain");
                    Check(c.verb == EditVerb::Undo || c.verb == EditVerb::Redo,
                          "paint history captured a non-undo verb");
                    Check(c.paintHistoryActive, "a paint action fired with no paint history");
                }

                // THE TEXT-FIELD GATE IS MINIMAL: it suppresses exactly Cut/Paste/
                // Duplicate. Undo/Redo/Copy are left to ImGui's higher-scoring active
                // item, which is the whole point of routing them.
                if (c.textFieldActive &&
                    (c.verb == EditVerb::Cut || c.verb == EditVerb::Paste ||
                     c.verb == EditVerb::Duplicate))
                    Check(a == EditAction::Ignored, "a destructive verb survived a text field");
            }
        }
    }

    // UNDO AND REDO ALWAYS PAIR TO THE SAME DOMAIN - Ctrl+Z and Ctrl+Y can never pop
    // from two different stacks for one surface.
    for (usize s = 0; s < nSurf; ++s) {
        EditContext u;
        u.focused = static_cast<SaveSurface>(s);
        u.surfaceHasContent = true;
        u.hasSelection = true;
        u.clipboardEmpty = false;
        u.historyEmpty = false;
        EditContext r = u;
        u.verb = EditVerb::Undo;
        r.verb = EditVerb::Redo;
        const EditAction au = DecideEdit(u), ar = DecideEdit(r);
        Check(isSceneWrite(au) == isSceneWrite(ar) && isAssetWrite(au) == isAssetWrite(ar),
              "undo and redo resolved to different histories for one surface");
    }

    // NO DEAD ENUMERATOR except the two deliberately reserved ones.
    for (usize i = 0; i < static_cast<usize>(EditAction::Count); ++i) {
        const EditAction a = static_cast<EditAction>(i);
        if (a == EditAction::RefusedPlayMode) continue; // reserved; see Editor.h note
        if (a == EditAction::AssetCut || a == EditAction::AssetCopy ||
            a == EditAction::AssetPaste || a == EditAction::AssetDuplicate)
            continue; // granted once each asset editor has its own clipboard
        Check(reached[i] > 0, "an EditAction is unreachable from every surface");
        const char* n = EditActionName(a);
        Check(n[0] != '?', "an EditAction has no name");
        for (usize j = 0; j < i; ++j)
            Check(std::strcmp(n, EditActionName(static_cast<EditAction>(j))) != 0,
                  "two EditActions share a name");
    }
    for (usize v = 0; v < nVerb; ++v)
        Check(EditVerbName(static_cast<EditVerb>(v))[0] != '?', "an EditVerb has no name");

    if (g_fails == 0) {
        std::printf("editdispatch: %zu surfaces x %zu verbs x 64 fact combinations; no "
                    "asset editor can ever edit the scene; unimplemented verbs are "
                    "swallowed; cut/paste/duplicate are gated\n",
                    nSurf, nVerb);
    }
    return g_fails == 0;
}

} // namespace hbe::editor
