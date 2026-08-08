// Construction/ConstructionDef.h - the procedural construction DEFINITION.
//
// THE ONE IDEA. This is the source of truth for a building. Meshes, colliders, fracture data
// and debris are DERIVED from it and are never authored, never serialized into a .hbscene, and
// never round-trip. The engine forces this rather than merely preferring it: an entity carrying
// a MeshInstance with no resolvable MeshRef silently saves an empty mesh source and reloads with
// no geometry (SceneSerializer.cpp:649-651 writes it, :2805 skips it), and undo restores through
// LoadMode::Replace -> DestroyWorld. Anything not reconstructible from serialized parameters is
// destroyed by Ctrl+Z. So the definition is small, complete, and the only thing that persists.
//
// WHY A FLAT ARRAY AND NOT A TREE. A tree cannot say "this beam is supported by two walls", and
// the support relationships ARE the system - they are what makes damage propagate and what a
// future destruction pass consumes. Containment (a Wall owns its Studs) is one field; support is
// a separate edge list. Conflating them is the mistake this layout exists to avoid.
//
// See docs/Design-ProceduralConstruction.md for the full design, including the four things the
// original brief assumed exist that do not.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace hbe::construction {

// ---------------------------------------------------------------------------
// Taxonomy
//
// Deliberately EXTENSIBLE and deliberately not exhaustive. Adding a kind must never renumber an
// existing one: these values are written into .hbbuild files. Append only, exactly as the mesh
// submesh rule works (MeshFaceSelect.h:10-14).
// ---------------------------------------------------------------------------

enum class ComponentKind : u16 {
    Unknown = 0,
    Building,   // the root; owns everything, generates nothing itself
    Foundation,
    Floor,      // the structural deck
    FloorSurface,
    Wall,
    Ceiling,
    Roof,
    RoofSurface,
    Column,
    Beam,
    Joist,
    Rafter,
    Truss,
    Stud,
    Plate,      // top/bottom plate of a stud wall
    Header,     // spans an opening
    Brace,
    Opening,    // the hole itself - a door/window OCCUPIES one
    Door,
    Window,
    Stair,
    Siding,
    Sheathing,
    Detail,     // non-structural dressing
    Count
};

// What a component DOES structurally. Kept separate from ComponentKind because the same kind can
// play different roles: an interior wall may or may not be load bearing, and that single fact
// changes everything about what happens when it is destroyed.
enum class StructuralRole : u8 {
    None = 0,        // decorative; carries nothing, collapses with its host
    Foundation,      // ANCHOR. load paths terminate here. see ConstructionGraph::IsAnchored
    LoadBearing,
    NonLoadBearing,
    Transfer,        // beams/headers: redistributes load sideways
    Surface,         // cladding: carried BY something, carries nothing
    Count
};

// The construction method. NOT a texture choice - each of these implies its own generation rules,
// its own cutting behaviour and its own damage vocabulary (brief SS4/SS9/SS11). Phase 1 only
// records the choice; the generators arrive in Phases 3-4.
enum class MaterialKind : u8 {
    Unknown = 0,
    Brick,
    ConcreteBlock,
    Stone,
    PouredConcrete,
    TimberFrame,
    WoodPlank,
    Plywood,
    OSB,
    WoodShingle,
    Drywall,
    Plaster,
    Metal,
    CorrugatedMetal,
    Glass,
    Mortar, // the bedding behind masonry units - its own material so it reads as mortar
    Count
};

// How one component holds another up. The kind matters because a future destruction pass treats
// them differently: severing Bears is a structural event, severing Attaches drops trim on the floor.
enum class EdgeKind : u8 {
    Bears = 0,   // supporter carries the supported component's load
    Attaches,    // fixed to, but carries no load (siding on studs)
    Contains,    // authoring containment (wall contains its studs)
    Occupies,    // a window occupies a wall opening
    Braces,      // lateral stability only
    Count
};

// How courses of masonry units are offset against each other. This is the single parameter that
// most changes how a brick wall READS, and every one of these is a real bond a bricklayer would
// name. Append only - the value is written into .hbbuild files.
enum class BondPattern : u8 {
    Running = 0, // every course offset by half a unit. the default everywhere for a reason
    Stack,       // no offset at all. modern, deliberate, and structurally weak
    Flemish,     // stretcher and header ALTERNATE within each course
    English,     // whole courses alternate: all stretchers, then all headers
    Count
};

