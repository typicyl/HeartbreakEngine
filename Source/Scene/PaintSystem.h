// Scene/PaintSystem.h - Art Editor surface painting (texture painting in UV
// space, plus relief height). Operates on a PaintComponent's CPU canvas:
// raycast the mesh under the brush, stamp pigment + relief into the canvas, then
// regenerate mips and (re)upload to the bindless paint textures. Canvases persist
// to `.hbpaint` files (the scene stores only the reference + metadata).
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace hbe {

class Renderer;
struct PaintComponent;
struct MeshData;
struct TerrainComponent;

namespace paint {

// Result of a brush raycast against a mesh (mesh-local space).
struct PaintHit {
    glm::vec2 uv{0.0f};         // interpolated texture coordinate at the hit
    glm::vec3 localPos{0.0f};   // hit position in mesh-local space
    glm::vec3 localNormal{0,1,0}; // geometric face normal in mesh-local space
    f32 t = 0.0f;               // ray parameter (distance along the ray)
    f32 uvPerWorld = 1.0f;      // UV units per world unit (maps a world brush size)
};

// Builds a tapered ribbon mesh that sweeps along a drawn path (a spline brush
// stroke). `pts`/`normals` are surface points + normals (world space); `width`
// is the half-width at the fullest point. The ribbon orients to the surface,
// tapers toward the ends (brush-stroke look), and tiles its UV.u along the
// length so the stroke texture repeats. Returns an empty mesh for < 2 points.
MeshData BuildRibbon(const std::vector<glm::vec3>& pts,
                     const std::vector<glm::vec3>& normals, f32 width,
                     bool doubleSided = false);

// Variable-width ribbon: `halfWidths[i]` is the half-width at point i (drives the
// start/end taper + procedural size variation of a brush stroke). When `arcLengthUV`
// the texture U runs 0..1 over the whole stroke (so a per-stroke dynamics texture
// maps once); otherwise U tiles by width as the scalar overload does.
MeshData BuildRibbon(const std::vector<glm::vec3>& pts,
                     const std::vector<glm::vec3>& normals,
                     const std::vector<f32>& halfWidths,
                     bool doubleSided, bool arcLengthUV);

// Box-projection parameters (paint that doesn't stretch on scaled geometry).
// Built from a mesh's local AABB + the object's world scale; the projection maps
// world-scaled local positions into a 4x4-cell atlas (6 cells for the 6 face
// directions) at a uniform world-space density.
struct BoxParams {
    glm::vec3 center{0.0f}; // local AABB center
    glm::vec3 scale{1.0f};  // object world scale (per axis)
    f32 invM = 1.0f;        // 1 / max(extent * scale) - the uniform density
};
BoxParams ComputeBoxParams(const glm::vec3& localMin, const glm::vec3& localMax,
                           const glm::vec3& worldScale);
// Maps a mesh-local position + face normal to a [0,1] canvas UV (matches the
// shader's box projection in MeshPBR.hlsl exactly).
glm::vec2 BoxProjectUV(const glm::vec3& localPos, const glm::vec3& localNormal,
                       const BoxParams& b);

// Allocates / resizes the canvas for `resolution` and guarantees at least one
// layer (cleared transparent). Resets the GPU handles when the resolution
// changes. Idempotent.
void EnsureCanvas(PaintComponent& p, u32 resolution);

// Appends a blank layer (sized for the canvas) and returns its index.
int AddLayer(PaintComponent& p, const std::string& name = "Layer");

// How a brush applies paint (beyond plain pigment laydown).
enum class BrushMode : int {
    Paint = 0,      // lay pigment (straight-alpha over)
    Smudge = 1,     // pick up existing paint and drag it along the stroke
    EdgeDarken = 2, // glaze a darker tone into the stroke edges (pooling/recess)
};

// What a single brush dab lays down (albedo + PBR material + relief).
struct Dab {
    glm::vec4 color{1.0f};     // RGB albedo, A = max colour coverage
    f32  metallic = 0.0f;
    f32  roughness = 0.5f;
    f32  height = 0.0f;        // relief delta (+ raises, - carves)
    f32  flow = 0.6f;          // 0..1 build-up per dab
    bool paintColor = true;    // write the colour channels
    bool paintMaterial = true; // write the metallic/roughness/height channels
    bool erase = false;        // remove paint instead of adding
    int  mode = 0;             // BrushMode: Paint / Smudge / EdgeDarken
    f32  colorVar = 0.0f;      // 0..1 per-dab hue/value jitter (painterly pooling)
};

// Möller-Trumbore raycast of a mesh-local ray against `mesh`'s triangles;
// returns the nearest hit (both faces). False when the ray misses.
bool RaycastMesh(const MeshData& mesh, const glm::vec3& localOrigin,
                 const glm::vec3& localDir, PaintHit& out);

// TERRAIN is the one paintable surface RaycastMesh can never answer for. Chunk
// meshes are generated straight into GPU buffers and the CPU copy is dropped, so a
// chunk has no MeshRef, no cacheable source and no MeshData - which is why the brush
// used to bail out before it ever reached the code that knew about terrain.
//
// This answers the same question from the heightmap instead: march the height
// function (terrain::RaycastLocal), then report the TERRAIN-WIDE canvas UV that
// TerrainSystem bakes into every chunk vertex, so one canvas covers the whole
// terrain seamlessly and the UV density is a constant 1/extent. The ray and the
// resulting PaintHit are in TERRAIN-local space (chunks sit at identity under it).
bool RaycastTerrain(const TerrainComponent& t, const glm::vec3& localOrigin,
                    const glm::vec3& localDir, PaintHit& out);

// A custom brush definition: a handful of parameters that bake into a grayscale
// tip stamp (a real brush mark, not a plain disc). Authored in the Art Editor's
// brush editor and saved as named presets. Flat / bristle tips are directional
// (they orient along the stroke).
struct BrushDef {
    std::string name = "Brush";
    int   shape = 0;        // 0 = round, 1 = flat (ellipse)
    f32   hardness = 0.5f;  // 0 = soft feathered edge .. 1 = hard edge
    f32   grain = 0.0f;     // chalky grain (0..1)
    f32   bristles = 0.0f;  // bristle streaks (0..1)
    f32   scatter = 0.0f;   // spray/scatter holes (0..1)
    // Tool defaults applied when this preset is selected:
    f32   flow = 0.6f;      // build-up per dab
    f32   spacing = 0.12f;  // dab spacing along a stroke (fraction of radius)
    f32   size = 0.35f;     // brush radius (world units)
    f32   relief = 0.2f;    // relief height per dab
    int   mode = 0;         // BrushMode: Paint / Smudge / EdgeDarken
    f32   colorVar = 0.0f;  // per-dab hue/value jitter (painterly colour pooling)

    // --- Stroke dynamics (3D brush strokes) - Photoshop/GIMP-style brush dynamics
    // applied to a whole drag: a real start/end taper, procedural width + path
    // variation while painting, and path smoothing. Defaults give a gestural,
    // tapered mark out of the box (so strokes aren't uniform "solid" ribbons).
    f32   taperStart = 0.35f; // 0..1 length fraction tapered to a point at the start
    f32   taperEnd   = 0.35f;  // 0..1 at the end
    f32   sizeJitter = 0.25f; // 0..1 procedural width wobble along the stroke
    f32   wobble     = 0.0f;  // 0..1 path perpendicular wobble (fraction of width)
    f32   smoothing  = 0.5f;  // 0..1 path smoothing (de-jitters the drawn path)

    // Custom tip: when non-empty, the tip alpha comes from this `customSize` x
    // `customSize` grayscale stamp (imported from an image or hand-painted in the
    // pixel editor) INSTEAD of the procedural params above.
    u32   customSize = 0;
    std::vector<u8> customAlpha; // customSize^2, row-major, 0..255 coverage
    bool HasCustom() const {
        return customSize > 0 && customAlpha.size() == static_cast<usize>(customSize) * customSize;
    }
};

// A baked brush tip: a `size` x `size` grayscale coverage mask (0..1, row-major),
// plus a parallel `detail` channel (0..1, ~0.5 neutral) carrying the bristle/grain
// MICRO-RELIEF of a loaded brush - directional streaks the stamp turns into impasto
// ridges (height) + dragged value streaks (pigment) so a mark reads as real oil
// paint under PBR lighting, not a flat decal. `detail` is 0.5 (flat) for brushes
// with no bristle/grain and for custom-image tips.
struct BrushTip {
    u32 size = 0;
    std::vector<f32> alpha;
    std::vector<f32> detail;
    bool Valid() const { return size > 0 && alpha.size() == static_cast<usize>(size) * size; }
    f32 Sample(f32 u, f32 v) const;       // coverage, bilinear, u/v in [0, size-1]
    f32 SampleDetail(f32 u, f32 v) const; // micro-relief, bilinear (0.5 if absent)
};

// Procedurally bakes a brush tip from a definition (deterministic). Cheap (e.g.
// 64x64); rebuild when the definition changes.
BrushTip MakeBrushTip(const BrushDef& def, u32 size = 64);

// The built-in starter brush kit (seeds a new project's brush library).
std::vector<BrushDef> DefaultBrushes();

// --- Stroke database (the editable source of truth; baked to layer textures) --
// One point along a brush stroke's path. `uv`/`radius` drive the UV-space disc
// stamp (mesh-uv / box modes); `localPos`/`localNormal`/`localRadius` drive the
// 3D projection stamp (mode 2), which paints by surface proximity in mesh-local
// space so a stroke crosses UV-island seams and never stretches. `pressure`
// modulates flow/size. Local fields are zero for non-projection strokes.
struct StrokePoint {
    glm::vec2 uv{0.0f};
    f32 radius = 0.01f;
    f32 pressure = 1.0f;
    glm::vec3 localPos{0.0f};       // hit point in mesh-local space (projection)
    glm::vec3 localNormal{0,1,0};   // surface normal in mesh-local space (projection)
    f32 localRadius = 0.0f;         // brush radius in mesh-local units (projection)
};

// What a recorded operation does to a layer when replayed.
enum class StrokeType : int { Path = 0, Fill = 1, Clear = 2 };

// One recorded paint operation - the record the canvas is BAKED from, so the
// painted textures are always derivable from (never ahead of) the stroke history.
// A Path replays as interpolated dabs along `path`; Fill/Clear act on the whole
// layer. The BrushDef is snapshotted so editing/deleting library brushes never
// alters existing strokes.
struct Stroke {
    StrokeType type = StrokeType::Path;
    int       layer = 0;         // index of the PaintLayer this stroke belongs to
    int       projection = 0;    // 0 mesh-uv / 1 box (both UV-disc) | 2 = 3D projection
    BrushDef  brush;             // snapshot -> deterministic tip via MakeBrushTip
    glm::vec4 color{1.0f};       // RGB albedo + max coverage
    f32  metallic = 0.0f, roughness = 0.5f, height = 0.0f;
    f32  flow = 0.6f, colorVar = 0.0f;
    bool paintColor = true, paintMaterial = true, erase = false;
    std::vector<StrokePoint> path; // empty for Fill / Clear
};

// Rebakes a component's layers by clearing them and replaying every stroke in
// order, then flattening. The expensive path - call on undo/redo or a stroke
// edit, NOT per dab (live painting stamps incrementally AND records the stroke).
// `mesh` is required only to replay projection (mode-2) strokes; pass the entity's
// CPU mesh when available (UV-disc / fill / clear strokes ignore it).
void BakeFromStrokes(PaintComponent& p, const MeshData* mesh = nullptr);

// Stamps a brush `tip` of radius `uvRadius` (UV units) at `uv` into layer
// `layerIndex`, rotated by `angleRad` (the stroke direction). `dab` carries the
// albedo / material / relief laid down. Sets the component dirty.
void Stamp(PaintComponent& p, int layerIndex, const glm::vec2& uv, f32 uvRadius,
           const BrushTip& tip, f32 angleRad, const Dab& dab);

// 3D PROJECTION stamp: paints every surface texel within `localRadius` (mesh-local
// units) of `center` by rasterizing `mesh`'s triangles into UV space and testing
// each covered texel's interpolated local position against the brush sphere. Unlike
// Stamp this crosses UV-island seams and never stretches (coverage = a 3D-distance
// falloff through the brush `tip`, oriented on the surface tangent plane by
// `angleRad`). `normal` culls faces that don't roughly co-face the brush so paint
// doesn't bleed through thin geometry. `dab` carries albedo / material / relief.
void StampProjected(PaintComponent& p, int layerIndex, const MeshData& mesh,
                    const glm::vec3& center, const glm::vec3& normal, f32 localRadius,
                    const BrushTip& tip, f32 angleRad, const Dab& dab);

// The tip-orientation angle for a projection dab: the local-space stroke direction
// `dir` projected onto the surface plane of `normal`, measured in the same tangent
// frame StampProjected builds. Both the live editor and a rebake call this so a
// directional tip (flat/bristle) aligns to the stroke identically. 0 for a zero dir.
f32 SurfaceAngle(const glm::vec3& normal, const glm::vec3& dir);

// Fills layer `layerIndex` with the dab's colour/material (coverage = color.a),
// or clears it when `clear` is true. Used by the editor's Fill / Clear actions.
void FillLayer(PaintComponent& p, int layerIndex, const Dab& dab, bool clear);

// Composites the visible layers (bottom -> top, with opacity + transparency) into
// the flattened output buffers. Called by Sync.
void Flatten(PaintComponent& p);

// Reflattens and (re)uploads both flattened canvases to the GPU when dirty.
// First call creates the bindless textures; later calls update them in place.
// `dilateEdges` pads painted texels into the empty UV gutter (so bilinear + mip
// sampling don't bleed background across island seams) - skip it on the throttled
// in-stroke updates (it's an O(canvas) pass) and apply it on the release / discrete
// flush; the default is on for all the one-shot call sites (fill/layer/undo/load).
void Sync(Renderer& renderer, PaintComponent& p, bool dilateEdges = true);

// `.hbpaint` binary persistence (magic + version + resolution + both buffers).
bool Save(const std::filesystem::path& absPath, const PaintComponent& p);
// Reads through the VFS (pack-aware), like the other asset loaders. `absPath` is
// the canvas file (typically assetsDir / source).
bool Load(const std::filesystem::path& absPath, PaintComponent& p);

} // namespace paint
} // namespace hbe
