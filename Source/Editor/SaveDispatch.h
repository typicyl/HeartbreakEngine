// Editor/SaveDispatch.h - WHAT DOES Ctrl+S SAVE?
//
// The rule, in one sentence:
//
//   Ctrl+S saves the surface that owns the FOCUSED window; every panel that owns a
//   savable surface claims the chord unconditionally; the scene is the single global
//   fallback that only fires when no panel claimed.
//
// This header holds the DECISION half of that rule and nothing else. It is pure: no
// ImGui, no Engine, no filesystem, no Editor state. The caller collects the facts
// (who is focused, is anything open there, is a text field active, are we playing)
// into a SaveContext, DecideSave() answers with exactly ONE SaveAction, and the
// caller executes it. That split is what makes the dispatch testable headlessly -
// see SaveDispatchSelfTest() / `--test-savedispatch`.
//
// WHY IT MATTERS THAT EXACTLY ONE ACTION COMES BACK. The bug this replaces was a
// global polled Ctrl+S with no focus gate running *alongside* two panel-local
// focus-gated handlers, so one keypress in the schematic editor saved the graph AND
// the scene, and one keypress in the UI editor saved the scene INSTEAD of the open
// document. A function that returns a single enumerator cannot express either
// failure; the imgui input-routing table on the other side guarantees only one
// claimant reaches it.
#pragma once

#include "Core/Types.h"

