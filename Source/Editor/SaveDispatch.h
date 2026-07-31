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

} // namespace hbe::editor
