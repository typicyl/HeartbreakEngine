#include "Construction/ConstructionPreset.h"

#include "Construction/ConstructionGeometry.h"
#include "Construction/ConstructionGraph.h"

#include <glm/gtc/quaternion.hpp>
#include <string>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hbe::construction {

namespace {

// Descriptor shorthands. These keep the tables readable enough to review as DATA, which is the
// point - a parameter table nobody can scan is one nobody will notice a wrong range in.
#define P_FLOAT(field, grp, nm, lo, hi, st, un, tip)                                              \
    ParamDesc {                                                                                   \
        grp, nm, tip, ParamType::Float, static_cast<u32>(offsetof(PresetParams, field)),           \
            static_cast<u32>(sizeof(PresetParams::field)), lo, hi, st, nullptr, 0, un             \
    }

#define P_SUB(block, field, grp, nm, lo, hi, st, un, tip)                                          \
    ParamDesc {                                                                                    \
        grp, nm, tip, ParamType::Float,                                                            \
            static_cast<u32>(offsetof(PresetParams, block) + offsetof(decltype(PresetParams::block), field)), \
            static_cast<u32>(sizeof(decltype(PresetParams::block)::field)), lo, hi, st, nullptr, 0, un \
    }

#define P_INT(field, grp, nm, lo, hi, tip)                                                        \
    ParamDesc {                                                                                   \
        grp, nm, tip, ParamType::Int, static_cast<u32>(offsetof(PresetParams, field)),             \
            static_cast<u32>(sizeof(PresetParams::field)), lo, hi, 1.0f, nullptr, 0, ""           \
    }

#define P_SEED(field, grp, nm, tip)                                                               \
    ParamDesc {                                                                                   \
        grp, nm, tip, ParamType::Seed, static_cast<u32>(offsetof(PresetParams, field)),            \
            static_cast<u32>(sizeof(PresetParams::field)), 0, 0, 0, nullptr, 0, ""                \
    }

ParamDesc MakeEnum(const char* grp, const char* nm, const char* tip, u32 offset, u32 size,
                   const char* const* names, u32 count) {
    ParamDesc d;
    d.group = grp;
    d.name = nm;
    d.tooltip = tip;
    d.type = ParamType::Enum;
    d.offset = offset;
    d.size = size;
    d.enumNames = names;
    d.enumCount = count;
    d.max = static_cast<f32>(count ? count - 1 : 0);
    return d;
}

const char* const* MatNames() {
    u32 n = 0;
    return MaterialKindNames(n);
}
u32 MatCount() {
    u32 n = 0;
    MaterialKindNames(n);
    return n;
}
const char* const* BondNames() {
    u32 n = 0;
    return BondPatternNames(n);
}
const char* const* ProfileNames() {
    u32 n = 0;
    return SidingProfileNames(n);
}

// ---------------------------------------------------------------------------
// Shared builders. Presets COMPOSE these rather than duplicating them, which is what makes
// "House" a combination of the same tools the artist can reach individually (brief SS28).
// ---------------------------------------------------------------------------

ComponentId AddComp(ConstructionDef& d, ComponentKind kind, StructuralRole role, MaterialKind mat,
                    glm::vec3 pos, glm::vec3 extent, ComponentId parent = kInvalidComponent) {
    ConstructionComponent c;
    c.kind = kind;
    c.role = role;
    c.material = mat;
    c.position = pos;
    c.extent = extent;
    c.parent = parent;
    return d.AddComponent(std::move(c));
}

void ApplyMethodParams(ConstructionDef& d, ComponentId id, const PresetParams& p) {
    if (ConstructionComponent* c = d.Find(id)) {
        c->masonry = p.masonry;
        c->weathering = p.weathering;
        c->timber = p.timber;
        c->plank = p.plank;
        c->shingle = p.shingle;
    }
}

// Evenly spaced openings along a wall, inset from the ends so an opening never lands on the
// corner where the framing has to be continuous.
void AddOpenings(ConstructionDef& d, ComponentId wall, i32 count, f32 w, f32 h, f32 sillFromBase,
                 f32 wallHalfLen, f32 wallHalfHeight, f32 wallHalfDepth) {
    if (count <= 0 || w <= 0.0f || h <= 0.0f) return;
    const f32 usable = wallHalfLen * 2.0f - w * 2.0f;
    if (usable <= 0.0f) return;
    for (i32 i = 0; i < count; ++i) {
        const f32 t = (count == 1) ? 0.5f : static_cast<f32>(i) / static_cast<f32>(count - 1);
        const f32 x = -usable * 0.5f + usable * t;
        const f32 y = -wallHalfHeight + sillFromBase + h * 0.5f;
        if (y + h * 0.5f > wallHalfHeight) continue; // would breach the plate line
        ConstructionComponent o;
        o.kind = ComponentKind::Opening;
        o.role = StructuralRole::None;
        o.parent = wall;
        o.position = glm::vec3(x, y, 0.0f);
        // Depth deliberately over-reaches the wall so the cut passes right through rather than
        // leaving a paper-thin membrane at the back that z-fights.
        o.extent = glm::vec3(w * 0.5f, h * 0.5f, wallHalfDepth * 2.0f);
        d.AddComponent(std::move(o));
    }
}

// ---------------------------------------------------------------------------
// Preset: Wall
// ---------------------------------------------------------------------------

void BuildWall(const PresetParams& p, ConstructionDef& out) {
    out = ConstructionDef{};
    out.seed = p.seed;
    const auto mat = static_cast<MaterialKind>(p.exteriorMaterial);
    const glm::vec3 e(p.width * 0.5f, p.height * 0.5f, p.thickness * 0.5f);

    const ComponentId foundation =
        AddComp(out, ComponentKind::Foundation, StructuralRole::Foundation,
                MaterialKind::PouredConcrete, glm::vec3(0.0f, -e.y - 0.15f, 0.0f),
                glm::vec3(e.x, 0.15f, e.z * 1.2f));
    const ComponentId wall =
        AddComp(out, ComponentKind::Wall, StructuralRole::LoadBearing, mat, glm::vec3(0.0f), e);
    ApplyMethodParams(out, wall, p);
    out.AddEdge(wall, foundation, EdgeKind::Bears);

    AddOpenings(out, wall, p.windowCount, p.windowWidth, p.windowHeight, p.windowSill, e.x, e.y,
                e.z);
    AddOpenings(out, wall, p.doorCount, p.doorWidth, p.doorHeight, 0.0f, e.x, e.y, e.z);
}

const ParamDesc kWallParams[] = {
    P_FLOAT(width, "Dimensions", "Width", 0.5f, 60.0f, 0.1f, "m", "Length of the wall"),
    P_FLOAT(height, "Dimensions", "Height", 0.5f, 20.0f, 0.1f, "m", "Floor to top of wall"),
    P_FLOAT(thickness, "Dimensions", "Thickness", 0.05f, 2.0f, 0.01f, "m", "Wall depth"),

    MakeEnum("Construction", "Material", "Chooses the construction RULES, not just a texture",
             static_cast<u32>(offsetof(PresetParams, exteriorMaterial)), sizeof(i32), MatNames(),
             MatCount()),

    P_SUB(masonry, unitLength, "Masonry", "Unit Length", 0.05f, 1.0f, 0.005f, "m", "Brick length"),
    P_SUB(masonry, unitHeight, "Masonry", "Unit Height", 0.02f, 0.5f, 0.005f, "m", "Course height"),
    P_SUB(masonry, unitDepth, "Masonry", "Unit Depth", 0.02f, 0.5f, 0.005f, "m", "Into the wall"),
    P_SUB(masonry, joint, "Masonry", "Mortar Joint", 0.0f, 0.05f, 0.001f, "m", "Bed and perpend"),
    MakeEnum("Masonry", "Bond", "How courses offset against each other",
             static_cast<u32>(offsetof(PresetParams, masonry) + offsetof(MasonryParams, bond)),
             sizeof(BondPattern), BondNames(), static_cast<u32>(BondPattern::Count)),

    P_SUB(masonry, sizeJitter, "Variation", "Size", 0.0f, 1.0f, 0.01f, "",
          "Units only ever shrink - growing them would close the joints"),
    P_SUB(masonry, depthJitter, "Variation", "Depth", 0.0f, 1.0f, 0.01f, "", "In/out protrusion"),
    P_SUB(masonry, rotJitter, "Variation", "Rotation", 0.0f, 0.2f, 0.005f, "rad", "Wobble"),

    P_SUB(timber, spacing, "Framing", "Stud Spacing", 0.1f, 1.2f, 0.05f, "m", "On centre"),
    P_SUB(timber, memberWidth, "Framing", "Member Width", 0.01f, 0.3f, 0.005f, "m", "Narrow face"),
    P_SUB(timber, memberDepth, "Framing", "Member Depth", 0.01f, 0.4f, 0.005f, "m", "Deep face"),
    P_INT(floorCount, "Framing", "Top Plates", 1, 3, "A double top plate is standard"),

    MakeEnum("Cladding", "Siding Profile", "Clapboard overlaps and tilts; flush does not",
             static_cast<u32>(offsetof(PresetParams, plank) + offsetof(PlankParams, profile)),
             sizeof(SidingProfile), ProfileNames(), static_cast<u32>(SidingProfile::Count)),
    P_SUB(plank, boardWidth, "Cladding", "Board Width", 0.02f, 0.6f, 0.005f, "m", ""),
    P_SUB(plank, overlap, "Cladding", "Overlap", 0.0f, 0.2f, 0.005f, "m", "Clapboard/shiplap lap"),

    P_INT(windowCount, "Openings", "Window Count", 0, 24, "Evenly spaced along the wall"),
    P_FLOAT(windowWidth, "Openings", "Window Width", 0.2f, 5.0f, 0.05f, "m", ""),
    P_FLOAT(windowHeight, "Openings", "Window Height", 0.2f, 4.0f, 0.05f, "m", ""),
    P_FLOAT(windowSill, "Openings", "Sill Height", 0.0f, 3.0f, 0.05f, "m", "Above the floor"),
    P_INT(doorCount, "Openings", "Door Count", 0, 8, ""),
    P_FLOAT(doorWidth, "Openings", "Door Width", 0.4f, 4.0f, 0.05f, "m", ""),
    P_FLOAT(doorHeight, "Openings", "Door Height", 1.2f, 4.0f, 0.05f, "m", ""),

    P_SUB(weathering, age, "Weathering", "Age", 0.0f, 1.0f, 0.01f, "",
          "The master dial: 0 = as built, 1 = derelict. Everything else scales off it"),
    P_SUB(weathering, moisture, "Weathering", "Moisture", 0.0f, 1.0f, 0.01f, "",
          "Rot, moss and darkening in sheltered places"),
    P_SUB(weathering, exposure, "Weathering", "Exposure", 0.0f, 1.0f, 0.01f, "",
          "Sun and wind: fading, splitting, lifted edges"),
    P_SUB(weathering, maintenance, "Weathering", "Maintenance", 0.0f, 1.0f, 0.01f, "",
          "1 = kept up, 0 = abandoned. Divides the damage"),

    P_SEED(seed, "Detail", "Seed", "Same seed and parameters always produce the same wall"),
};

// ---------------------------------------------------------------------------
// Preset: Floor
// ---------------------------------------------------------------------------

void BuildFloor(const PresetParams& p, ConstructionDef& out) {
    out = ConstructionDef{};
    out.seed = p.seed;
    const glm::vec3 e(p.width * 0.5f, p.thickness * 0.5f, p.depth * 0.5f);
    const ComponentId f =
        AddComp(out, ComponentKind::Foundation, StructuralRole::Foundation,
                MaterialKind::PouredConcrete, glm::vec3(0.0f, -e.y - 0.15f, 0.0f),
                glm::vec3(e.x, 0.15f, e.z));
    const ComponentId floor =
        AddComp(out, ComponentKind::Floor, StructuralRole::NonLoadBearing,
                static_cast<MaterialKind>(p.structureMaterial), glm::vec3(0.0f), e);
    ApplyMethodParams(out, floor, p);
    out.AddEdge(floor, f, EdgeKind::Bears);
}

const ParamDesc kFloorParams[] = {
    P_FLOAT(width, "Dimensions", "Width", 0.5f, 60.0f, 0.1f, "m", ""),
    P_FLOAT(depth, "Dimensions", "Depth", 0.5f, 60.0f, 0.1f, "m", ""),
    P_FLOAT(thickness, "Dimensions", "Depth of Structure", 0.05f, 2.0f, 0.01f, "m",
            "Joist depth plus the deck"),
    MakeEnum("Construction", "Material", "Timber frames into joists; concrete stays a slab",
             static_cast<u32>(offsetof(PresetParams, structureMaterial)), sizeof(i32), MatNames(),
             MatCount()),
    P_SUB(timber, spacing, "Framing", "Joist Spacing", 0.1f, 1.2f, 0.05f, "m", "On centre"),
    P_SUB(timber, memberWidth, "Framing", "Joist Width", 0.01f, 0.3f, 0.005f, "m", ""),
    P_SUB(timber, memberDepth, "Framing", "Joist Depth", 0.02f, 0.6f, 0.005f, "m", ""),
    P_SUB(plank, boardThickness, "Deck", "Deck Thickness", 0.005f, 0.1f, 0.001f, "m", ""),
    P_SEED(seed, "Detail", "Seed", ""),
};

// ---------------------------------------------------------------------------
// Preset: Roof
// ---------------------------------------------------------------------------

void BuildRoof(const PresetParams& p, ConstructionDef& out) {
    out = ConstructionDef{};
    out.seed = p.seed;
    // Pitch drives the rise: a 35 degree roof over a 6 m span rises about 2.1 m. Exposing the
    // ANGLE rather than the rise is what an artist actually thinks in.
    const f32 halfSpan = p.depth * 0.5f + p.roofOverhang;
    const f32 rise = std::tan(p.roofPitch * 3.14159265f / 180.0f) * halfSpan;
    const glm::vec3 e(p.width * 0.5f + p.roofOverhang, std::max(rise, 0.05f) * 0.5f, halfSpan);
    const ComponentId roof =
        AddComp(out, ComponentKind::Roof, StructuralRole::NonLoadBearing,
                static_cast<MaterialKind>(p.roofMaterial), glm::vec3(0.0f), e);
    ApplyMethodParams(out, roof, p);
}

const ParamDesc kRoofParams[] = {
    P_FLOAT(width, "Dimensions", "Width", 0.5f, 60.0f, 0.1f, "m", "Along the ridge"),
    P_FLOAT(depth, "Dimensions", "Span", 0.5f, 40.0f, 0.1f, "m", "Eave to eave"),
    P_FLOAT(roofPitch, "Shape", "Pitch", 5.0f, 70.0f, 1.0f, "deg",
            "The angle, not the rise - what an artist actually thinks in"),
    P_FLOAT(roofOverhang, "Shape", "Overhang", 0.0f, 2.0f, 0.05f, "m", "Beyond the wall line"),
    MakeEnum("Construction", "Material", "Shingle lays courses; anything else stays a solid prism",
             static_cast<u32>(offsetof(PresetParams, roofMaterial)), sizeof(i32), MatNames(),
             MatCount()),
    P_SUB(shingle, width, "Shingles", "Width", 0.05f, 1.0f, 0.01f, "m", ""),
    P_SUB(shingle, length, "Shingles", "Length", 0.05f, 1.5f, 0.01f, "m", ""),
    P_SUB(shingle, exposure, "Shingles", "Exposure", 0.02f, 1.0f, 0.01f, "m",
          "How much of each course stays visible below the one above"),
    P_SUB(shingle, jitter, "Variation", "Shingle Jitter", 0.0f, 1.0f, 0.01f, "", ""),
    P_SEED(seed, "Detail", "Seed", ""),
};

// ---------------------------------------------------------------------------
// Preset: House - COMPOSED from the same builders above (brief SS28)
// ---------------------------------------------------------------------------

void BuildHouse(const PresetParams& p, ConstructionDef& out) {
    out = ConstructionDef{};
    out.seed = p.seed;

    const f32 hw = p.width * 0.5f, hd = p.depth * 0.5f;
    const f32 wallT = p.thickness * 0.5f;
    const i32 storeys = p.floorCount < 1 ? 1 : p.floorCount;
    const f32 storeyH = p.floorHeight;

    const ComponentId building =
        AddComp(out, ComponentKind::Building, StructuralRole::None, MaterialKind::Unknown,
                glm::vec3(0.0f), glm::vec3(hw, storeyH * storeys * 0.5f, hd));

    const ComponentId foundation =
        AddComp(out, ComponentKind::Foundation, StructuralRole::Foundation,
                MaterialKind::PouredConcrete, glm::vec3(0.0f, -0.2f, 0.0f),
                glm::vec3(hw + 0.1f, 0.2f, hd + 0.1f), building);

    const auto structMat = static_cast<MaterialKind>(p.structureMaterial);
    const auto extMat = static_cast<MaterialKind>(p.exteriorMaterial);

    for (i32 s = 0; s < storeys; ++s) {
        const f32 base = static_cast<f32>(s) * storeyH;
        const f32 midY = base + storeyH * 0.5f;

        // Floor deck for this storey.
        const ComponentId floor =
            AddComp(out, ComponentKind::Floor, StructuralRole::NonLoadBearing, structMat,
                    glm::vec3(0.0f, base + 0.1f, 0.0f), glm::vec3(hw, 0.1f, hd), building);
        ApplyMethodParams(out, floor, p);

        // FOUR WALLS. The two along X get the openings; the gable ends stay solid, because a
        // window in a gable end is a decision an artist makes deliberately rather than something
        // a preset should scatter there by default.
        for (int side = 0; side < 4; ++side) {
            const bool alongX = side < 2;
            const f32 sign = (side % 2) ? 1.0f : -1.0f;
            const glm::vec3 pos = alongX ? glm::vec3(0.0f, midY, sign * (hd - wallT))
                                         : glm::vec3(sign * (hw - wallT), midY, 0.0f);
            const glm::vec3 ext = alongX ? glm::vec3(hw, storeyH * 0.5f, wallT)
                                         : glm::vec3(hd - wallT * 2.0f, storeyH * 0.5f, wallT);

            ConstructionComponent w;
            w.kind = ComponentKind::Wall;
            w.role = StructuralRole::LoadBearing;
            w.material = extMat;
            w.parent = building;
            w.position = pos;
            w.extent = ext;
            if (!alongX) w.rotation = glm::angleAxis(1.5707963f, glm::vec3(0, 1, 0));
            const ComponentId wall = out.AddComponent(std::move(w));
            ApplyMethodParams(out, wall, p);

            out.AddEdge(wall, foundation, EdgeKind::Bears);
            out.AddEdge(floor, wall, EdgeKind::Bears, 0.25f);

            if (alongX) {
                AddOpenings(out, wall, p.windowCount, p.windowWidth, p.windowHeight, p.windowSill,
                            ext.x, ext.y, ext.z);
                // The front door goes on the ground storey only - a door on the first floor
                // opening onto nothing is the classic procedural-building tell.
                if (s == 0 && side == 0)
                    AddOpenings(out, wall, p.doorCount, p.doorWidth, p.doorHeight, 0.0f, ext.x,
                                ext.y, ext.z);
            }
        }
    }

    // Roof over the top storey.
    const f32 eaveY = static_cast<f32>(storeys) * storeyH;
    const f32 halfSpan = hd + p.roofOverhang;
    const f32 rise = std::tan(p.roofPitch * 3.14159265f / 180.0f) * halfSpan;
    const ComponentId roof = AddComp(
        out, ComponentKind::Roof, StructuralRole::NonLoadBearing,
        static_cast<MaterialKind>(p.roofMaterial),
        glm::vec3(0.0f, eaveY + std::max(rise, 0.05f) * 0.5f, 0.0f),
        glm::vec3(hw + p.roofOverhang, std::max(rise, 0.05f) * 0.5f, halfSpan), building);
    ApplyMethodParams(out, roof, p);

    // The roof rests on every top-storey wall, so losing one wall does not drop it.
    for (const ConstructionComponent& c : out.components)
        if (c.kind == ComponentKind::Wall) out.AddEdge(roof, c.id, EdgeKind::Bears, 0.25f);
}

const ParamDesc kHouseParams[] = {
    P_FLOAT(width, "Dimensions", "Width", 2.0f, 80.0f, 0.5f, "m", ""),
    P_FLOAT(depth, "Dimensions", "Depth", 2.0f, 80.0f, 0.5f, "m", ""),
    P_FLOAT(thickness, "Dimensions", "Wall Thickness", 0.05f, 1.0f, 0.01f, "m", ""),

    P_INT(floorCount, "Storeys", "Floor Count", 1, 8, ""),
    P_FLOAT(floorHeight, "Storeys", "Floor Height", 1.8f, 6.0f, 0.1f, "m", "Floor to floor"),

    MakeEnum("Structure", "Structure", "Timber frames into studs and joists",
             static_cast<u32>(offsetof(PresetParams, structureMaterial)), sizeof(i32), MatNames(),
             MatCount()),
    P_SUB(timber, spacing, "Structure", "Stud Spacing", 0.1f, 1.2f, 0.05f, "m", "On centre"),
    P_SUB(timber, memberWidth, "Structure", "Member Width", 0.01f, 0.3f, 0.005f, "m", ""),
    P_SUB(timber, memberDepth, "Structure", "Member Depth", 0.01f, 0.4f, 0.005f, "m", ""),

    MakeEnum("Exterior", "Exterior", "Brick lays courses; timber frames; concrete stays solid",
             static_cast<u32>(offsetof(PresetParams, exteriorMaterial)), sizeof(i32), MatNames(),
             MatCount()),
    MakeEnum("Exterior", "Bond", "",
             static_cast<u32>(offsetof(PresetParams, masonry) + offsetof(MasonryParams, bond)),
             sizeof(BondPattern), BondNames(), static_cast<u32>(BondPattern::Count)),
    P_SUB(masonry, unitLength, "Exterior", "Brick Length", 0.05f, 1.0f, 0.005f, "m", ""),
    P_SUB(masonry, unitHeight, "Exterior", "Brick Height", 0.02f, 0.5f, 0.005f, "m", ""),
    P_SUB(masonry, joint, "Exterior", "Mortar Joint", 0.0f, 0.05f, 0.001f, "m", ""),

    MakeEnum("Roof", "Roof Material", "",
             static_cast<u32>(offsetof(PresetParams, roofMaterial)), sizeof(i32), MatNames(),
             MatCount()),
    P_FLOAT(roofPitch, "Roof", "Pitch", 5.0f, 70.0f, 1.0f, "deg", ""),
    P_FLOAT(roofOverhang, "Roof", "Overhang", 0.0f, 2.0f, 0.05f, "m", ""),
    P_SUB(shingle, exposure, "Roof", "Shingle Exposure", 0.02f, 1.0f, 0.01f, "m", ""),

    P_INT(windowCount, "Openings", "Windows per Wall", 0, 24, ""),
    P_FLOAT(windowWidth, "Openings", "Window Width", 0.2f, 5.0f, 0.05f, "m", ""),
    P_FLOAT(windowHeight, "Openings", "Window Height", 0.2f, 4.0f, 0.05f, "m", ""),
    P_FLOAT(windowSill, "Openings", "Sill Height", 0.0f, 3.0f, 0.05f, "m", ""),
    P_INT(doorCount, "Openings", "Doors", 0, 4, "Ground storey only"),
    P_FLOAT(doorWidth, "Openings", "Door Width", 0.4f, 4.0f, 0.05f, "m", ""),
    P_FLOAT(doorHeight, "Openings", "Door Height", 1.2f, 4.0f, 0.05f, "m", ""),

    P_SUB(masonry, sizeJitter, "Variation", "Brick Size", 0.0f, 1.0f, 0.01f, "", ""),
    P_SUB(timber, warp, "Variation", "Lumber Warp", 0.0f, 1.0f, 0.01f, "", ""),
    P_SUB(weathering, age, "Weathering", "Age", 0.0f, 1.0f, 0.01f, "",
          "0 = as built, 1 = derelict"),
    P_SUB(weathering, moisture, "Weathering", "Moisture", 0.0f, 1.0f, 0.01f, "", ""),
    P_SUB(weathering, exposure, "Weathering", "Exposure", 0.0f, 1.0f, 0.01f, "", ""),
    P_SUB(weathering, maintenance, "Weathering", "Maintenance", 0.0f, 1.0f, 0.01f, "",
          "1 = kept up, 0 = abandoned"),

    P_SEED(seed, "Detail", "Seed", "Same seed and parameters always produce the same house"),
};

#undef P_FLOAT
#undef P_SUB
#undef P_INT
#undef P_SEED

const PresetDesc kPresets[] = {
    {"wall", "Wall", "Construction",
     "One wall in any construction method, with openings.", &BuildWall, kWallParams,
     static_cast<u32>(sizeof(kWallParams) / sizeof(kWallParams[0]))},
    {"floor", "Floor", "Construction", "A floor deck, framed or solid.", &BuildFloor, kFloorParams,
     static_cast<u32>(sizeof(kFloorParams) / sizeof(kFloorParams[0]))},
    {"roof", "Roof", "Construction", "A pitched roof, optionally shingled.", &BuildRoof,
     kRoofParams, static_cast<u32>(sizeof(kRoofParams) / sizeof(kRoofParams[0]))},
    {"house", "House", "Architectural",
     "Foundation, storeys, four walls, openings and a roof - composed from the same builders the "
     "individual presets use.",
     &BuildHouse, kHouseParams,
     static_cast<u32>(sizeof(kHouseParams) / sizeof(kHouseParams[0]))},
};

u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("preset FAIL: %s\n", what);
        ++g_fails;
    }
}

} // namespace