const char* ToString(ComponentKind k);
const char* ToString(StructuralRole r);
const char* ToString(MaterialKind m);
const char* ToString(EdgeKind e);
const char* ToString(BondPattern b);

// True for materials laid as discrete UNITS with joints between them. PouredConcrete is
// deliberately NOT masonry: it has no units and no bond, and generating it as bricks would be
// exactly the "different textures pretending to be different construction" the brief rejects.
bool IsMasonry(MaterialKind m);

// Masonry construction parameters (brief SS5). Defaults are a standard metric brick:
// 215 x 102.5 x 65 mm with a 10 mm joint, which is why a course is 75 mm.
struct MasonryParams {
    f32 unitLength = 0.215f;
    f32 unitHeight = 0.065f;
    f32 unitDepth = 0.1025f;
    f32 joint = 0.010f; // mortar thickness, applied on both bed and perpend
    BondPattern bond = BondPattern::Running;

    // Per-unit variation, all 0..1 and all DETERMINISTIC from the component's seed. Real masonry
    // is never dimensionally perfect, and a wall of identical boxes reads as a texture.
    f32 sizeJitter = 0.0f;  // fraction of unit size
    f32 depthJitter = 0.0f; // in/out protrusion, fraction of unit depth
    f32 rotJitter = 0.0f;   // radians of yaw/roll wobble

    // A recessed backing slab behind the units, so the joints read as mortar rather than as holes
    // through the wall. Cheap - one box per component - and it is what stops a brick wall being
    // see-through at grazing angles.
    bool generateMortar = true;

    // SAFETY CAP, not a style choice. Unit dimensions come from a UI, and a wall authored with a
    // 1 mm unit would otherwise lay tens of millions of boxes and hang the editor with no
    // diagnostic. Generation stops here and reports rather than trying.
    u32 maxUnits = 20000;
};

// ---------------------------------------------------------------------------
// Wood construction parameters (brief SS6)
//
// Beside MasonryParams for the same reason: a component OWNS these, so the definition header has
// to be self-contained for serialization. The layout code lives in ConstructionWood.h.
// ---------------------------------------------------------------------------

// What job a generated member does. Carried on each placement so a later damage pass can treat a
// snapped stud differently from a missing siding board.
enum class MemberRole : u8 {
    Stud = 0,
    Plate,   // the horizontal member capping a stud wall, top and bottom
    Joist,   // floor / ceiling framing
    Rafter,  // roof framing
    Board,   // cladding / decking / flooring
    Batten,  // the narrow strip covering a board seam
    Shingle,
    Count
};

enum class BoardDirection : u8 { Horizontal = 0, Vertical, Diagonal, Count };

// How boards meet each other. This is what makes siding read as a specific kind of building
// rather than as generic planks.
enum class SidingProfile : u8 {
    Flush = 0,      // butted edge to edge, flat
    Clapboard,      // each course overlaps the one below and tilts out at its bottom edge
    Shiplap,        // rebated overlap, sits flat with a shadow line
    BoardAndBatten, // wide boards with a narrow batten over every seam
    Count
};

const char* ToString(MemberRole r);
const char* ToString(BoardDirection d);
const char* ToString(SidingProfile p);

// True for materials supplied as boards or sheets rather than as framing.
bool IsPlankMaterial(MaterialKind m);

// Timber framing. Defaults are a metric 2x4 stud wall at 400 mm centres with the double top plate
// that carries load from above onto the studs.
struct TimberParams {
    f32 memberWidth = 0.038f; // the narrow face, 2x4 -> 38 mm
    f32 memberDepth = 0.089f; // the deep face, 89 mm; becomes the wall thickness
    f32 spacing = 0.400f;     // ON CENTRE, as framing is always specified
    u32 topPlates = 2;        // a double top plate is standard practice, not decoration
    bool bottomPlate = true;
    f32 warp = 0.0f;       // 0..1 bow and twist; real lumber is never straight
    u32 maxMembers = 4000; // safety cap - see MasonryParams::maxUnits for why this exists
};

struct PlankParams {
    f32 boardWidth = 0.150f;
    f32 boardThickness = 0.019f;
    f32 gap = 0.003f; // between boards, for Flush and Vertical
    BoardDirection direction = BoardDirection::Horizontal;
    SidingProfile profile = SidingProfile::Flush;
    f32 overlap = 0.030f;     // Clapboard / Shiplap
    f32 battenWidth = 0.045f; // BoardAndBatten
    f32 lengthJitter = 0.0f;  // 0..1, boards are not all cut to one length
    f32 warp = 0.0f;          // 0..1 bow and cup
    u32 maxBoards = 4000;
};

