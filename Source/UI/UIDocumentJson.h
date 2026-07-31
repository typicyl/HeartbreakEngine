// UI/UIDocumentJson.h - INTERNAL header: the per-component UI JSON sub-object
// writers/readers, in ONE place.
//
// WHY THIS FILE IS SEPARATE FROM UIDocument.h
// -------------------------------------------
// `UI/UIDocument.h` is a PUBLIC header: it is included by `Scene/Components.h`
// consumers and (later) by the Engine and the Editor. It must not drag
// <nlohmann/json.hpp> into every translation unit that only wants `ui::DocData`,
// and blocker B10 of docs/Design-TagStreaming.md forbids putting a json type in
// its signatures. So the json half lives HERE, and only the two .cpp that
// actually serialize include it:
//
//     UI/UIDocument.cpp        - reads/writes .hbui documents
//     Scene/SceneSerializer.cpp - reads/writes the SAME sub-objects in .hbscene
//
// ONE IMPLEMENTATION, TWO CALLERS, ZERO DRIFT. Before this file existed the
// fourteen blocks below were inlined in SceneSerializer.cpp's EntityToJson /
// ParseSceneJson; a `.hbui` writer that re-typed them would have been a 53-way
// opportunity to silently drop a UIElement field, and a dropped UI field is
// invisible until somebody opens the settings menu on a shipped build.
//
// The bodies are a VERBATIM lift. Key spelling, key ORDER, default values, the
// clamps, and all three UIElement back-compat rules (the v2 collapsed "anchor",
// the v1 "textScale" x 28, and every glm::clamp/glm::max) are preserved exactly,
// because `.hbscene` files already on disk are written by these functions and
// SaveScene output must stay byte-identical. `--test-uidoc` pins that: it
// carries a frozen copy of the pre-extraction blocks and diffs them against
// these, bit for bit, over real authored content plus a fuzz.
//
// NOTE ON WorldText. It is in this set purely by adjacency - it is WORLD-space
// 3D text placed by a Transform and drawn through the particle pass, not screen
// UI. It has no place in a `.hbui` document (`ui::DocEntity` has no field for
// it) and the migrator does NOT treat it as a UI key; it lives here only so that
// all seven blocks the scene serializer shares with the UI layer have one home.
#pragma once

#include "Scene/Components.h"

#include <nlohmann/json.hpp>

namespace hbe::ui {

// --- Writers: component -> sub-object -----------------------------------------
// Each returns exactly the object that used to be assigned to je["<key>"].
nlohmann::json WriteElement(const UIElement& el);
nlohmann::json WriteCanvas(const UICanvas& canvas);
nlohmann::json WriteAnimator(const UIAnimator& an);
nlohmann::json WritePanel(const UIPanel& p);
nlohmann::json WriteLayout(const UILayoutGroup& lg);
nlohmann::json WriteGroup(const UICanvasGroup& cg);
nlohmann::json WriteWorldText(const WorldText& wt); // scene-only; see the note above

// --- Readers: sub-object -> component -----------------------------------------
// `out` is read as well as written: fields whose JSON key is absent keep the
// value `out` came in with (the scene path passes a default-constructed
// component, which is where the documented defaults come from). Callers set
// their own `has*` flag - these never do.
void ReadElement(const nlohmann::json& j, UIElement& out);
void ReadCanvas(const nlohmann::json& j, UICanvas& out);
void ReadAnimator(const nlohmann::json& j, UIAnimator& out);
void ReadPanel(const nlohmann::json& j, UIPanel& out);
void ReadLayout(const nlohmann::json& j, UILayoutGroup& out);
void ReadGroup(const nlohmann::json& j, UICanvasGroup& out);
void ReadWorldText(const nlohmann::json& j, WorldText& out); // scene-only

} // namespace hbe::ui