const PresetDesc* Presets(u32& outCount) {
    outCount = static_cast<u32>(sizeof(kPresets) / sizeof(kPresets[0]));
    return kPresets;
}

const PresetDesc* FindPreset(const char* id) {
    if (!id) return nullptr;
    for (const PresetDesc& d : kPresets)
        if (std::strcmp(d.id, id) == 0) return &d;
    return nullptr;
}

bool BuildPreset(const char* id, const PresetParams& p, ConstructionDef& out) {
    const PresetDesc* d = FindPreset(id);
    if (!d || !d->build) {
        out = ConstructionDef{};
        return false;
    }
    // LOCKED COMPONENTS SURVIVE A REBUILD. This is what "generate 90% of a building procedurally
    // and hand-author the remaining 10%" actually requires (brief SS18): without it, the one wall
    // an artist carefully positioned is destroyed by the next touch of any unrelated slider, and
    // the feature is a lie. Captured BEFORE the rebuild because `out` may alias nothing useful
    // afterwards.
    std::vector<ConstructionComponent> keep;
    for (const ConstructionComponent& c : out.components)
        if (c.locked) keep.push_back(c);
    // Clamped before building, never after. A parameter out of range is not a cosmetic problem
    // here - a negative stud spacing or a zero brick would reach a generator that would then
    // refuse or cap, and the artist would see "nothing generated" with no explanation.
    PresetParams safe = p;
    ClampAll(safe, d->params, d->paramCount);
    d->build(safe, out);

    // Re-attached AFTER the rebuild, keeping their original ids so every damage record, support
    // edge and override that named them still resolves. Their ids came from a previous mint, so
    // the counter is pushed past them - otherwise the next AddComponent would collide.
    for (const ConstructionComponent& c : keep) {
        // A preset that happened to mint the same id must not end up with two components sharing
        // an identity; the locked one wins, because it is the artist's.
        out.components.erase(std::remove_if(out.components.begin(), out.components.end(),
                                            [&](const ConstructionComponent& x) {
                                                return x.id == c.id;
                                            }),
                             out.components.end());
        out.components.push_back(c);
        if (c.id >= out.nextId) out.nextId = c.id + 1;
    }
    return true;
}