// Aging and weathering (brief SS12/SS16).
//
// GEOMETRIC, NOT PAINTED. This engine has no per-unit material variation - instancing carries
// transforms only, there is no vertex colour channel and no texture arrays - so weathering here is
// what it does to the CONSTRUCTION: units go missing, edges chip away, boards sag and gap, and the
// whole surface shifts colour. That is also the honest half: a normal map cannot lose a brick, and
// losing bricks is what actually reads as age.
//
// DETERMINISTIC. Every effect draws from a stream keyed on (component id, purpose, element id), so
// the same age on the same seed always removes the same bricks - a weathered building is
// reproducible, cacheable and safe to persist damage against.
struct WeatheringParams {
    // The single master dial. 0 = as built, 1 = derelict. Everything below scales off it, so an
    // artist can get most of the way with one slider and then tune.
    f32 age = 0.0f;

    f32 moisture = 0.3f;    // rot, moss, darkening in sheltered places
    f32 exposure = 0.5f;    // sun and wind: fading, splitting, lifted edges
    f32 maintenance = 1.0f; // 1 = kept up, 0 = abandoned. DIVIDES the damage.

    // Explicit overrides. -1 means "derive from age"; anything else wins, so an artist can ask for
    // a wall that is filthy but structurally perfect.
    f32 missingChance = -1.0f;  // units simply absent
    f32 chipAmount = -1.0f;     // units eroded smaller
    f32 displacement = -1.0f;   // units pushed out of true

    // Effective 0..1 values after age, maintenance and the overrides are folded together.
    f32 EffectiveMissing() const;
    f32 EffectiveChip() const;
    f32 EffectiveDisplace() const;
    // How far the surface colour shifts: darker and desaturated with age, greener with moisture.
    f32 ColourShift() const;
};

struct ShingleParams {
    f32 width = 0.200f;
    f32 length = 0.400f;
    f32 exposure = 0.150f; // how much of each course stays visible below the one above
    f32 thickness = 0.012f;
    f32 jitter = 0.0f; // 0..1 per-shingle offset and rotation
    u32 maxShingles = 8000;
};

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

// A stable identity for one construction component.
//
// IDS ARE NEVER REUSED AND NEVER RENUMBERED. A damage record written today must still name the
// same component after the artist adds a window tomorrow, or persistent destruction, save files
// and multiplayer sync all silently corrupt. This is the same constraint the mesh layer already
// lives under - "a split NEVER reorders: the extracted group is APPENDED at the end" - and it is
// enforced here by minting from a monotonic counter that is itself serialized.
using ComponentId = u32;
inline constexpr ComponentId kInvalidComponent = 0;

struct ConstructionComponent {
    ComponentId id = kInvalidComponent;
    ComponentKind kind = ComponentKind::Unknown;
    StructuralRole role = StructuralRole::None;
    MaterialKind material = MaterialKind::Unknown;

    // Authoring containment only. NOT a support relationship - a stud is contained by its wall
    // and also BEARS the plate above it, and those are different questions with different answers.
    ComponentId parent = kInvalidComponent;

    // The parametric box every kind is defined within. Position/rotation are parent-relative,
    // matching the scene's own convention for child transforms.
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 extent{1.0f}; // half-extents, like AABB

    // THE ARTIST'S REROLL KNOB (brief SS20). Changing this reseeds THIS component and nothing
    // else - see ConstructionDef::SeedFor for why that property holds.
    u64 seedSalt = 0;

    // Construction parameters, one block per method. Only the block matching `material` is read.
    //
    // NOW THE TAGGED-UNION CASE IS REAL - there are four. They are still kept as plain members
    // rather than a variant, for one concrete reason: serialization. A .hbbuild writer over plain
    // members is a flat field list that round-trips unambiguously and can gain a field without a
    // format break; a variant needs a discriminator written, read and kept in lockstep with the
    // material enum, which is exactly the kind of two-place invariant this codebase has been bitten
    // by before (the schematic catalog/enum lockstep, the asset-format registry). The cost is
    // ~120 bytes on a component that uses one of them, on a struct there are hundreds of, not
    // millions. Revisit if a method ever needs a genuinely large parameter block.
    MasonryParams masonry;
    WeatheringParams weathering;
    TimberParams timber;
    PlankParams plank;
    ShingleParams shingle;

