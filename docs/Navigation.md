# Navigation (Recast / Detour)

Heartbreak's navigation is a **persistent, independently-streamed tiled navmesh**. The
editor bakes world geometry into a tiled `.hbnav` with **Recast**; the runtime consumes
it with **Detour** (pathfinding) and **DetourTileCache** (dynamic obstacles / per-tile
rebuild). Recast ships **only in the editor** — the shipped game links Detour +
DetourTileCache and never carries a navmesh generator.

The defining property: **navigation is decoupled from the level's geometry streaming.**
World geometry loads and unloads around the player (tag streaming), but the navmesh lives
on disk in the `.hbnav` and is streamed by its *own* manager on its *own* radius — so AI
can path through regions whose visuals are not resident.

```
            Heartbreak World
                   │
     ┌─────────────┴──────────────┐
 World/Level data            Navigation data
 (streamed in/out)           (persistent, tiled)
                                   │
                        NavWorld (streams tile columns)
                          ┌────────┼────────┐
                       (10,12)  (11,12)  (12,12)   ← Detour dtNavMesh tiles
```

## Runtime architecture (engine-owned, Detour hidden)

Gameplay/AI never sees a `dtNavMesh*` or `dtPolyRef`. Everything goes through
`nav::NavWorld`, whose header is Detour-free (pimpl).

| Type | File | Role |
|------|------|------|
| `nav::NavWorld` | `Navigation/NavWorld.{h,cpp}` | Engine facade: loads a `.hbnav`, streams tile columns around the foci, owns obstacles, answers `FindPath`. **No Detour in the header.** |
| `nav::NavMeshManager` | `Navigation/NavMesh.{h,cpp}` | Wraps `dtNavMesh` + `dtTileCache` + `dtNavMeshQuery`. Add/remove tile columns, obstacles, queries. All Detour lives here. |
| `nav::NavTileCompressor` / allocator | `Navigation/NavTileCodec.{h,cpp}` | The one zlib `dtTileCacheCompressor` used by **both** bake and runtime, so the compressed tile blobs agree byte-for-byte. |
| `.hbnav` format | `Navigation/NavFormat.{h,cpp}` | Header / agent profiles / per-column tile directory / compressed payload. Streamable per-column without loading the whole file. |
| `nav::UpdateAgents` / `SyncObstacles` | `Navigation/NavAgents.{h,cpp}` | Fills `NavigationAgent.path` from `NavWorld::FindPath` and steers; mirrors `NavigationObstacle`s into the tile cache. |
| `nav::BakeNavMesh` / `SelfTest` | `Navigation/NavBaker.{h,cpp}` | **Editor only** (Recast). Gathers geometry → tiled Recast build → `.hbnav`. |

## `.hbnav` format

```
HBNAV
├── Header        magic/version, sourceHash, cellSize/cellHeight, tileVoxels,
│                 origin[3], grid extent, payload offset+size
├── AgentProfile* name + radius/height/climb/slope + [firstTile, tileCount) run
├── TileRecord*   per (x,y) column: coords, world AABB, layerCount,
│                 payloadOffset, payloadSize   (the streaming directory)
└── Payload       per column: [u32 len][zlib-compressed Detour tile layer]...
```

Tile blobs are read on demand (loose-file seeks for the editor / unpacked runtime; the
whole payload rides in RAM when served from a `.uap` pack, since nav data is tiny). Either
way the **live** `dtNavMesh` stays windowed — only the columns near the player are built.

The format carries **multiple agent profiles** (Human / Large / Small / Vehicle …); the
current baker emits one, and the runtime uses the first.

## Coordinate mapping