PresetParams DefaultParams(const char* id) {
    PresetParams p;
    if (const PresetDesc* d = FindPreset(id)) ClampAll(p, d->params, d->paramCount);
    return p;
}

bool PresetSelfTest() {
    g_fails = 0;

    u32 n = 0;
    const PresetDesc* all = Presets(n);
    Check(n >= 4, "the registry publishes the initial presets");

    for (u32 i = 0; i < n; ++i) {
        const PresetDesc& d = all[i];
        Check(FindPreset(d.id) == &d, "every preset is findable by its stable id");
        Check(d.category && *d.category, "every preset declares a category for the picker");

        ConstructionDef def;
        Check(BuildPreset(d.id, DefaultParams(d.id), def), "every preset builds at its defaults");
        Check(!def.components.empty(), "...and produces components");

        std::vector<std::string> errs;
        Check(def.Validate(errs),
              "A PRESET MUST PRODUCE A VALID DEFINITION - an invalid one would validate as a real "
              "building with missing structure");

        // Determinism: the whole caching and destruction-persistence story depends on it.
        ConstructionDef again;
        BuildPreset(d.id, DefaultParams(d.id), again);
        Check(again.components.size() == def.components.size(),
              "the same preset and parameters always produce the same graph");
        Check(again.edges.size() == def.edges.size(), "...including the same support edges");

        // It must generate geometry, and the capacity must cover it.
        SectionMesh mesh;
        BuildSection(def, nullptr, kInvalidComponent, mesh);
        Check(mesh.TotalIndices() > 0, "every preset generates geometry at its defaults");
        const MeshCapacity cap = EstimateCapacity(def, kInvalidComponent);
        Check(cap.vertices >= mesh.TotalVertices(),
              "the reservation covers what the preset generates");
    }

    Check(FindPreset("nope") == nullptr, "an unknown preset id is not found");
    {
        ConstructionDef def;
        Check(!BuildPreset("nope", PresetParams{}, def), "...and refuses to build");
        Check(def.components.empty(),
              "A FAILED BUILD LEAVES NOTHING - a half-expanded definition would look like a real "
              "building with missing structure");
    }

    // Parameters must actually DO something. A preset that ignores its own parameters is the
    // failure mode this whole design exists to prevent.
    {
        PresetParams a = DefaultParams("wall");
        PresetParams b = a;
        b.width = a.width * 2.0f;
        ConstructionDef da, db;
        BuildPreset("wall", a, da);
        BuildPreset("wall", b, db);
        SectionMesh ma, mb;
        BuildSection(da, nullptr, kInvalidComponent, ma);
        BuildSection(db, nullptr, kInvalidComponent, mb);
        Check(mb.TotalIndices() > ma.TotalIndices(),
              "A WIDER WALL IS MORE GEOMETRY - a preset that ignores its own parameters is exactly "
              "the 'uneditable generated result' this design exists to prevent");

        PresetParams c = a;
        c.exteriorMaterial = static_cast<i32>(MaterialKind::TimberFrame);
        ConstructionDef dc;
        BuildPreset("wall", c, dc);
        SectionMesh mc;
        BuildSection(dc, nullptr, kInvalidComponent, mc);
        Check(mc.TotalIndices() != ma.TotalIndices(),
              "changing the construction METHOD changes the construction, not just a texture");

        // Seed independence at the preset level.
        PresetParams s = a;
        s.seed = a.seed + 1;
        ConstructionDef ds;
        BuildPreset("wall", s, ds);
        Check(ds.seed != da.seed, "the seed reaches the definition");
    }

    // LOCKED COMPONENTS SURVIVE A REBUILD (brief SS18). Without this, the one wall an artist
    // hand-positioned is destroyed by the next touch of any unrelated slider, and "generate 90%,
    // hand-author 10%" is a lie.
    {
        PresetParams p = DefaultParams("wall");
        ConstructionDef d;
        BuildPreset("wall", p, d);
        Check(!d.components.empty(), "the fixture has components");

        // The artist positions and locks one piece.
        const ComponentId lockedId = d.components[1].id;
        d.components[1].locked = true;
        d.components[1].name = "hand-placed";
        d.components[1].position = glm::vec3(3.25f, 1.5f, 0.0f);
        const usize before = d.components.size();

        // ...then changes something entirely unrelated and rebuilds.
        p.width *= 1.5f;
        BuildPreset("wall", p, d);

        const ConstructionComponent* survivor = d.Find(lockedId);
        Check(survivor != nullptr,
              "A LOCKED COMPONENT SURVIVES A PRESET REBUILD - otherwise every hand-authored piece "
              "is destroyed by the next unrelated slider move");
        Check(survivor && survivor->locked, "...still locked");
        Check(survivor && survivor->name == "hand-placed", "...keeping its name");
        Check(survivor && std::fabs(survivor->position.x - 3.25f) < 1e-4f,
              "...and its hand-authored position");
        Check(survivor && survivor->id == lockedId,
              "...AND ITS ID, so every damage record and support edge naming it still resolves");

        // No duplicate identity, and the mint counter must be past the reattached id.
        u32 seen = 0;
        for (const ConstructionComponent& c : d.components)
            if (c.id == lockedId) ++seen;
        Check(seen == 1, "the locked component appears exactly once, not twice");
        Check(d.nextId > lockedId,
              "the id mint is pushed past a reattached component - otherwise the next add would "
              "collide with it");
        std::vector<std::string> errs;
        Check(d.Validate(errs), "the rebuilt definition is still valid");
        (void)before;

        // An UNLOCKED component is rebuilt normally.
        ConstructionDef fresh;
        BuildPreset("wall", p, fresh);
        bool anyLocked = false;
        for (const ConstructionComponent& c : fresh.components)
            if (c.locked) anyLocked = true;
        Check(!anyLocked, "a rebuild into a fresh definition carries no locked components");
    }

    // Openings authored by a preset must really cut.
    {
        PresetParams p = DefaultParams("wall");
        p.windowCount = 3;
        p.doorCount = 1;
        ConstructionDef d;
        BuildPreset("wall", p, d);
        u32 openings = 0;
        for (const ConstructionComponent& c : d.components)
            if (c.kind == ComponentKind::Opening) ++openings;
        Check(openings == 4, "a preset's opening counts produce that many cutters");

        PresetParams none = p;
        none.windowCount = 0;
        none.doorCount = 0;
        ConstructionDef dn;
        BuildPreset("wall", none, dn);
        SectionMesh withHoles, solid;
        BuildSection(d, nullptr, kInvalidComponent, withHoles);
        BuildSection(dn, nullptr, kInvalidComponent, solid);
        Check(withHoles.TotalIndices() != solid.TotalIndices(),
              "...and those cutters actually change the geometry");
    }

    // The House must be a real structure, not a box: multi-storey, anchored, and its roof must
    // survive losing one wall.
    {
        PresetParams p = DefaultParams("house");
        p.floorCount = 2;
        ConstructionDef d;
        BuildPreset("house", p, d);

        u32 walls = 0, floors = 0, roofs = 0;
        for (const ConstructionComponent& c : d.components) {
            if (c.kind == ComponentKind::Wall) ++walls;
            if (c.kind == ComponentKind::Floor) ++floors;
            if (c.kind == ComponentKind::Roof) ++roofs;
        }
        Check(walls == 8, "two storeys of four walls");
        Check(floors == 2, "one floor deck per storey");
        Check(roofs == 1, "one roof");

        ConstructionGraph g;
        g.Build(d);
        Check(g.Unsupported().empty(),
              "A GENERATED HOUSE MUST STAND UP - nothing in it may be structurally stranded");

        ComponentId aWall = kInvalidComponent;
        for (const ConstructionComponent& c : d.components)
            if (c.kind == ComponentKind::Wall) {
                aWall = c.id;
                break;
            }
        ComponentId theRoof = kInvalidComponent;
        for (const ConstructionComponent& c : d.components)
            if (c.kind == ComponentKind::Roof) theRoof = c.id;
        const auto stranded = g.UnsupportedIfRemoved({aWall});
        bool roofFell = false;
        for (ComponentId id : stranded)
            if (id == theRoof) roofFell = true;
        Check(!roofFell,
              "THE ROOF RESTS ON EVERY WALL, so losing one does not drop it - a roof bearing on a "
              "single wall is the structural equivalent of a hardcoded mesh");
    }

    if (g_fails == 0)
        std::printf("preset: presets are FUNCTIONS FROM PARAMETERS TO A GRAPH, not baked assets - "
                    "each builds a valid, deterministic, standing structure whose parameters "
                    "measurably change the result, whose opening counts really cut, and whose "
                    "House composes the same builders the individual presets expose\n");
    return g_fails == 0;
}

} // namespace hbe::construction