    // SUBTRACTIVE BRUSH. The Unreal additive/subtractive model: any part can be a cutter, not
    // just a component that happens to be kind==Opening. A subtractive part carves every additive
    // part it overlaps and draws nothing itself.
    //
    // This generalises what `Opening` used to be. Openings still work - they default to this - but
    // "make this box subtract" is now one toggle on ANY box, which is what makes boolean shaping
    // usable: you place a box where you want the hole and flip it, rather than hunting for the
    // one component kind that happens to cut.
    bool subtract = false;

    // Artist overrides (brief SS20). A locked component is never touched by regeneration, which
    // is what "generate 90% and hand-author 10%" actually requires.
    bool locked = false;
    bool hidden = false;

    std::string name; // optional, for the artist; NEVER identity - the id is identity
};

struct SupportEdge {
    ComponentId supported = kInvalidComponent;
    ComponentId supporter = kInvalidComponent;
    EdgeKind kind = EdgeKind::Bears;
    // Share of the supported component's load this edge carries, 0..1. Several edges may sum to
    // more or less than 1; the graph normalises when it evaluates rather than trusting authoring.
    f32 capacity = 1.0f;
};

// ---------------------------------------------------------------------------
// Damage state (brief SS15)
//
// SMALL, ID-KEYED, AND SERIALIZED - unlike the geometry. The whole destruction state of a
// building is a few hundred bytes because geometry is a pure function of
// (definition, seed, damage). That is what makes repair, rebuilding, persistence and network
// sync possible without shipping meshes around.
// ---------------------------------------------------------------------------

struct DamageState {
    std::vector<ComponentId> destroyed; // sorted, unique
    struct EdgeBreak {
        ComponentId supported = kInvalidComponent;
        ComponentId supporter = kInvalidComponent;
    };
    std::vector<EdgeBreak> brokenEdges;

    bool IsDestroyed(ComponentId id) const;
    bool IsEdgeBroken(ComponentId supported, ComponentId supporter) const;
    void Destroy(ComponentId id);        // idempotent, keeps `destroyed` sorted
    void BreakEdge(ComponentId supported, ComponentId supporter);
    void Clear();
    bool Empty() const { return destroyed.empty() && brokenEdges.empty(); }
};

// ---------------------------------------------------------------------------
// The definition
// ---------------------------------------------------------------------------

struct ConstructionDef {
    u64 seed = 0;             // root seed; every component's stream derives from this
    ComponentId nextId = 1;   // monotonic mint. SERIALIZED - see AddComponent.
    std::vector<ConstructionComponent> components;
    std::vector<SupportEdge> edges;

    // Mints a NEW stable id. Never reuses one, even after a delete, because a stale damage record
    // naming a recycled id would silently damage an unrelated component.
    ComponentId AddComponent(ConstructionComponent c);

    // Removes a component, every edge touching it, and reparents nothing - orphaned children are
    // reported by Validate rather than silently rehomed, because silently rehoming a stud into a
    // different wall is worse than an error.
    void RemoveComponent(ComponentId id);

    void AddEdge(ComponentId supported, ComponentId supporter, EdgeKind kind,
                 f32 capacity = 1.0f);

    const ConstructionComponent* Find(ComponentId id) const;
    ConstructionComponent* Find(ComponentId id);

    // THE DETERMINISM RULE (brief SS21).
    //
    //   seed(component) = f(rootSeed, component.id, component.seedSalt)
    //
    // and NOTHING else. Not its index, not an iteration counter, not its siblings. That is what
    // makes "adding a window to wall 3 does not perturb the brick pattern of wall 4" true - a
    // counter-based seed is the single easiest way to get this wrong, and the failure is
    // invisible until an artist notices a whole facade reshuffling when they touch one window.
    u64 SeedFor(ComponentId id) const;
    // An independent sub-stream of a component's seed, for a named purpose ("mortar", "warp").
    // Two purposes never disturb each other regardless of draw order or thread.
    u64 SeedFor(ComponentId id, u64 purpose) const;

    // Structural problems that are worth refusing to generate from. Returns false and fills
    // `outErrors` - never throws, never fixes silently.
    bool Validate(std::vector<std::string>& outErrors) const;
};

} // namespace hbe::construction
