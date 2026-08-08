// Construction/ConstructionPreset.h - presets as preconfigured procedural graphs.
//
// WHAT A PRESET IS, precisely. It is a FUNCTION FROM PARAMETERS TO A ConstructionDef - not a
// finished asset and not a hardcoded mesh. The ConstructionDef it produces is a graph of
// components and support edges that the artist can then inspect, retune, extend, cut and override.
// So the workflow the brief asks for in SS2:
//
//     Preset -> procedural graph -> parameters -> generated geometry
//
// maps onto:
//
//     PresetDesc::build -> ConstructionDef -> PresetParams -> BuildSection
//
// WHY THIS ANSWERS SS23 (a future graph editor) WITHOUT PRETENDING TO BE ONE. The preset's build
// function is ordinary C++, which is not itself a node graph. But what it EMITS is declarative:
// a component list with stable ids, containment, typed parameter blocks and an explicit support
// edge list. That IS the graph. A future node editor edits the ConstructionDef, and presets remain
// what they are here - constructors that produce a starting graph. Nothing about this layout
// forecloses that, which is what the brief actually asked for.
//
// EXTENSIBILITY (SS24). Adding a preset is one PresetDesc: an id, a display name, a category, a
// descriptor list naming the parameters it exposes, and a build function. No other file changes,
// because the editor enumerates the registry rather than switching on preset names.
#pragma once

#include "Construction/ConstructionParams.h"

namespace hbe::construction {

struct PresetDesc {
    const char* id = "";       // stable, serialized - NEVER renamed, same rule as component ids
    const char* name = "";     // shown in the picker
    const char* category = ""; // "Architectural", "Construction", "Infrastructure"
    const char* summary = "";

    // Expands parameters into a construction graph. Must be DETERMINISTIC and must clear `out`.
    void (*build)(const PresetParams& p, ConstructionDef& out) = nullptr;

    const ParamDesc* params = nullptr;
    u32 paramCount = 0;
};

// The registry. Stable order, safe to iterate for UI.
const PresetDesc* Presets(u32& outCount);
const PresetDesc* FindPreset(const char* id);

// Expands a preset. Returns false for an unknown id, leaving `out` empty rather than partially
// built - a half-expanded definition would validate as a real building with missing structure.
bool BuildPreset(const char* id, const PresetParams& p, ConstructionDef& out);

// Parameters with every value at the preset's declared default. Used when a preset is first
// chosen, and as the "Reset" button's target.
PresetParams DefaultParams(const char* id);

bool PresetSelfTest();

} // namespace hbe::construction
