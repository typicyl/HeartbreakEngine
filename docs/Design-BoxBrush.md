# CSG Blockout Box Brush

Unreal-style **box brushes**: editable box *primitives* that become real level geometry, with
**Additive** brushes adding solid and **Subtractive** brushes boolean-carving doorways / windows /
room interiors out of them. This is geometry authoring (blockout), **not** a decal or a material
projection — the result is rendered, collided against, and baked into the navmesh like any mesh.

> This is distinct from the **Material Volume** tool (`MaterialVolumeComponent`), which *projects a
> material* onto existing meshes. That one is a surface decal; this one is geometry.

## Architecture

The system mirrors **terrain** exactly: an editable source regenerates derived geometry on a dirty
flag. Nothing derived is serialized.

```
BrushComponent (editable source: halfExtents, op Add/Subtract, uvScale)
      │  brush::Update  (Engine::Update, every frame, no-op when clean)
      ▼
Additive brush entity gets:  MeshInstance(mesh) + AABB + RigidBody(Mesh,Static)
      │
      └── mesh = csg::CarveBox(this box, every subtractive box in the scene)
```

| File | Role |
|------|------|
| `Source/Assets/MeshCsg.{h,cpp}` | BSP CSG solver (`csg::CarveBox`, `csg::PointSolid`). Backend-agnostic, headless. |
| `Source/Assets/MeshCsgTest.cpp` | `--test-csg`: signed-volume correctness (10 cases incl. coplanar/full-carve). |
| `Source/Scene/Components.h` | `BrushComponent` (the editable source; plain data). |
| `Source/Scene/BrushSystem.{h,cpp}` | `brush::BuildEntityMesh` / `Update` / `MarkAllDirty` / `AllClean`. |
| `Source/Engine/Engine.cpp` | calls `brush::Update(scene, renderer)` beside `terrain::Update`. |
| `Source/Navigation/NavBaker.cpp` | additive-brush pass in `GatherGeometry` (brushes have no MeshRef). |
| `Source/Editor/Editor.cpp` | create menu, drag-out tool, inspector, wireframe, gizmo-live-rebuild. |
| `Source/Scene/SceneSerializer.{h,cpp}`, `TagShard.cpp` | `BrushComponent` persistence (6 sites + collab + streaming bounds). |

## The CSG solver (`csg::CarveBox`)

Classic **BSP constructive solid geometry** (Naylor–Thibault; the csg.js formulation). Each box → 6
CCW-outward planar polygons; `A − B` is computed by clipping polygon sets against each other's BSP
trees and re-triangulating. Boxes are convex, so trees stay tiny and the result is robust. Output is
flat-shaded (per-face normals) with **world-planar tiling UVs** (`uvScale` = metres per tile) so a
grid/checker blockout material tiles uniformly across the whole shell. Deterministic.

`CarveBox(body, cutters)` returns `body` minus each cutter, all in one common space.

## Data flow / lifecycle

- **BuildEntityMesh(scene, e)** — a pure function of scene state: the additive brush's box (in its
  own local frame) minus every subtractive brush transformed into that frame (`invWorldA · worldB`).
  Shared by the geometry rebuild **and** the navmesh baker.
- **brush::Update** — when *any* `BrushComponent.dirty` is set, rebuilds every additive brush:
  `DestroyMesh(old)` → `UploadMesh(new)` → reassign `MeshInstance.mesh`, refresh `AABB`, refill the
  static-mesh `RigidBody` (`collisionVertices/Indices`, `bodyId = kInvalidBody` to trigger the Jolt
  rebuild). Subtractive brushes carry no drawable/collider (carve tools) — they keep a box `AABB` for
  picking + a wireframe. Global invalidation is intentional: a subtractive edit reaches any additive
  it overlaps (blockout meshes are tiny, so re-carving all is cheap).
- **Collision** — exact triangle `JPH::MeshShape`, static. Walls/floors are solid immediately.
- **Nav** — additive brushes are gathered as static nav geometry (they have no `MeshRef`, so
  `GatherGeometry` has a dedicated brush pass); a manual *Bake Navmesh* picks them up.
- **Serialization** — only `BrushComponent` (halfExtents / op / uvScale) is written; geometry,
  collision, and AABB are re-derived on load (`dirty` defaults true, like `TerrainComponent`).

## Editor workflow

- **Create ▸ Box Brush (Additive / Subtractive)** — drops a default box; geometry appears next frame.
- **Create ▸ Box Brush Drag Tool** — toggle it, then click-drag on the ground plane to rubber-band a
  footprint; release drops a box of that size at the tool's default height (Additive or Subtractive).
- **Gizmo** — move / rotate / scale like any entity; geometry re-carves **live** while dragging.
- **Inspector ▸ Box Brush** — type (Additive/Subtractive), half extents, texture tile size, and
  (additive) colour + roughness. Any geometric edit re-carves.
- **Wireframe** — the selected brush's box draws green (additive) or orange (subtractive).

## Performance

- CSG is convex box-on-box: shallow BSP trees, sub-millisecond per brush for typical blockout.
- Rebuild is **dirty-gated** — `brush::Update` early-outs when clean, so idle cost is one view scan.
- A whole-scene rebuild on any edit is `O(additive × subtractive)`; fine at blockout scale (tens of
  brushes). If a level ever grows to hundreds of brushes, scope the rebuild to brushes whose AABB
  overlaps a dirty brush (a localised optimisation, not needed today).
- Static mesh collision + nav are baked from the same carved mesh — no extra geometry.

## Limitations / future

- Adjacent additive brushes are **not** coplanar-merged (Unreal BSP merges faces); overlapping
  additives keep their own shells. Invisible from outside; fine for blockout.
- Shapes are boxes only. The `csg::` layer is shape-agnostic (planar polygons), so wedges / cylinders
  are a straightforward extension if needed.
- Painting / material-volume tools target `MeshRef` meshes; brushes have none, so they take a solid
  colour, not painted detail (by design — blockout uses grid textures).

## Tests

- `--test-csg` — BSP correctness by closed-mesh signed volume vs the analytic answer: plain box,
  interior void, doorway-through, rotated cutter, flush/coplanar (no removal), no-overlap, full
  carve, corner carve, two doorways, determinism.
- `--test-brush` — `BuildEntityMesh` additive/subtractive routing, world-frame carve volumes, and
  `BrushComponent` scene save round-trip. Headless (no GPU).
