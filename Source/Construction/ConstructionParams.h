// Construction/ConstructionParams.h - parameter reflection for procedural construction.
//
// WHY THIS EXISTS. The generators already expose every meaningful decision as a real parameter -
// brick dimensions, mortar thickness, bond pattern, stud spacing, plate count, board overlap.
// What was missing is any way for the EDITOR to know they exist. Without reflection, the
// inspector has to hardcode a widget per field, which means every new parameter is an edit in two
// places and every new preset is an edit in N places. That is the exact lockstep hazard this
// codebase has been bitten by repeatedly (the schematic catalog, the asset-format registry, the
// nine-site component wiring).
//
// So: one table describes the parameters, and the UI, serialization and defaults all read it.
// Adding a parameter becomes ONE row.
//
// THIS IS DELIBERATELY NOT A GENERAL REFLECTION SYSTEM. The engine has none - a repo-wide grep
// for entt::meta or any ComponentRegistry returns nothing - and inventing one to solve a
// construction-inspector problem would be a much larger and much riskier change than the problem
// justifies. This is ~200 lines describing one struct.
//
// GROUPING (brief SS5) is by consecutive runs of `group` in declaration order, NOT by sorting.
// That gives the preset author explicit control over both the group order and the order within a
// group, which matters: "Width, Height, Thickness" reads correctly and "Height, Thickness, Width"
// does not, and no sort key expresses that.
#pragma once

#include "Construction/ConstructionDef.h"

#include <cstddef>

namespace hbe::construction {

enum class ParamType : u8 { Float, Int, Bool, Enum, Seed, Count };

// One editable parameter, located by byte offset into PresetParams.
//
// Offsets rather than accessor pairs because the whole point is a compact declarative table; the
// risk that comes with them (a type/offset mismatch) is covered by ParamsSelfTest, which checks
// every descriptor lands inside the struct and matches the declared size.
struct ParamDesc {
    const char* group = "";   // "Dimensions", "Brick", "Pattern", ...
    const char* name = "";    // "Unit Length"
    const char* tooltip = ""; // why it matters, not what it is
    ParamType type = ParamType::Float;
    u32 offset = 0;
    u32 size = 0; // sizeof the field, checked against `type`
    f32 min = 0.0f;
    f32 max = 1.0f;
    f32 step = 0.01f;
    const char* const* enumNames = nullptr; // Enum only
    u32 enumCount = 0;
    const char* unit = ""; // "m", "mm", "deg" - shown by the UI, never applied to the value
};

// The parameter block every preset draws from.
//
// ONE STRUCT, NOT A VARIANT PER PRESET. Each preset publishes a DESCRIPTOR LIST that selects the
// subset it exposes, so a Brick Wall shows the masonry block and a Timber Wall shows the framing
// block while both serialize through the same flat field list. A variant would need a
// discriminator written, read and kept in lockstep with the preset id - the same two-place
// invariant that has broken things here before - and would buy nothing, because these blocks are
// small and a preset that ignores one simply never reads it.
struct PresetParams {
    // -- Dimensions --------------------------------------------------------
    f32 width = 8.0f;
    f32 depth = 6.0f;
    f32 height = 3.0f;
    f32 thickness = 0.25f;

    // -- Storeys -----------------------------------------------------------
    i32 floorCount = 1;
    f32 floorHeight = 2.7f;

    // -- Construction ------------------------------------------------------
    i32 structureMaterial = static_cast<i32>(MaterialKind::TimberFrame);
    i32 exteriorMaterial = static_cast<i32>(MaterialKind::Brick);
    i32 roofMaterial = static_cast<i32>(MaterialKind::WoodShingle);

    // -- Roof --------------------------------------------------------------
    f32 roofPitch = 35.0f;   // degrees
    f32 roofOverhang = 0.4f; // metres beyond the wall line

    // -- Openings ----------------------------------------------------------
    i32 windowCount = 2;
    f32 windowWidth = 1.2f;
    f32 windowHeight = 1.4f;
    f32 windowSill = 0.9f; // height of the sill above the floor
    i32 doorCount = 1;
    f32 doorWidth = 0.9f;
    f32 doorHeight = 2.1f;

    // -- Detail ------------------------------------------------------------
    u64 seed = 12345;

    // -- Construction-method blocks, reused verbatim from the generators ----
    MasonryParams masonry;
    WeatheringParams weathering;
    TimberParams timber;
    PlankParams plank;
    ShingleParams shingle;
};

// Typed access by descriptor. The UI uses these rather than reaching through the offset itself, so
// the cast lives in exactly one place.
f32 GetFloat(const PresetParams& p, const ParamDesc& d);
void SetFloat(PresetParams& p, const ParamDesc& d, f32 v);
i32 GetInt(const PresetParams& p, const ParamDesc& d);
void SetInt(PresetParams& p, const ParamDesc& d, i32 v);
bool GetBool(const PresetParams& p, const ParamDesc& d);
void SetBool(PresetParams& p, const ParamDesc& d, bool v);
u64 GetSeed(const PresetParams& p, const ParamDesc& d);
void SetSeed(PresetParams& p, const ParamDesc& d, u64 v);

// Clamps to the descriptor's range. Called on every write, because a UI is not the only writer -
// a .hbbuild from a newer version, or a hand-edited file, can carry anything.
void ClampToRange(PresetParams& p, const ParamDesc& d);
void ClampAll(PresetParams& p, const ParamDesc* descs, u32 count);

// Shared enum name tables, so a preset's descriptor row does not have to restate them.
const char* const* MaterialKindNames(u32& outCount);
const char* const* BondPatternNames(u32& outCount);
const char* const* SidingProfileNames(u32& outCount);
const char* const* BoardDirectionNames(u32& outCount);

bool ParamsSelfTest();

} // namespace hbe::construction