namespace hbe::editor {

// The editing surfaces that can own a Ctrl+S. One enumerator per THING THAT HAS A
// FILE, not per panel: several panels map onto Scene, and the Asset Viewer resolves
// to whichever of its three sub-editors is live.
enum class SaveSurface : u8 {
    None = 0,    // nothing focused, or a focused panel that owns no savable content
    Scene,       // Viewport / Game / Hierarchy / Inspector / Timeline / ... + Art Editor
    UIDocument,  // UI Editor + UI Document panel -> the active `.hbui`
    Schematic,   // Schematic Editor -> the open `.hbschem`
    Dialogue,    // Dialogue Editor -> the open `.hbdialogue`
    Cutscene,    // Cutscene Timeline -> the open `.hbcutscene`
    Music,       // Music panel -> the open `.hbmusic` (+ the .hbproj keys it owns)
    Character,   // Character Editor -> the open `.hbchar`
    Material,    // Asset Viewer -> `.hbmat` sub-editor
    AudioEvent,  // Asset Viewer -> `.hbevent` sub-editor
    MeshSlots,   // Asset Viewer -> `.uaf` mesh material-slot sub-editor
    // The Asset Viewer is focused but NONE of its three sub-editors is live (a
    // texture, a scene, a read-only preview). Deliberately NOT None: None means
    // "fall through to the scene", and a focused Asset Viewer must never write the
    // level. Always resolves to NothingOpen.
    AssetViewer,
    // The collaboration panels (Collaborate / People / Review changes). Same reasoning
    // as AssetViewer and, if anything, sharper: these panels are mostly TEXT BOXES that
    // an invitation gets pasted into. Left as None, a Ctrl+V aimed at the paste field
    // would fall through and paste the scene CLIPBOARD - entities - into the level, and
    // Ctrl+X would cut the current selection out of it. They own no savable document, so
    // every chord here resolves to "swallowed", never to the scene.
    Collaborate,
    Count
};

// What the caller must do. Exactly one per keypress.
enum class SaveAction : u8 {
    // --- non-writing outcomes; each one still produces a visible status line ---
    NoProject,        // no project is open: nothing can be written
    Defer,            // a text field is active: commit it, re-dispatch next frame
    NothingOpen,      // the focused surface owns nothing right now. WRITE NOTHING.
    RefusedPlayMode,  // scene / `.hbui` save attempted during Play. WRITE NOTHING.
    // --- writes ---
    Scene,
    UIDocument,
    Schematic,
    Dialogue,
    Cutscene,
    Music,
    Character,
    Material,
    AudioEvent,
    MeshSlots,
    Count
};

// The facts the decision is made from. Everything here is observable without
// touching a file.
struct SaveContext {
    // Who won the chord. SaveSurface::None = nobody claimed it (nothing focused, the
    // void is focused, or the focused panel owns no savable surface).
    SaveSurface focused = SaveSurface::None;
    // Is a project open. Nothing is savable without one - mirrors SaveAll's guard.
    bool projectOpen = false;
    // Does `focused` actually have something open (a graph, a document, an asset).
    // Ignored for Scene / None: the world always exists, and a never-saved scene is
    // handled by the executor's Save-As path, not by refusing here.
    bool surfaceHasContent = false;
    // Editor Play mode. The Play world is a snapshot-restored COPY that gameplay is
    // allowed to destroy entities in and strip components from, and `.hbui` widget
    // state (value/toggled/selected/scrollPos) is authored data that Play mutates -
    // so neither may be written back over the authored file.
    bool playMode = false;
    // Any ImGui text field is active. Ctrl+S is honoured, but only AFTER the field
    // has committed: saving on the same frame would write the widget's previous
    // value for every buffered-commit field in the tree.
    bool textFieldActive = false;
    // This dispatch is the re-run on the frame after a Defer, once the field has
    // been deactivated and its commit path has run.
    bool afterCommitFrame = false;
};

// THE dispatch rule. Total: every SaveSurface yields exactly one SaveAction.
SaveAction DecideSave(const SaveContext& ctx);

// Stable names for status lines, logs and the self-test.
const char* SaveSurfaceName(SaveSurface s);
const char* SaveActionName(SaveAction a);
// The file kind a write-action targets ("scene", "UI document", ...), for the
// author-facing "Saved <what> '<file>'" line. Empty for the non-writing actions.
const char* SaveActionNoun(SaveAction a);

// `--test-savedispatch`: proves the rule above without an ImGui context - each
// surface maps to its own target, exactly one target per keypress, no surface but
// Scene/None can ever produce a scene write, the fallback rules hold, a text field
// defers rather than being dropped, and Play refuses the two mutated surfaces.
bool SaveDispatchSelfTest();

// ============================================================================
// THE EDIT CHORDS - Ctrl+Z / Y / X / C / V / D
// ============================================================================
//
// Same rule, same shape, same reason. Ctrl+S was routed to the focused surface
// years before these were, and they stayed a single global poll gated only on
// `io.KeyCtrl && !io.WantTextInput`. That meant Ctrl+X with the Dialogue Editor
// focused ran CopySelection + PushUndo + DestroyRecursive on the SCENE selection -
// a destructive edit to the level, from a keypress aimed at a dialogue node.
//
// Three further defects fall out of the same fix:
//   * `io.KeyCtrl` is a modifier-DOWN test, not an exact chord match, so Ctrl+Alt+D
//     duplicated and Ctrl+Shift+Y redid. ImGui::Shortcut matches modifiers exactly -
//     the same property that already makes Ctrl+Shift+S its own registration.
//   * `!io.WantTextInput` was a blunt global gate: while ANY text field anywhere was
//     active, Ctrl+Z in the viewport did nothing. Under routing an active InputText
//     registers Ctrl+Z/Y/X/C/V against its own item id and scores 300 (vs a focused
//     window's 199), so it wins and undoes THE FIELD - which is what the author means.
//   * The paint-vs-scene arbitration was global: with the Art Editor painting, Ctrl+Z
//     popped a paint stroke even when the Dialogue Editor was focused.

// One enumerator per CHORD, not per key: Ctrl+Z and Ctrl+Shift+Z are two chords.
enum class EditVerb : u8 { Undo = 0, Redo, Cut, Copy, Paste, Duplicate, Count };

// What the caller must do. Exactly one per chord per frame. The PREFIX names the
// history/clipboard the verb lands in - the edit analogue of "which file does
// Ctrl+S write".
enum class EditAction : u8 {
    // --- non-acting outcomes ---
    // The focused surface owns the chord and has nothing to do with it. SWALLOWED:
    // no status line, and above all NOT forwarded to the scene. This enumerator is
    // the fix for the destructive bug above.
    Ignored = 0,
    NothingOpen,      // a focused asset editor with no asset open (status line)
    RefusedPlayMode,  // reserved: a structural scene edit during Play (see below)
    // --- the SCENE history (undoStack_/redoStack_) + the SCENE clipboard ---
    // Viewport / Hierarchy / Inspector / Timeline, AND the UI Editor: `.hbui`
    // elements ARE entities, PushUndo(Engine&) already captures every open document,
    // and CopySelection already carries clipboardFromDoc_. Giving the UI editor a
    // second history would be exactly the reinvention this dispatch exists to avoid.
    SceneUndo, SceneRedo, SceneCut, SceneCopy, ScenePaste, SceneDuplicate,
    // --- the surface-PAINT stroke history (paintStrokeOrder_/paintStrokeRedo_) ---
    // Only Undo/Redo exist: strokes have no clipboard.
    PaintUndo, PaintRedo,
    // --- the FOCUSED asset editor's OWN working copy ---
    // WHICH asset is ctx.focused; the executor switches on it the same way
    // ProcessSaveRequest's per-surface cases already do.
    AssetUndo, AssetRedo, AssetCut, AssetCopy, AssetPaste, AssetDuplicate,
    Count
};

// The facts, all observable without touching a file. Mirrors SaveContext.
struct EditContext {
    SaveSurface focused = SaveSurface::None; // REUSED verbatim - same claim ids
    EditVerb verb = EditVerb::Undo;
    bool playMode = false;
    // ImGui routes Ctrl+Z/Y/X/C/V to an ACTIVE InputText at score 300 (beating a
    // panel's 199), so those never reach here while typing. Ctrl+D is the exception -
    // ImGui registers no route for it - so this gate survives, but NARROWED to the
    // destructive verbs instead of blanking the whole block.
    bool textFieldActive = false;
    bool surfaceHasContent = false;
    // paintActive_ && !paintStrokeMode_ && (strokes || redo). Only ever diverts
    // Undo/Redo, and only inside the Scene domain - it used to divert them from ANY
    // focused panel.
    bool paintHistoryActive = false;
    bool hasSelection = false;   // scene: selected_ valid; asset: a node/key selected
    bool clipboardEmpty = true;  // the clipboard the RESOLVED domain pastes from
    bool historyEmpty = true;    // that domain's undo (Undo) or redo (Redo) stack
};

// THE dispatch rule. Total: every (SaveSurface, EditVerb) pair yields exactly one
// EditAction.
EditAction DecideEdit(const EditContext& ctx);

// Does `s` implement `v` AT ALL. The capability table in the .cpp is the whole
// policy: granting a verb to a panel is one bit there plus one executor case. A verb
// a surface does not implement resolves to Ignored - NEVER to the scene.
bool SurfaceHandlesVerb(SaveSurface s, EditVerb v);

const char* EditVerbName(EditVerb v);
const char* EditActionName(EditAction a);

// Folded into `--test-savedispatch`. Same contract: pure, headless, no ImGui.
// Sweeps the full cross product of surfaces x verbs x every boolean combination.
bool EditDispatchSelfTest();

} // namespace hbe::editor