`origin` (the bake's world AABB min) is stored in the header. A world XZ maps to a tile
column by `tx = floor((wx - origin.x) / tileWorldSize)`, `tz = floor((wz - origin.z) /
tileWorldSize)`, `tileWorldSize = tileVoxels * cellSize`. `floor` handles negative
coordinates. Detour's `dtNavMesh`/`dtTileCache` are initialised with the same `origin`, so
bake and runtime agree deterministically. Very large worlds keep precision by anchoring
`origin` near the content (there is no floating-origin rebasing yet — see limitations).

## Streaming behaviour

`NavWorld::Update(foci, dt)` runs every frame (foci = player + camera, the same source as
geometry streaming):

1. **Finalize** ready tile columns (budgeted per frame) into the `dtNavMesh`.
2. **Unload** resident columns beyond `unloadRadius` of every focus (hysteresis).
3. **Kick** async reads for desired-but-absent columns within `loadRadius` (job system).
4. **Reconcile** obstacles + run the tile-cache rebuild.

`loadRadius` defaults **wider** than geometry streaming so AI sees navigation ahead of the
visuals. Tile reads run on `jobs::RunDetached`; Detour mutation stays on the main thread.

A path request that needs a non-resident tile returns `missing = true`, **requests** those
tiles, and the agent keeps its current path and retries — it never blocks on streaming.

## Dynamic obstacles

`NavigationObstacle` (crates, vehicles, barricades, doors, destructibles) → a
`dtTileCache` cylinder obstacle. `tileCache->update()` rebuilds only the affected tiles
(Detour bounds this to 64 requests/rebuilds per call), so a mover reroutes AI in real time
with **no re-bake**. Movement is remove+add (tile-cache obstacles are static). Static world
geometry stays baked.

## Editor workflow

Navigation panel → set bake params (cell size/height, tile voxels, agent radius/height/
climb/slope) → **Bake Navmesh** (writes `Assets/Nav/….hbnav`, sets
`SceneEnvironment::navSource`, stamps a pack slot) → **Save the scene** (Ctrl+S) so the
reference persists. *Show navmesh* overlays the resident tiles; *Test Path* runs a live
`FindPath`. Exclude a mesh from baking with a disabled **Navmesh Input**; make a mover a
**Navigation Obstacle** instead of baking it.

## Runtime workflow

On level load, `SceneEnvironment::navSource` is applied; the Engine's sim band notices the
change and calls `NavWorld::Load`. Thereafter tiles stream around the player; agents path
and obstacles rebuild tiles. The shipped `.hbnav` is packed like any other engine asset
(`AssetFormats` → `runtimeLoaded`, read via the VFS).

## Runtime vs editor dependency boundary

- **Runtime (`hbe`)** links `Detour` + `DetourTileCache` only. Consumes baked tiles.
- **Editor (`hbe_editor`)** additionally links `Recast` (one TU: `NavBaker.cpp`).
- Verified: `DetourTileCache` depends only on `Detour`, so no Recast reaches the runtime.

## Tests

`HeartbreakEditor --test-nav` (alias `--navtest`): headless. Recast-bakes synthetic
geometry (floor + wall) into a `.hbnav`, streams the tiles through Detour, paths around the
baked wall, then adds and removes a `dtTileCache` obstacle and asserts the path reroutes
and recovers. Covers bake + `.hbnav` round-trip + streaming + Detour query + dynamic
obstacles.

## Known limitations

- **One agent profile** is baked/streamed at runtime today (the format supports N).
- **No off-mesh connections** yet (jumps / ladders); the format reserves space.
- **No detail mesh** — the tile-cache path trades it for dynamic-obstacle support (a
  standard Recast tradeoff), so agent Y comes from the navmesh polygon plane.
- **No floating-origin rebasing** — precision relies on `origin` anchored near content.
- **Obstacle-under-stream-in**: an obstacle added while its tile is not resident is
  re-applied when the tile streams in (overlap reconciliation), a bounded extra rebuild.

## License

Recast/Detour is used under the **zlib license** (Copyright © 2009 Mikko Mononen),
unmodified via FetchContent. See `docs/ThirdParty.md` and `cmake/Dependencies.cmake`.
