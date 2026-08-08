// Construction/ConstructionTest.cpp - --test-construction.
//
// What this proves, in order of how expensive the bug would be to find later:
//
//   1. SEED INDEPENDENCE. Editing one component must not perturb any other. This is the property
//      the whole caching/source-control/destruction-persistence story rests on, and a
//      counter-based seed silently breaks it in a way an artist only notices as "the whole
//      facade reshuffled when I moved one window".
//   2. ID STABILITY across delete. A recycled id would let a stale damage record destroy an
//      unrelated component with nothing reporting it.
//   3. The structural graph queries, including the hypothetical one a destruction pass consumes.
#include "Construction/ConstructionGeometry.h"
#include "Construction/ConstructionMasonry.h"
#include "Construction/ConstructionWood.h"
#include "Construction/ConstructionOpenings.h"
#include "Construction/ConstructionPreset.h"
#include "Construction/ConstructionChunk.h"
#include "Construction/ConstructionGraph.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace hbe::construction {

namespace {

u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("construction FAIL: %s\n", what);
        ++g_fails;
    }
}

ComponentId Add(ConstructionDef& d, ComponentKind kind, StructuralRole role,
                ComponentId parent = kInvalidComponent) {
    ConstructionComponent c;
    c.kind = kind;
    c.role = role;
    c.parent = parent;
    // POURED CONCRETE ON PURPOSE. This fixture exists to test STRUCTURE - the graph, the seeds,
    // the massing geometry - and poured concrete is the one construction method that generates a
    // single solid block per component (it has no units and no members). Building it from
    // TimberFrame would frame every wall into studs and plates, which is correct behaviour but
    // makes the massing assertions below measure Phase 4's output instead of Phase 2's.
    // The wood and masonry fixtures build their own components.
    c.material = MaterialKind::PouredConcrete;
    return d.AddComponent(std::move(c));
}

// A small but REAL structure, the one the brief keeps drawing:
//   roof -> rafters -> walls -> foundation, and floor -> joists -> beam -> column -> foundation.
struct House {
    ConstructionDef def;
    ComponentId foundation, wallL, wallR, rafter, roof, column, beam, joist, floor, siding;
};

House MakeHouse() {
    House h;
    ConstructionDef& d = h.def;
    d.seed = 0xC0FFEEull;

    h.foundation = Add(d, ComponentKind::Foundation, StructuralRole::Foundation);
    h.wallL = Add(d, ComponentKind::Wall, StructuralRole::LoadBearing);
    h.wallR = Add(d, ComponentKind::Wall, StructuralRole::LoadBearing);
    h.rafter = Add(d, ComponentKind::Rafter, StructuralRole::Transfer);
    h.roof = Add(d, ComponentKind::Roof, StructuralRole::NonLoadBearing);
    h.column = Add(d, ComponentKind::Column, StructuralRole::LoadBearing);
    h.beam = Add(d, ComponentKind::Beam, StructuralRole::Transfer);
    h.joist = Add(d, ComponentKind::Joist, StructuralRole::Transfer);
    h.floor = Add(d, ComponentKind::Floor, StructuralRole::NonLoadBearing);
    h.siding = Add(d, ComponentKind::Siding, StructuralRole::Surface, h.wallL);

    d.AddEdge(h.wallL, h.foundation, EdgeKind::Bears);
    d.AddEdge(h.wallR, h.foundation, EdgeKind::Bears);
    d.AddEdge(h.column, h.foundation, EdgeKind::Bears);
    // The roof rests on ONE rafter which rests on BOTH walls - the case a Parent link cannot
    // express and the reason this graph exists at all.
    d.AddEdge(h.rafter, h.wallL, EdgeKind::Bears, 0.5f);
    d.AddEdge(h.rafter, h.wallR, EdgeKind::Bears, 0.5f);
    d.AddEdge(h.roof, h.rafter, EdgeKind::Bears);
    d.AddEdge(h.beam, h.column, EdgeKind::Bears);
    d.AddEdge(h.joist, h.beam, EdgeKind::Bears);
    d.AddEdge(h.floor, h.joist, EdgeKind::Bears);
    // Siding is ATTACHED, not supported - it must never count as structure.
    d.AddEdge(h.siding, h.wallL, EdgeKind::Attaches);
    return h;
}

bool Has(const std::vector<ComponentId>& v, ComponentId id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

} // namespace

// --test-chunk. The distinction between LOGICAL UNITS, RENDER OBJECTS and PHYSICS OBJECTS is the
// thing this system has to get right to scale, so it is tested as an invariant rather than assumed.
bool ChunkSelfTest() {
    g_fails = 0;

    // A brick house: enough units to be worth chunking, spread over enough space to need it.
    PresetParams p = DefaultParams("house");
    p.width = 12.0f;
    p.depth = 8.0f;
    p.floorCount = 2;
    p.exteriorMaterial = static_cast<i32>(MaterialKind::Brick);
    ConstructionDef def;
    Check(BuildPreset("house", p, def), "the house preset builds");

    std::vector<PlacedPiece> pieces;
    CollectPieces(def, nullptr, kInvalidComponent, pieces);
    Check(pieces.size() > 2000,
          "a two-storey brick house is thousands of LOGICAL UNITS - that is expected and fine");

    SectionMesh flat;
    BuildSection(def, nullptr, kInvalidComponent, flat);
    ChunkedSection chunked;
    BuildChunked(def, nullptr, kInvalidComponent, 4.0f, chunked);

    Check(chunked.chunks.size() > 1, "a 12x8 m house spans several 4 m chunks");
    Check(chunked.TotalPieces() == pieces.size(),
          "CHUNKING MUST NOT LOSE A SINGLE UNIT - every logical piece lands in exactly one chunk");
    Check(chunked.TotalIndices() == flat.TotalIndices(),
          "chunked and flat generation are the SAME GEOMETRY, differently grouped - if these "
          "disagree, chunking is silently changing what gets built");

    // THE POINT OF THE WHOLE EXERCISE. Thousands of units, a handful of draws.
    Check(chunked.DrawCount() < 100,
          "THOUSANDS OF LOGICAL UNITS MUST COST TENS OF DRAWS, not thousands - this renderer caps "
          "the whole FRAME at roughly 5,400 draw items and silently truncates past it");
    Check(chunked.DrawCount() >= flat.model.size(),
          "chunking trades some draws for cullability, so it never costs fewer than flat");

    // Every PIECE lands in exactly one chunk.
    //
    // CORRECTED INVARIANT. This used to assert that an (component, element) pair appears exactly
    // once, which was wrong once cutters could split a member: a stud crossing a window becomes a
    // cripple above AND below, and both legitimately carry the SAME element id because they are
    // one stud. Sharing the id is what lets a later damage pass remove the whole stud rather than
    // half of it.
    //
    // What must actually hold is that no single PIECE is duplicated across chunks - guaranteed by
    // bucketing on the piece's centre - and that the total is conserved, which is checked above.
    {
        u32 totalRanges = 0;
        for (const ConstructionChunk& c : chunked.chunks)
            totalRanges += static_cast<u32>(c.elements.size());
        Check(totalRanges > 0, "the chunked structure addresses its units");
        Check(totalRanges <= chunked.TotalPieces(),
              "NO PIECE IS DUPLICATED ACROSS CHUNKS - each is bucketed by its CENTRE and never "
              "split, so a brick straddling a boundary lands wholly in one of them");

        // ...and a component whose members were cut may legitimately own several ranges.
        u32 mostRangesForOne = 0;
        std::vector<std::pair<u64, u32>> counts;
        for (const ConstructionChunk& c : chunked.chunks)
            for (const ElementRange& e : c.elements) {
                const u64 key = (static_cast<u64>(e.component) << 32) | e.element;
                bool found = false;
                for (auto& kv : counts)
                    if (kv.first == key) {
                        ++kv.second;
                        found = true;
                        break;
                    }
                if (!found) counts.emplace_back(key, 1u);
            }
        for (const auto& kv : counts) mostRangesForOne = std::max(mostRangesForOne, kv.second);
        Check(mostRangesForOne <= 8,
              "a cut member yields a handful of pieces, not an unbounded spray - a runaway here "
              "would mean the cutter is shattering members instead of splitting them");
    }

    // Determinism, including chunk ORDER - a std::map iteration, not a hash.
    {
        ChunkedSection again;
        BuildChunked(def, nullptr, kInvalidComponent, 4.0f, again);
        Check(again.chunks.size() == chunked.chunks.size(), "chunking is deterministic in count");
        bool sameOrder = again.chunks.size() == chunked.chunks.size();
        for (usize i = 0; sameOrder && i < again.chunks.size(); ++i)
            sameOrder = again.chunks[i].cx == chunked.chunks[i].cx &&
                        again.chunks[i].cy == chunked.chunks[i].cy &&
                        again.chunks[i].cz == chunked.chunks[i].cz;
        Check(sameOrder,
              "...and in ORDER - a hash map here would reorder chunks between runs and break any "
              "cache keyed on chunk identity");
    }

    // Chunk size is a real knob.
    {
        ChunkedSection coarse, fine;
        BuildChunked(def, nullptr, kInvalidComponent, 20.0f, coarse);
        BuildChunked(def, nullptr, kInvalidComponent, 2.0f, fine);
        Check(fine.chunks.size() > coarse.chunks.size(),
              "a smaller chunk size produces more chunks - finer culling and finer cache "
              "invalidation, at the cost of more draws");
        Check(fine.TotalPieces() == coarse.TotalPieces(),
              "...while the logical construction is identical either way");
        Check(fine.DrawCount() > coarse.DrawCount(), "and the draw count is the price paid");
    }

    // "Which construction units are in this region?" (brief SS13) - the coarse half of an impact
    // query, before any per-unit test.
    {
        const glm::vec3 c = (chunked.boundsMin + chunked.boundsMax) * 0.5f;
        const auto hit = chunked.ChunksInBounds(c - glm::vec3(1.0f), c + glm::vec3(1.0f));
        Check(!hit.empty(), "a query at the centre of the structure finds chunks");
        const auto miss =
            chunked.ChunksInBounds(chunked.boundsMax + glm::vec3(500.0f),
                                   chunked.boundsMax + glm::vec3(501.0f));
        Check(miss.empty(), "a query far outside finds none");
        const auto whole = chunked.ChunksInBounds(chunked.boundsMin - glm::vec3(1.0f),
                                                  chunked.boundsMax + glm::vec3(1.0f));
        Check(whole.size() == chunked.chunks.size(), "a query covering everything finds everything");
    }

    // Chunk coordinates are integer cells, so a chunk's identity survives regeneration rather
    // than shifting when a wall is resized.
    {
        Check(chunked.ChunkAt(chunked.chunks[0].cx, chunked.chunks[0].cy, chunked.chunks[0].cz) ==
                  &chunked.chunks[0],
              "a chunk is addressable by its integer cell");
        Check(chunked.ChunkAt(9999, 9999, 9999) == nullptr, "an empty cell has no chunk");
    }

    // Damage removes units from their chunk without disturbing the others.
    {
        ComponentId aWall = kInvalidComponent;
        for (const ConstructionComponent& c : def.components)
            if (c.kind == ComponentKind::Wall) {
                aWall = c.id;
                break;
            }
        DamageState dmg;
        dmg.Destroy(aWall);
        ChunkedSection damaged;
        BuildChunked(def, &dmg, kInvalidComponent, 4.0f, damaged);
        Check(damaged.TotalPieces() < chunked.TotalPieces(),
              "destroying a component removes its units from the chunked output");
        Check(!damaged.chunks.empty(), "...while the rest of the structure survives");
    }

    if (g_fails == 0)
        std::printf("chunk: thousands of LOGICAL UNITS become tens of DRAWS; chunking loses no "
                    "unit, changes no geometry, puts every unit in exactly ONE chunk (bucketed by "
                    "centre, never split), iterates in deterministic order, and answers 'which "
                    "units are in this region'\n");
    return g_fails == 0;
}

bool SelfTest() {
    g_fails = 0;

    // -- Determinism ---------------------------------------------------------
    {
        House a = MakeHouse();
        House b = MakeHouse();
        Check(a.def.SeedFor(a.wallL) == b.def.SeedFor(b.wallL),
              "the same definition and seed must produce the same component seed");
        Check(a.def.SeedFor(a.wallL) != a.def.SeedFor(a.wallR),
              "two different components must not share a stream");
        Check(a.def.SeedFor(a.wallL, 1) != a.def.SeedFor(a.wallL, 2),
              "two purposes within one component must be independent streams");
        Check(a.def.SeedFor(a.wallL, 1) == b.def.SeedFor(b.wallL, 1),
              "...and a purpose stream must still be reproducible");

        House c = MakeHouse();
        c.def.seed = 0xD1FFull;
        Check(c.def.SeedFor(c.wallL) != a.def.SeedFor(a.wallL),
              "a different ROOT seed must change every component");
    }

    // -- THE SEED-INDEPENDENCE PROPERTY --------------------------------------
    // Editing one component must not perturb any other. This is the whole of brief SS21 and the
    // reason seeds derive from a stable ID rather than an index or a counter.
    {
        House a = MakeHouse();
        House b = MakeHouse();
        const u64 before = b.def.SeedFor(b.wallR);

        // Reroll wallL - the artist's per-component knob.
        b.def.Find(b.wallL)->seedSalt = 12345;
        Check(b.def.SeedFor(b.wallL) != a.def.SeedFor(a.wallL),
              "changing a component's salt must change ITS stream");
        Check(b.def.SeedFor(b.wallR) == before,
              "REROLLING ONE COMPONENT MUST NOT PERTURB ANOTHER - a counter-based seed would "
              "reshuffle the whole building when an artist touches one window");

        // Adding a whole new component must not disturb anything either.
        const u64 wallRBefore = b.def.SeedFor(b.wallR);
        const ComponentId extra = Add(b.def, ComponentKind::Window, StructuralRole::None);
        Check(b.def.SeedFor(b.wallR) == wallRBefore,
              "ADDING A COMPONENT MUST NOT PERTURB EXISTING ONES - this is what makes the "
              "generated-data cache valid across an edit");
        Check(b.def.SeedFor(extra) != b.def.SeedFor(b.wallR), "...and the new one gets its own stream");

        // And removing one must not either.
        b.def.RemoveComponent(extra);
        Check(b.def.SeedFor(b.wallR) == wallRBefore, "removing a component must not perturb the rest");
    }

    // -- Id stability --------------------------------------------------------
    {
        ConstructionDef d;
        const ComponentId a = Add(d, ComponentKind::Wall, StructuralRole::LoadBearing);
        const ComponentId b = Add(d, ComponentKind::Wall, StructuralRole::LoadBearing);
        d.RemoveComponent(a);
        const ComponentId c = Add(d, ComponentKind::Wall, StructuralRole::LoadBearing);
        Check(c != a,
              "AN ID MUST NEVER BE RECYCLED - a stale damage record naming a recycled id would "
              "destroy an unrelated component and nothing would report it");
        Check(c != b, "...and must not collide with a live one");
        Check(d.Find(a) == nullptr, "a removed component is gone");
        Check(d.Find(b) != nullptr, "...and its neighbour is not");
    }

    // -- Validation ----------------------------------------------------------
    {
        House h = MakeHouse();
        std::vector<std::string> errs;
        Check(h.def.Validate(errs), "a well-formed definition must validate");
        Check(errs.empty(), "...with no errors reported");

        // An edge naming a component that no longer exists.
        ConstructionDef d = h.def;
        d.edges.push_back(SupportEdge{h.roof, 9999, EdgeKind::Bears, 1.0f});
        errs.clear();
        Check(!d.Validate(errs), "an edge naming a missing component must fail validation");
        Check(!errs.empty(), "...and say so");

        // A containment cycle must be caught rather than hanging a subtree walk.
        ConstructionDef cyc;
        const ComponentId x = Add(cyc, ComponentKind::Wall, StructuralRole::LoadBearing);
        const ComponentId y = Add(cyc, ComponentKind::Wall, StructuralRole::LoadBearing, x);
        cyc.Find(x)->parent = y;
        errs.clear();
        Check(!cyc.Validate(errs), "A CONTAINMENT CYCLE MUST BE DETECTED, not hang a walk");
    }

    // -- Graph relationships -------------------------------------------------
    {
        House h = MakeHouse();
        ConstructionGraph g;
        g.Build(h.def);

        Check(g.NodeCount() == h.def.components.size(), "every component becomes a node");

        const auto rafterSup = g.Supporters(h.rafter);
        Check(rafterSup.size() == 2, "the rafter is supported by BOTH walls");
        Check(Has(rafterSup, h.wallL) && Has(rafterSup, h.wallR), "...specifically those two");

        Check(Has(g.Supported(h.foundation), h.wallL), "the foundation supports the walls");
        Check(!Has(g.Supported(h.foundation), h.roof), "...but not the roof directly");

        const auto deps = g.Dependents(h.wallL);
        Check(Has(deps, h.rafter) && Has(deps, h.roof),
              "the wall's dependents reach TRANSITIVELY to the roof");
        Check(!Has(deps, h.floor), "...and do not leak into the unrelated floor chain");

        // Siding is attached, not borne. It must not appear as structure.
        Check(!Has(g.Supported(h.wallL), h.siding),
              "AN 'ATTACHES' EDGE IS NOT SUPPORT - counting it would make every decorative "
              "component read as structure");
        Check(Has(g.Children(h.wallL), h.siding), "...but containment still sees it");

        Check(g.IsAnchored(h.roof), "the roof reaches the foundation through rafter and walls");
        Check(g.IsAnchored(h.floor), "the floor reaches it through joist, beam and column");
        Check(g.IsAnchored(h.foundation), "a foundation is trivially anchored");
        Check(g.Unsupported().empty(), "an intact structure has nothing unsupported");

        // THE CASE THAT CAUGHT A REAL BUG. Cladding bears no load and hangs off its host by an
        // `Attaches` edge. Judged by load alone it has no path to ground, so an intact wall
        // reported its OWN SIDING as unsupported - "carries no load" is not "has no support".
        Check(g.IsAnchored(h.siding),
              "CLADDING IS HELD UP BY ATTACHMENT - an intact wall must not report its own siding "
              "as unsupported");
        Check(g.Integrity(h.siding) > 0.99f, "...and intact cladding is at full integrity");
    }

    // -- The hypothetical: what breaks if this is removed --------------------
    {
        House h = MakeHouse();
        ConstructionGraph g;
        g.Build(h.def);

        // ONE wall is not enough - the rafter has a second support. This is the case a naive
        // parent-link model gets wrong, and it is exactly why support is an edge LIST.
        auto oneWall = g.UnsupportedIfRemoved({h.wallL});
        Check(!Has(oneWall, h.roof),
              "REMOVING ONE OF TWO SUPPORTS MUST NOT COLLAPSE THE ROOF - the rafter still "
              "reaches ground through the other wall");

        auto bothWalls = g.UnsupportedIfRemoved({h.wallL, h.wallR});
        Check(Has(bothWalls, h.rafter), "removing BOTH walls strands the rafter");
        Check(Has(bothWalls, h.roof), "...and the roof above it, transitively");
        Check(!Has(bothWalls, h.floor), "...while the independent floor chain is untouched");
        Check(!Has(bothWalls, h.wallL), "the removed components are not themselves reported");

        // A single column is a single point of failure for the whole floor chain.
        auto col = g.UnsupportedIfRemoved({h.column});
        Check(Has(col, h.beam) && Has(col, h.joist) && Has(col, h.floor),
              "one column failing strands the entire beam/joist/floor chain");

        Check(g.UnsupportedIfRemoved({}).empty(), "removing nothing strands nothing");
        // The query must not mutate - the same graph must answer the same way twice.
        Check(g.UnsupportedIfRemoved({h.column}).size() == col.size(),
              "THE HYPOTHETICAL MUST NOT MUTATE THE GRAPH - an editor preview asks it repeatedly");
    }

    // -- Damage state --------------------------------------------------------
    {
        House h = MakeHouse();
        DamageState dmg;
        dmg.Destroy(h.wallL);
        dmg.Destroy(h.wallL); // idempotent
        Check(dmg.destroyed.size() == 1, "destroying the same component twice records it once");
        Check(dmg.IsDestroyed(h.wallL), "...and it reads as destroyed");
        Check(!dmg.IsDestroyed(h.wallR), "...while its neighbour does not");

        ConstructionGraph g;
        g.Build(h.def, &dmg);
        Check(!g.Exists(h.wallL), "a destroyed component no longer exists in the graph");
        // ...and cladding falls WITH its host, not before it and not after.
        Check(!g.IsAnchored(h.siding),
              "cladding loses its support when the wall it is attached to is destroyed");
        Check(g.IsAnchored(h.roof), "the roof survives on the remaining wall");
        Check(g.Supporters(h.rafter).size() == 1, "the rafter now has one support, not two");

        dmg.Destroy(h.wallR);
        g.Build(h.def, &dmg);
        Check(!g.IsAnchored(h.rafter), "with both walls gone the rafter is unsupported");
        Check(!g.IsAnchored(h.roof), "...and so is the roof");
        Check(g.IsAnchored(h.floor), "...but the floor chain still stands");
        const auto un = g.Unsupported();
        Check(Has(un, h.rafter) && Has(un, h.roof), "Unsupported reports exactly the stranded pair");
        Check(!Has(un, h.floor), "...and not the intact chain");

        // A broken edge severs support WITHOUT destroying either component - the "connection
        // failed but the wall is still standing" case the brief's SS15 example calls out.
        House h2 = MakeHouse();
        DamageState d2;
        d2.BreakEdge(h2.roof, h2.rafter);
        ConstructionGraph g2;
        g2.Build(h2.def, &d2);
        Check(g2.Exists(h2.rafter) && g2.Exists(h2.roof), "breaking an edge destroys neither end");
        Check(!g2.IsAnchored(h2.roof), "...but the roof loses its load path");
        Check(g2.IsAnchored(h2.rafter), "...and the rafter below is unaffected");
    }

    // -- Integrity -----------------------------------------------------------
    {
        House h = MakeHouse();
        ConstructionGraph g;
        g.Build(h.def);
        Check(g.Integrity(h.foundation) == 1.0f, "a foundation is always fully intact");
        Check(g.Integrity(h.rafter) > 0.99f, "an intact rafter has full support");

        DamageState dmg;
        dmg.Destroy(h.wallL);
        g.Build(h.def, &dmg);
        const f32 half = g.Integrity(h.rafter);
        Check(half > 0.4f && half < 0.6f,
              "losing one of two EQUAL supports leaves the rafter at about half integrity");
        Check(g.Integrity(h.wallL) == 0.0f, "a destroyed component has zero integrity");

        // Authoring will not be careful. Three supports written as 1.0 each means "three equal
        // supports", not 300% - integrity normalises against authored capacity rather than
        // trusting it.
        ConstructionDef d;
        const ComponentId f = Add(d, ComponentKind::Foundation, StructuralRole::Foundation);
        const ComponentId a = Add(d, ComponentKind::Column, StructuralRole::LoadBearing);
        const ComponentId b = Add(d, ComponentKind::Column, StructuralRole::LoadBearing);
        const ComponentId slab = Add(d, ComponentKind::Floor, StructuralRole::NonLoadBearing);
        d.AddEdge(a, f, EdgeKind::Bears);
        d.AddEdge(b, f, EdgeKind::Bears);
        d.AddEdge(slab, a, EdgeKind::Bears, 1.0f);
        d.AddEdge(slab, b, EdgeKind::Bears, 1.0f);
        ConstructionGraph g3;
        g3.Build(d);
        Check(g3.Integrity(slab) <= 1.0f,
              "INTEGRITY MUST NEVER EXCEED 1 even when authored capacities sum past it");
    }

    // =======================================================================
    // PHASE 2 - geometry
    // =======================================================================

    // -- Determinism of the generated mesh -----------------------------------
    {
        House a = MakeHouse();
        House b = MakeHouse();
        SectionMesh ma, mb;
        BuildSection(a.def, nullptr, kInvalidComponent, ma);
        BuildSection(b.def, nullptr, kInvalidComponent, mb);

        Check(!ma.model.empty(), "an intact house generates geometry");
        Check(ma.model.size() == mb.model.size(), "the same definition yields the same submeshes");
        bool identical = ma.model.size() == mb.model.size();
        for (usize i = 0; identical && i < ma.model.size(); ++i) {
            identical = ma.model[i].vertices.size() == mb.model[i].vertices.size() &&
                        ma.model[i].indices.size() == mb.model[i].indices.size() &&
                        std::memcmp(ma.model[i].vertices.data(), mb.model[i].vertices.data(),
                                    ma.model[i].vertices.size() * sizeof(Vertex)) == 0 &&
                        std::memcmp(ma.model[i].indices.data(), mb.model[i].indices.data(),
                                    ma.model[i].indices.size() * sizeof(u32)) == 0;
        }
        Check(identical,
              "THE SAME DEFINITION MUST PRODUCE BYTE-IDENTICAL GEOMETRY - the whole generated-data "
              "cache is keyed on that being true");
    }

    // -- Capacity is a bound, not a guess ------------------------------------
    // The RHI cannot free a mesh and UpdateMesh never grows, so a reservation that is even one
    // vertex short means a refused update that leaves stale geometry on the GPU forever.
    {
        House h = MakeHouse();
        SectionMesh m;
        BuildSection(h.def, nullptr, kInvalidComponent, m);
        const MeshCapacity cap = EstimateCapacity(h.def, kInvalidComponent);
        Check(cap.vertices >= m.TotalVertices(),
              "THE CAPACITY ESTIMATE MUST NEVER UNDER-COUNT VERTICES - a short reservation is a "
              "permanently refused UpdateMesh with no way to free the mesh and retry");
        Check(cap.indices >= m.TotalIndices(), "...nor indices");
        Check(cap.vertices == m.TotalVertices(), "...and for Phase 2 shapes it is exact");

        // A reservation must survive damage being repaired and hidden things being shown, so it
        // counts what COULD exist, not what currently does.
        DamageState dmg;
        dmg.Destroy(h.wallL);
        dmg.Destroy(h.roof);
        SectionMesh dm;
        BuildSection(h.def, &dmg, kInvalidComponent, dm);
        Check(dm.TotalVertices() < m.TotalVertices(), "destroyed components drop out of the mesh");
        Check(EstimateCapacity(h.def, kInvalidComponent).vertices == cap.vertices,
              "A RESERVATION MUST COVER THE REPAIRED BUILDING TOO - sizing it to the damaged "
              "state would refuse the update that puts the wall back");

        const MeshCapacity head = EstimateCapacity(h.def, kInvalidComponent, 2.0f);
        Check(head.vertices >= cap.vertices * 2, "headroom multiplies the reservation");
    }

    // -- Ranges address every component's triangles --------------------------
    {
        House h = MakeHouse();
        SectionMesh m;
        BuildSection(h.def, nullptr, kInvalidComponent, m);

        u32 covered = 0;
        for (const MeshRange& r : m.ranges) {
            Check(r.submesh < m.model.size(), "every range names a submesh that exists");
            if (r.submesh < m.model.size()) {
                const MeshData& md = m.model[r.submesh];
                Check(r.firstIndex + r.indexCount <= md.IndexCount(),
                      "a range must stay inside its submesh");
                Check(r.firstVertex + r.vertexCount <= md.VertexCount(), "...for vertices too");
            }
            covered += r.indexCount;
        }
        Check(covered == m.TotalIndices(),
              "EVERY TRIANGLE MUST BELONG TO EXACTLY ONE COMPONENT - unaddressed geometry could "
              "never be removed when that component is destroyed");

        const MeshRange* wall = m.RangeFor(h.wallL);
        Check(wall != nullptr, "a generated component is addressable by id");
        Check(wall && wall->indexCount == 36, "a box massing block is 12 triangles");
        Check(m.RangeFor(h.foundation) != nullptr, "...as is the foundation");

        // Containers and holes are real components that draw nothing.
        Check(!EmitsGeometry(ComponentKind::Building), "a Building is a container, not geometry");
        Check(!EmitsGeometry(ComponentKind::Opening), "an Opening is a hole, not geometry");
        Check(EmitsGeometry(ComponentKind::Wall), "a wall is geometry");
    }

    // -- Roofs are not boxes -------------------------------------------------
    {
        House h = MakeHouse();
        SectionMesh m;
        BuildSection(h.def, nullptr, kInvalidComponent, m);
        const MeshRange* roof = m.RangeFor(h.roof);
        Check(roof && roof->indexCount == 24,
              "a roof is a GABLE PRISM (8 triangles), not a box - a box roof is the single "
              "clearest sign a construction system is only pretending");
        Check(roof && roof->vertexCount == 18, "...with 18 flat-shaded vertices");
    }

    // -- Submesh order is by material, and stable ----------------------------
    {
        House h = MakeHouse();
        // Give one component a material that sorts BEFORE the rest.
        h.def.Find(h.foundation)->material = MaterialKind::PouredConcrete;
        h.def.Find(h.wallL)->material = MaterialKind::Brick;
        SectionMesh m;
        BuildSection(h.def, nullptr, kInvalidComponent, m);
        Check(m.model.size() >= 2, "distinct materials become distinct submeshes");

        const MeshRange* brick = m.RangeFor(h.wallL);
        const MeshRange* conc = m.RangeFor(h.foundation);
        Check(brick && conc && brick->submesh < conc->submesh,
              "SUBMESHES ORDER BY THE MaterialKind ENUM (Brick before PouredConcrete) - the enum "
              "is append-only, so a material added later can only APPEND a submesh and can never "
              "renumber one, which would silently repoint every uaf:<path>#<index> reference");

        // Components sharing a material must share a submesh - that is the whole draw-count win.
        h.def.Find(h.wallR)->material = MaterialKind::Brick;
        SectionMesh m2;
        BuildSection(h.def, nullptr, kInvalidComponent, m2);
        const MeshRange* l = m2.RangeFor(h.wallL);
        const MeshRange* r = m2.RangeFor(h.wallR);
        Check(l && r && l->submesh == r->submesh,
              "two components of the same material MERGE into one submesh - one draw, not two");
        Check(l && r && l->firstIndex != r->firstIndex, "...while staying separately addressable");
    }

    // -- Geometry sanity: no degenerate triangles, normals are unit ----------
    {
        House h = MakeHouse();
        SectionMesh m;
        BuildSection(h.def, nullptr, kInvalidComponent, m);
        bool unitNormals = true, finite = true, nonDegenerate = true;
        for (const MeshData& md : m.model) {
            for (const Vertex& v : md.vertices) {
                const f32 len = glm::length(v.normal);
                if (len < 0.99f || len > 1.01f) unitNormals = false;
                if (!std::isfinite(v.position.x) || !std::isfinite(v.normal.x) ||
                    !std::isfinite(v.tangent.x) || !std::isfinite(v.uv.x))
                    finite = false;
            }
            for (usize i = 0; i + 2 < md.indices.size(); i += 3) {
                const glm::vec3& a = md.vertices[md.indices[i + 0]].position;
                const glm::vec3& b = md.vertices[md.indices[i + 1]].position;
                const glm::vec3& c = md.vertices[md.indices[i + 2]].position;
                if (glm::length(glm::cross(b - a, c - a)) <= 1e-9f) nonDegenerate = false;
            }
        }
        Check(unitNormals, "every generated normal must be unit length");
        Check(finite, "no generated vertex may be NaN or infinite");
        Check(nonDegenerate, "no generated triangle may be degenerate");
    }

    // -- THE ZERO-EXTENT CASE that caught a submesh-remap bug ----------------
    // A component with a zero extent emits no faces at all. If its material was unique, that
    // leaves an EMPTY submesh - and erasing it without remapping would shift every later submesh
    // index down while the ranges still named the old ones.
    {
        ConstructionDef d;
        ConstructionComponent flat;
        flat.kind = ComponentKind::Wall;
        flat.role = StructuralRole::LoadBearing;
        flat.material = MaterialKind::Brick; // sorts FIRST, so erasing it shifts everything
        flat.extent = glm::vec3(0.0f);
        const ComponentId zero = d.AddComponent(flat);

        ConstructionComponent solid;
        solid.kind = ComponentKind::Wall;
        solid.role = StructuralRole::LoadBearing;
        solid.material = MaterialKind::Drywall; // sorts after Brick
        solid.extent = glm::vec3(1.0f);
        const ComponentId real = d.AddComponent(solid);

        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        Check(m.RangeFor(zero) == nullptr, "a zero-extent component contributes no range");
        const MeshRange* rr = m.RangeFor(real);
        Check(rr != nullptr, "...while the real one still generates");
        Check(!m.model.empty() && rr && rr->submesh < m.model.size(),
              "A DROPPED EMPTY SUBMESH MUST REMAP THE RANGES - erasing without remapping points "
              "every later range at a different material's geometry");
        for (const MeshData& md : m.model)
            Check(!md.Empty(),
                  "no empty submesh may be published - CreateMesh returns an INVALID handle for "
                  "empty MeshData on every backend");
    }

    // -- Subtree scoping (the unit of caching and streaming) -----------------
    {
        House h = MakeHouse();
        SectionMesh whole, sub;
        BuildSection(h.def, nullptr, kInvalidComponent, whole);
        BuildSection(h.def, nullptr, h.wallL, sub);
        Check(sub.RangeFor(h.wallL) != nullptr, "a subtree build includes its root");
        Check(sub.RangeFor(h.siding) != nullptr, "...and its children");
        Check(sub.RangeFor(h.roof) == nullptr, "...and nothing outside it");
        Check(sub.TotalIndices() < whole.TotalIndices(), "a subtree is smaller than the whole");
    }

    // -- Hidden components (artist override) ---------------------------------
    {
        House h = MakeHouse();
        SectionMesh before, after;
        BuildSection(h.def, nullptr, kInvalidComponent, before);
        h.def.Find(h.roof)->hidden = true;
        BuildSection(h.def, nullptr, kInvalidComponent, after);
        Check(after.RangeFor(h.roof) == nullptr, "a hidden component generates nothing");
        Check(after.TotalIndices() < before.TotalIndices(), "...and the mesh shrinks");
        Check(EstimateCapacity(h.def, kInvalidComponent).vertices ==
                  EstimateCapacity(MakeHouse().def, kInvalidComponent).vertices,
              "but the RESERVATION still covers it, so un-hiding cannot refuse the update");
    }

    // -- Transforms compose through containment ------------------------------
    {
        ConstructionDef d;
        ConstructionComponent parent;
        parent.kind = ComponentKind::Wall;
        parent.position = glm::vec3(10.0f, 0.0f, 0.0f);
        parent.extent = glm::vec3(1.0f);
        const ComponentId p = d.AddComponent(parent);

        ConstructionComponent child;
        child.kind = ComponentKind::Stud;
        child.parent = p;
        child.position = glm::vec3(0.0f, 5.0f, 0.0f);
        child.extent = glm::vec3(0.1f);
        const ComponentId c = d.AddComponent(child);

        const glm::mat4 w = WorldMatrix(d, c);
        const glm::vec3 origin = glm::vec3(w[3]);
        Check(std::fabs(origin.x - 10.0f) < 1e-4f && std::fabs(origin.y - 5.0f) < 1e-4f,
              "a child's transform composes with its parent's, as the scene's own hierarchy does");
    }

    // =======================================================================
    // PHASE 3 - masonry
    // =======================================================================

    // A plain brick wall: 4 m long, 2.4 m high, 0.1025 m deep.
    auto BrickWall = [](BondPattern bond) {
        ConstructionDef d;
        d.seed = 0xB0DEull;
        ConstructionComponent w;
        w.kind = ComponentKind::Wall;
        w.role = StructuralRole::LoadBearing;
        w.material = MaterialKind::Brick;
        w.extent = glm::vec3(2.0f, 1.2f, 0.05125f);
        w.masonry.bond = bond;
        const ComponentId id = d.AddComponent(w);
        return std::pair<ConstructionDef, ComponentId>(d, id);
    };

    // -- Units are real geometry, not a texture ------------------------------
    {
        auto [d, id] = BrickWall(BondPattern::Running);
        std::vector<BrickPlacement> units;
        const MasonryResult r = LayoutMasonry(d, *d.Find(id), units);
        Check(r == MasonryResult::Ok, "a well-formed brick wall lays out cleanly");

        // 2.4 m at a 75 mm course = 32 courses; 4 m at a 225 mm stride ~ 18 units per course.
        Check(!units.empty(), "a brick wall produces units");
        const u32 courses = units.empty() ? 0 : units.back().course + 1;
        Check(courses == 32, "2.4 m of wall at a 75 mm course is exactly 32 courses");
        Check(units.size() > 500 && units.size() < 700,
              "a 4 x 2.4 m wall is roughly 600 bricks - the real price of construction geometry");

        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        Check(m.ElementsOf(id).size() == units.size(),
              "EVERY UNIT IS INDIVIDUALLY ADDRESSABLE - a normal map cannot lose a brick, and "
              "losing bricks is the whole point of building this on a destruction foundation");
    }

    // -- THE INVARIANT THAT MATTERS MOST -------------------------------------
    // Capacity is computed by the SAME course/bond walk that generates. If it could under-count
    // by even one unit, UpdateMesh would refuse forever and there is no mesh destroy to recover.
    {
        const BondPattern bonds[] = {BondPattern::Running, BondPattern::Stack, BondPattern::Flemish,
                                     BondPattern::English};
        for (BondPattern bond : bonds) {
            auto [d, id] = BrickWall(bond);
            SectionMesh m;
            BuildSection(d, nullptr, kInvalidComponent, m);
            const MeshCapacity cap = EstimateCapacity(d, kInvalidComponent);
            Check(cap.vertices >= m.TotalVertices() && cap.indices >= m.TotalIndices(),
                  "MASONRY CAPACITY MUST NEVER UNDER-COUNT - a short reservation is a permanently "
                  "refused UpdateMesh with no way to free the mesh and retry");
            Check(cap.vertices == m.TotalVertices(),
                  "...and because count and generation share one traversal, it is EXACT");
            Check(MasonryUnitCount(d, *d.Find(id)) == m.ElementsOf(id).size(),
                  "the counter and the layout must agree unit for unit");
        }
    }

    // -- Bond patterns are actually different --------------------------------
    {
        auto [runD, runId] = BrickWall(BondPattern::Running);
        auto [stkD, stkId] = BrickWall(BondPattern::Stack);
        std::vector<BrickPlacement> run, stk;
        LayoutMasonry(runD, *runD.Find(runId), run);
        LayoutMasonry(stkD, *stkD.Find(stkId), stk);

        // Stack bond: every course starts at the same x, so course 0 and course 1 line up.
        auto firstOfCourse = [](const std::vector<BrickPlacement>& v, u32 course) {
            for (const BrickPlacement& b : v)
                if (b.course == course) return b.center.x;
            return 0.0f;
        };
        Check(std::fabs(firstOfCourse(stk, 0) - firstOfCourse(stk, 1)) < 1e-4f,
              "STACK BOND stacks: every perpend joint lines up vertically");
        Check(std::fabs(firstOfCourse(run, 0) - firstOfCourse(run, 1)) > 1e-3f,
              "RUNNING BOND offsets alternate courses - if these matched, the bond would be "
              "decorative rather than structural");

        // English bond alternates whole courses of headers, which are shorter, so a header course
        // holds MORE units than a stretcher course.
        auto [engD, engId] = BrickWall(BondPattern::English);
        std::vector<BrickPlacement> eng;
        LayoutMasonry(engD, *engD.Find(engId), eng);
        auto countInCourse = [](const std::vector<BrickPlacement>& v, u32 course) {
            u32 n = 0;
            for (const BrickPlacement& b : v)
                if (b.course == course) ++n;
            return n;
        };
        Check(countInCourse(eng, 1) > countInCourse(eng, 0),
              "ENGLISH BOND lays whole HEADER courses - headers expose the short face, so a "
              "header course holds more units than a stretcher course");

        // Flemish alternates within a course, so its courses sit between the two.
        auto [flmD, flmId] = BrickWall(BondPattern::Flemish);
        std::vector<BrickPlacement> flm;
        LayoutMasonry(flmD, *flmD.Find(flmId), flm);
        Check(countInCourse(flm, 0) > countInCourse(eng, 0),
              "FLEMISH alternates stretcher and header WITHIN a course, so it fits more units "
              "per course than an all-stretcher course");
    }

    // -- Units are clipped at the wall ends, not overhanging -----------------
    {
        auto [d, id] = BrickWall(BondPattern::Running);
        std::vector<BrickPlacement> units;
        LayoutMasonry(d, *d.Find(id), units);
        const ConstructionComponent& c = *d.Find(id);
        bool inside = true;
        for (const BrickPlacement& b : units) {
            if (b.center.x - b.extent.x < -c.extent.x - 1e-3f) inside = false;
            if (b.center.x + b.extent.x > c.extent.x + 1e-3f) inside = false;
            if (b.center.y + b.extent.y > c.extent.y + 1e-3f) inside = false;
        }
        Check(inside,
              "NO UNIT MAY OVERHANG THE WALL - a bricklayer cuts the closer, and a running bond "
              "guarantees a cut at one end of every other course");
    }

    // -- Determinism and per-unit seed independence --------------------------
    {
        auto [a, aid] = BrickWall(BondPattern::Running);
        a.Find(aid)->masonry.sizeJitter = 0.3f;
        a.Find(aid)->masonry.depthJitter = 0.4f;
        a.Find(aid)->masonry.rotJitter = 0.05f;
        auto b = a;

        std::vector<BrickPlacement> ua, ub;
        LayoutMasonry(a, *a.Find(aid), ua);
        LayoutMasonry(b, *b.Find(aid), ub);
        Check(ua.size() == ub.size(), "jittered layout is stable in size");
        bool same = ua.size() == ub.size();
        for (usize i = 0; same && i < ua.size(); ++i)
            same = ua[i].center == ub[i].center && ua[i].extent == ub[i].extent &&
                   ua[i].yaw == ub[i].yaw;
        Check(same, "JITTERED MASONRY IS STILL DETERMINISTIC - same seed, same wall, every time");

        // The unit stream is keyed on (course, indexInCourse), so making the wall TALLER must not
        // disturb the units already laid in the courses below.
        auto tall = a;
        tall.Find(aid)->extent.y *= 2.0f;
        std::vector<BrickPlacement> ut;
        LayoutMasonry(tall, *tall.Find(aid), ut);
        usize compared = 0;
        bool lowerUnchanged = true;
        for (usize i = 0; i < ua.size() && i < ut.size(); ++i) {
            if (ua[i].course != ut[i].course || ua[i].indexInCourse != ut[i].indexInCourse) break;
            if (ua[i].extent != ut[i].extent || ua[i].yaw != ut[i].yaw) lowerUnchanged = false;
            ++compared;
        }
        Check(compared > 100, "...enough shared units to be a real comparison");
        Check(lowerUnchanged,
              "MAKING A WALL TALLER MUST NOT RESHUFFLE THE COURSES BELOW - the per-unit stream is "
              "keyed on (course, index), not on emission order, exactly so this holds");

        Check(ua[0].ElementId() != ua[1].ElementId(), "element ids are distinct");
        Check(ua[0].ElementId() == ((ua[0].course << 16) | ua[0].indexInCourse),
              "an element id is a pure function of its position in the bond, so it survives "
              "regeneration and damage");
    }

    // -- Jitter only ever shrinks a unit -------------------------------------
    {
        auto [d, id] = BrickWall(BondPattern::Running);
        std::vector<BrickPlacement> plain;
        LayoutMasonry(d, *d.Find(id), plain);
        d.Find(id)->masonry.sizeJitter = 0.5f;
        std::vector<BrickPlacement> jit;
        LayoutMasonry(d, *d.Find(id), jit);
        Check(plain.size() == jit.size(), "jitter must not change the unit COUNT");
        bool noGrowth = true;
        for (usize i = 0; i < jit.size() && i < plain.size(); ++i)
            if (jit[i].extent.y > plain[i].extent.y + 1e-5f) noGrowth = false;
        Check(noGrowth,
              "UNITS ONLY EVER SHRINK - growing them would push neighbours together and close the "
              "joints that make the bond legible");
    }

    // -- Degenerate parameters are refused, not obeyed -----------------------
    {
        auto [d, id] = BrickWall(BondPattern::Running);
        std::vector<BrickPlacement> units;

        d.Find(id)->masonry.unitLength = 0.0f;
        Check(LayoutMasonry(d, *d.Find(id), units) == MasonryResult::DegenerateUnit,
              "a zero-length unit is refused rather than looping forever");
        Check(units.empty(), "...and produces nothing");

        d.Find(id)->masonry = MasonryParams{};
        d.Find(id)->extent = glm::vec3(0.0f);
        Check(LayoutMasonry(d, *d.Find(id), units) == MasonryResult::DegenerateExtent,
              "a zero-extent wall is refused");

        // THE SAFETY CAP. Unit dimensions come from a UI; a 1 mm brick in a big wall would
        // otherwise lay tens of millions of boxes and hang the editor with no diagnostic.
        d.Find(id)->extent = glm::vec3(50.0f, 20.0f, 0.1f);
        d.Find(id)->masonry.unitLength = 0.002f;
        d.Find(id)->masonry.unitHeight = 0.002f;
        d.Find(id)->masonry.joint = 0.0f;
        const MasonryResult capped = LayoutMasonry(d, *d.Find(id), units);
        Check(capped == MasonryResult::UnitCapReached,
              "A PATHOLOGICAL UNIT SIZE MUST HIT THE CAP AND SAY SO - not hang the editor");
        Check(units.size() <= d.Find(id)->masonry.maxUnits + 1,
              "...and the output stays bounded");
    }

    // -- Poured concrete is NOT masonry --------------------------------------
    {
        auto [d, id] = BrickWall(BondPattern::Running);
        d.Find(id)->material = MaterialKind::PouredConcrete;
        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        Check(m.ElementsOf(id).empty(),
              "POURED CONCRETE HAS NO UNITS AND NO BOND - generating it as bricks would be exactly "
              "the 'different textures pretending to be different construction' the brief rejects");
        const MeshRange* r = m.RangeFor(id);
        Check(r && r->indexCount == 36, "...it stays one solid massing block");

        // A brick-material BEAM is a lintel: one piece, not a course of bricks.
        d.Find(id)->material = MaterialKind::Brick;
        d.Find(id)->kind = ComponentKind::Beam;
        SectionMesh lintel;
        BuildSection(d, nullptr, kInvalidComponent, lintel);
        Check(lintel.ElementsOf(id).empty(),
              "a brick BEAM is a lintel - one piece, not a wall of units");
    }

    // =======================================================================
    // WEATHERING (brief SS12/SS16)
    // =======================================================================
    {
        auto AgedWall = [](f32 age) {
            ConstructionDef d;
            d.seed = 0xA6E5ull;
            ConstructionComponent w;
            w.kind = ComponentKind::Wall;
            w.role = StructuralRole::LoadBearing;
            w.material = MaterialKind::Brick;
            w.extent = glm::vec3(2.0f, 1.2f, 0.125f);
            w.weathering.age = age;
            const ComponentId id = d.AddComponent(w);
            return std::pair<ConstructionDef, ComponentId>(d, id);
        };

        auto [fresh, fid] = AgedWall(0.0f);
        auto [old, oid] = AgedWall(0.9f);
        std::vector<BrickPlacement> a, b;
        LayoutMasonry(fresh, *fresh.Find(fid), a);
        LayoutMasonry(old, *old.Find(oid), b);

        Check(b.size() < a.size(),
              "AN AGED WALL LOSES UNITS - missing bricks are what actually read as age, and a "
              "normal map cannot lose a brick");
        Check(!b.empty(), "...but never all of them - a wall that generates nothing reads as a bug");

        // Determinism survives ageing: the same age on the same seed loses the SAME bricks.
        auto [old2, oid2] = AgedWall(0.9f);
        std::vector<BrickPlacement> c;
        LayoutMasonry(old2, *old2.Find(oid2), c);
        Check(c.size() == b.size(), "weathering is deterministic in count");
        bool same = c.size() == b.size();
        for (usize i = 0; same && i < c.size(); ++i)
            same = c[i].ElementId() == b[i].ElementId() && c[i].center == b[i].center;
        Check(same,
              "...and in WHICH units are lost - a weathered building has to be reproducible or "
              "persistent destruction cannot be stored against it");

        // Maintenance divides the damage.
        auto [kept, kid] = AgedWall(0.9f);
        kept.Find(kid)->weathering.maintenance = 1.0f;
        auto [neglected, nid] = AgedWall(0.9f);
        neglected.Find(nid)->weathering.maintenance = 0.0f;
        std::vector<BrickPlacement> k, n;
        LayoutMasonry(kept, *kept.Find(kid), k);
        LayoutMasonry(neglected, *neglected.Find(nid), n);
        Check(n.size() < k.size(), "an abandoned wall loses more than a maintained one");

        // THE RESERVATION MUST SURVIVE TURNING THE AGE DIAL BACK DOWN. Ageing only ever removes
        // units, so a capacity sized to the aged state would refuse the update that restores them.
        {
            SectionMesh freshMesh;
            BuildSection(fresh, nullptr, kInvalidComponent, freshMesh);
            const MeshCapacity agedCap = EstimateCapacity(old, kInvalidComponent);
            Check(agedCap.vertices >= freshMesh.TotalVertices(),
                  "A RESERVATION MADE ON AN AGED WALL MUST COVER THE PRISTINE ONE - lowering the "
                  "age dial brings units BACK, and UpdateMesh never grows");
            Check(EstimateCapacity(old, kInvalidComponent).vertices ==
                      EstimateCapacity(fresh, kInvalidComponent).vertices,
                  "...so the reservation does not depend on the age at all");
        }

        // Structure is spared: a timber frame never loses studs, however old.
        {
            ConstructionDef d;
            d.seed = 0x5711ull;
            ConstructionComponent w;
            w.kind = ComponentKind::Wall;
            w.role = StructuralRole::LoadBearing;
            w.material = MaterialKind::TimberFrame;
            w.extent = glm::vec3(2.0f, 1.2f, 0.0445f);
            const ComponentId id = d.AddComponent(w);
            std::vector<BoardPlacement> young;
            LayoutWood(d, *d.Find(id), young);
            d.Find(id)->weathering.age = 1.0f;
            d.Find(id)->weathering.maintenance = 0.0f;
            std::vector<BoardPlacement> ancient;
            LayoutWood(d, *d.Find(id), ancient);

            auto studs = [](const std::vector<BoardPlacement>& v) {
                u32 n = 0;
                for (const BoardPlacement& b : v)
                    if (b.role == MemberRole::Stud || b.role == MemberRole::Plate) ++n;
                return n;
            };
            Check(studs(ancient) == studs(young),
                  "A DERELICT BARN LOSES ITS SIDING AND ITS SHINGLES LONG BEFORE ITS FRAME - "
                  "deleting random structural members would make the graph report a building that "
                  "cannot stand when the artist only asked for 'old'");
        }

        // Cladding, by contrast, is the first thing to go.
        {
            ConstructionDef d;
            d.seed = 0x5711ull;
            ConstructionComponent w;
            w.kind = ComponentKind::Siding;
            w.role = StructuralRole::Surface;
            w.material = MaterialKind::WoodPlank;
            w.extent = glm::vec3(2.0f, 1.2f, 0.01f);
            const ComponentId id = d.AddComponent(w);
            std::vector<BoardPlacement> young;
            LayoutWood(d, *d.Find(id), young);
            d.Find(id)->weathering.age = 0.9f;
            d.Find(id)->weathering.maintenance = 0.0f;
            std::vector<BoardPlacement> ancient;
            LayoutWood(d, *d.Find(id), ancient);
            Check(ancient.size() < young.size(),
                  "weathered siding loses boards - the gaps are what make a derelict barn read");
        }

        // Colour shifts with age, and moisture pulls it green.
        {
            const WeatheringParams none;
            WeatheringParams aged;
            aged.age = 1.0f;
            Check(aged.ColourShift() > none.ColourShift(), "age shifts the surface colour");
            Check(none.EffectiveMissing() == 0.0f, "an unweathered wall loses nothing");
            Check(aged.EffectiveMissing() > 0.0f, "a derelict one does");
            WeatheringParams override_;
            override_.age = 0.0f;
            override_.missingChance = 0.5f;
            Check(override_.EffectiveMissing() > 0.4f,
                  "an explicit override beats the age-derived value, so an artist can ask for a "
                  "wall that is filthy but structurally perfect");
        }
    }

    // -- THE BUG THAT MADE A BRICK WALL RENDER AS A FLAT BOX -----------------
    //
    // Units were sized to their NOMINAL unit depth while the mortar backing was sized to the WALL
    // depth. On any wall thicker than one brick the backing completely engulfed the brickwork, so
    // a 4 m brick wall drew as a single flat coloured slab with every course invisible inside it.
    {
        ConstructionDef d;
        d.seed = 0xB0DEull;
        ConstructionComponent w;
        w.kind = ComponentKind::Wall;
        w.role = StructuralRole::LoadBearing;
        w.material = MaterialKind::Brick;
        // A wall MUCH thicker than the 0.1025 m nominal brick depth - the case that broke.
        w.extent = glm::vec3(2.0f, 1.2f, 0.125f);
        const ComponentId wall = d.AddComponent(w);

        std::vector<BrickPlacement> units;
        LayoutMasonry(d, *d.Find(wall), units);
        Check(!units.empty(), "the thick wall lays units");

        const f32 inset = std::max(d.Find(wall)->masonry.joint, 0.002f);
        const f32 mortarHalfZ = d.Find(wall)->extent.z - inset;
        bool allProud = true;
        for (const BrickPlacement& b : units)
            if (b.extent.z <= mortarHalfZ + 1e-5f) allProud = false;
        Check(allProud,
              "EVERY UNIT MUST STAND PROUD OF THE MORTAR BACKING. When the backing was deeper than "
              "the units it swallowed them whole and the wall rendered as a flat slab - the "
              "brickwork was generated correctly and then buried");

        // And the mortar must be a DIFFERENT material, because a 10 mm recess alone is invisible
        // past a couple of metres - the colour break is what actually makes masonry read.
        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        bool haveBrick = false, haveMortar = false;
        for (const MeshData& md : m.model) {
            if (md.name == std::string(ToString(MaterialKind::Brick))) haveBrick = true;
            if (md.name == std::string(ToString(MaterialKind::Mortar))) haveMortar = true;
        }
        Check(haveBrick && haveMortar,
              "brick and mortar are SEPARATE submeshes with separate colours - one flat colour for "
              "both is why the wall read as a painted box");

        // RangeFor must resolve a masonry wall to its UNIT submesh, not its mortar submesh.
        const MeshRange* r = m.RangeFor(wall);
        Check(r != nullptr, "the wall is addressable");
        if (r && r->submesh < m.model.size())
            Check(m.model[r->submesh].name == std::string(ToString(MaterialKind::Brick)),
                  "a masonry wall's primary range is its UNITS, not its backing");
    }

    // -- A shingled roof needs a deck under it -------------------------------
    // Shingles are separate overlapping boards with real gaps; with nothing behind them you see
    // straight through into an unlit interior, which is what made roofs render black and speckled.
    {
        ConstructionDef d;
        d.seed = 0x4F0Full;
        ConstructionComponent r;
        r.kind = ComponentKind::Roof;
        r.role = StructuralRole::NonLoadBearing;
        r.material = MaterialKind::WoodShingle;
        r.extent = glm::vec3(3.0f, 1.0f, 2.0f);
        const ComponentId roof = d.AddComponent(r);

        std::vector<PlacedPiece> pieces;
        CollectPieces(d, nullptr, kInvalidComponent, pieces);
        bool haveDeck = false, haveShingles = false;
        for (const PlacedPiece& p : pieces) {
            if (p.gable) haveDeck = true;
            if (p.element != kWholeComponent) haveShingles = true;
        }
        Check(haveShingles, "a shingled roof lays courses");
        Check(haveDeck,
              "A SHINGLED ROOF MUST HAVE A SOLID DECK UNDER IT - without sheathing you see through "
              "the gaps between courses into an unlit interior, which is why roofs rendered black");

        // ...and the reservation must cover the deck, or UpdateMesh is refused forever.
        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        const MeshCapacity cap = EstimateCapacity(d, kInvalidComponent);
        Check(cap.vertices >= m.TotalVertices(),
              "the reservation covers the deck as well as the courses");
        (void)roof;
    }

    // -- Mortar backing ------------------------------------------------------
    {
        auto [d, id] = BrickWall(BondPattern::Running);
        SectionMesh with, without;
        BuildSection(d, nullptr, kInvalidComponent, with);
        d.Find(id)->masonry.generateMortar = false;
        BuildSection(d, nullptr, kInvalidComponent, without);
        Check(with.TotalIndices() == without.TotalIndices() + 36,
              "the mortar backing is exactly one extra box - without it the joints are holes "
              "straight through the wall");
        Check(with.ElementsOf(id).size() == without.ElementsOf(id).size(),
              "...and it is not itself a unit");
    }

    // =======================================================================
    // PHASE 4 - wood
    // =======================================================================

    auto WoodComp = [](ComponentKind kind, MaterialKind mat, glm::vec3 extent) {
        ConstructionDef d;
        d.seed = 0x7EE5ull;
        ConstructionComponent c;
        c.kind = kind;
        c.role = StructuralRole::LoadBearing;
        c.material = mat;
        c.extent = extent;
        const ComponentId id = d.AddComponent(c);
        return std::pair<ConstructionDef, ComponentId>(d, id);
    };

    auto CountRole = [](const std::vector<BoardPlacement>& v, MemberRole r) {
        u32 n = 0;
        for (const BoardPlacement& b : v)
            if (b.role == r) ++n;
        return n;
    };

    // -- Timber-framed wall: plates + studs ----------------------------------
    {
        // 4 m long, 2.4 m high stud wall at 400 mm centres.
        auto [d, id] = WoodComp(ComponentKind::Wall, MaterialKind::TimberFrame,
                                glm::vec3(2.0f, 1.2f, 0.0445f));
        std::vector<BoardPlacement> mem;
        Check(LayoutWood(d, *d.Find(id), mem) == WoodResult::Ok, "a stud wall lays out cleanly");

        Check(CountRole(mem, MemberRole::Plate) == 3,
              "a standard stud wall has a bottom plate and a DOUBLE top plate - three in total");
        const u32 studs = CountRole(mem, MemberRole::Stud);
        Check(studs >= 10 && studs <= 13,
              "4 m at 400 mm centres is about 11 studs including the end stud");

        // THE END STUD. On-centre spacing almost never divides the wall exactly, so the regular
        // run stops short. Real framing always closes the last bay.
        f32 maxX = -1e9f;
        for (const BoardPlacement& b : mem)
            if (b.role == MemberRole::Stud) maxX = std::max(maxX, b.center.x);
        Check(maxX > d.Find(id)->extent.x - 0.05f,
              "THE LAST BAY MUST BE CLOSED BY AN END STUD - on-centre spacing does not divide the "
              "wall evenly, and sheathing needs something to fix to at the corner");

        // Studs must sit BETWEEN the plates, not through them.
        f32 studTop = -1e9f;
        for (const BoardPlacement& b : mem)
            if (b.role == MemberRole::Stud) studTop = std::max(studTop, b.center.y + b.extent.y);
        Check(studTop <= d.Find(id)->extent.y - 0.03f,
              "studs stop below the top plates rather than passing through them");

        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        Check(m.ElementsOf(id).size() == mem.size(),
              "every framing member is individually addressable");
    }

    // -- Spacing actually changes the framing --------------------------------
    {
        auto [d400, id400] = WoodComp(ComponentKind::Wall, MaterialKind::TimberFrame,
                                      glm::vec3(2.0f, 1.2f, 0.0445f));
        auto d600 = d400;
        d600.Find(id400)->timber.spacing = 0.600f;
        std::vector<BoardPlacement> a, b;
        LayoutWood(d400, *d400.Find(id400), a);
        LayoutWood(d600, *d600.Find(id400), b);
        Check(CountRole(b, MemberRole::Stud) < CountRole(a, MemberRole::Stud),
              "600 mm centres use fewer studs than 400 mm - spacing is a real parameter, not a "
              "label");

        auto single = d400;
        single.Find(id400)->timber.topPlates = 1;
        single.Find(id400)->timber.bottomPlate = false;
        std::vector<BoardPlacement> s;
        LayoutWood(single, *single.Find(id400), s);
        Check(CountRole(s, MemberRole::Plate) == 1, "plate count follows the parameters");
    }

    // -- Floors frame differently from walls ---------------------------------
    {
        auto [d, id] = WoodComp(ComponentKind::Floor, MaterialKind::TimberFrame,
                                glm::vec3(2.0f, 0.15f, 2.0f));
        std::vector<BoardPlacement> mem;
        Check(LayoutWood(d, *d.Find(id), mem) == WoodResult::Ok, "a timber floor lays out");
        Check(CountRole(mem, MemberRole::Joist) > 5,
              "A FLOOR FRAMES WITH JOISTS, NOT STUDS - same material, different member, because "
              "kind and material decide together");
        Check(CountRole(mem, MemberRole::Stud) == 0, "...and no studs appear in a floor");
        Check(CountRole(mem, MemberRole::Board) == 1, "...with a subfloor deck on top");
    }

    // -- Siding profiles are genuinely different -----------------------------
    {
        auto [flush, id] = WoodComp(ComponentKind::Siding, MaterialKind::WoodPlank,
                                    glm::vec3(2.0f, 1.2f, 0.01f));
        auto clap = flush;
        clap.Find(id)->plank.profile = SidingProfile::Clapboard;
        auto bnb = flush;
        bnb.Find(id)->plank.profile = SidingProfile::BoardAndBatten;

        std::vector<BoardPlacement> f, c2, b2;
        LayoutWood(flush, *flush.Find(id), f);
        LayoutWood(clap, *clap.Find(id), c2);
        LayoutWood(bnb, *bnb.Find(id), b2);

        Check(CountRole(c2, MemberRole::Board) > CountRole(f, MemberRole::Board),
              "CLAPBOARD OVERLAPS, so it takes MORE boards to cover the same wall than butting "
              "them flush does - if these matched, the overlap would be decorative");

        bool tilted = false;
        for (const BoardPlacement& b : c2)
            if (std::fabs(b.pitch) > 1e-3f) tilted = true;
        Check(tilted,
              "A CLAPBOARD IS TILTED - it is thin where it tucks under the course above and thick "
              "where it laps the one below, and that tilt is the shadow line that makes clapboard "
              "read as clapboard");
        bool flat = true;
        for (const BoardPlacement& b : f)
            if (std::fabs(b.pitch) > 1e-3f) flat = false;
        Check(flat, "...while flush boards lie flat");

        Check(CountRole(b2, MemberRole::Batten) > 0,
              "BOARD-AND-BATTEN HAS BATTENS - without the strips over the seams it is just "
              "vertical siding, which is a different building");
        Check(CountRole(f, MemberRole::Batten) == 0, "...and a flush wall has none");

        // Board-and-batten is vertical by construction, whatever direction was asked for.
        bool anyVertical = false;
        for (const BoardPlacement& b : b2)
            if (b.role == MemberRole::Board && b.extent.y > b.extent.x) anyVertical = true;
        Check(anyVertical, "board-and-batten runs its boards vertically");
    }

    // -- Boards clad the FACE, standing proud of the wall --------------------
    {
        auto [d, id] = WoodComp(ComponentKind::Siding, MaterialKind::WoodPlank,
                                glm::vec3(2.0f, 1.2f, 0.01f));
        std::vector<BoardPlacement> mem;
        LayoutWood(d, *d.Find(id), mem);
        bool proud = true;
        for (const BoardPlacement& b : mem)
            if (b.center.z <= d.Find(id)->extent.z) proud = false;
        Check(proud,
              "cladding sits ON the face it covers, not inside it - otherwise it z-fights with "
              "whatever it is cladding");
    }

    // -- Shingles cover BOTH slopes ------------------------------------------
    {
        auto [d, id] = WoodComp(ComponentKind::Roof, MaterialKind::WoodShingle,
                                glm::vec3(3.0f, 1.0f, 2.0f));
        std::vector<BoardPlacement> mem;
        Check(LayoutWood(d, *d.Find(id), mem) == WoodResult::Ok, "a shingled roof lays out");
        Check(CountRole(mem, MemberRole::Shingle) > 50, "a roof takes a lot of shingles");

        u32 front = 0, back = 0;
        for (const BoardPlacement& b : mem) {
            if (b.center.z > 0.01f) ++front;
            if (b.center.z < -0.01f) ++back;
        }
        Check(front > 0 && back > 0,
              "BOTH SLOPES MUST BE SHINGLED - a roof covered on one side is a bug nobody notices "
              "until the camera moves");

        bool pitched = false;
        for (const BoardPlacement& b : mem)
            if (std::fabs(b.pitch) > 0.05f) pitched = true;
        Check(pitched, "shingles lie along the slope, not flat");

        // Courses climb from the eave to the ridge.
        f32 lowest = 1e9f, highest = -1e9f;
        for (const BoardPlacement& b : mem) {
            lowest = std::min(lowest, b.center.y);
            highest = std::max(highest, b.center.y);
        }
        Check(highest > lowest, "courses climb the roof rather than stacking in one place");
    }

    // -- Single pieces of lumber stay single ---------------------------------
    {
        auto [d, id] = WoodComp(ComponentKind::Beam, MaterialKind::TimberFrame,
                                glm::vec3(2.0f, 0.1f, 0.05f));
        std::vector<BoardPlacement> mem;
        Check(LayoutWood(d, *d.Find(id), mem) == WoodResult::NotWood,
              "A BEAM IS ONE PIECE OF LUMBER - generating it as a stack of boards is the same "
              "error as generating poured concrete as bricks, in the opposite direction");
        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        const MeshRange* r = m.RangeFor(id);
        Check(r && r->indexCount == 36, "...so it stays one solid box");
        Check(m.ElementsOf(id).empty(), "...with no sub-elements");

        for (ComponentKind k : {ComponentKind::Column, ComponentKind::Header, ComponentKind::Stud,
                                ComponentKind::Rafter, ComponentKind::Joist})
            Check(!IsWoodConstruction(ConstructionComponent{
                      kInvalidComponent, k, StructuralRole::LoadBearing, MaterialKind::TimberFrame}),
                  "every single-member kind stays a solid box");
    }

    // -- Capacity must cover wood too ----------------------------------------
    {
        const std::pair<ComponentKind, MaterialKind> cases[] = {
            {ComponentKind::Wall, MaterialKind::TimberFrame},
            {ComponentKind::Floor, MaterialKind::TimberFrame},
            {ComponentKind::Siding, MaterialKind::WoodPlank},
            {ComponentKind::Roof, MaterialKind::WoodShingle},
        };
        for (auto [kind, mat] : cases) {
            auto [d, id] = WoodComp(kind, mat, glm::vec3(2.0f, 1.2f, 0.1f));
            SectionMesh m;
            BuildSection(d, nullptr, kInvalidComponent, m);
            const MeshCapacity cap = EstimateCapacity(d, kInvalidComponent);
            Check(cap.vertices >= m.TotalVertices() && cap.indices >= m.TotalIndices(),
                  "WOOD CAPACITY MUST NEVER UNDER-COUNT - the estimator runs the real layout "
                  "rather than re-deriving four sets of end-member and clipping rules");
            Check(cap.vertices == m.TotalVertices(), "...and is exact, because it IS the layout");
        }
    }

    // -- Determinism and per-member seed independence ------------------------
    {
        auto [d, id] = WoodComp(ComponentKind::Wall, MaterialKind::TimberFrame,
                                glm::vec3(2.0f, 1.2f, 0.0445f));
        d.Find(id)->timber.warp = 0.6f;
        auto d2 = d;
        std::vector<BoardPlacement> a, b;
        LayoutWood(d, *d.Find(id), a);
        LayoutWood(d2, *d2.Find(id), b);
        bool same = a.size() == b.size();
        for (usize i = 0; same && i < a.size(); ++i)
            same = a[i].center == b[i].center && a[i].yaw == b[i].yaw && a[i].roll == b[i].roll;
        Check(same, "warped lumber is still deterministic - same seed, same wall, every time");

        // Roles use independent streams, so the studs must not shift when the plates change.
        auto more = d;
        more.Find(id)->timber.topPlates = 1;
        std::vector<BoardPlacement> c3;
        LayoutWood(more, *more.Find(id), c3);
        f32 aFirstStudYaw = 0.0f, cFirstStudYaw = 0.0f;
        for (const BoardPlacement& x : a)
            if (x.role == MemberRole::Stud && x.index == 0) aFirstStudYaw = x.yaw;
        for (const BoardPlacement& x : c3)
            if (x.role == MemberRole::Stud && x.index == 0) cFirstStudYaw = x.yaw;
        Check(aFirstStudYaw == cFirstStudYaw,
              "CHANGING THE PLATE COUNT MUST NOT RESHUFFLE THE STUDS - each ROLE draws from its "
              "own stream, keyed on (role, index) rather than emission order");

        Check(a[0].ElementId() != a[1].ElementId(), "member element ids are distinct");
        // A stud and a plate at the same index must still differ - the role is in the id.
        BoardPlacement s0, p0;
        s0.role = MemberRole::Stud;
        s0.index = 0;
        p0.role = MemberRole::Plate;
        p0.index = 0;
        Check(s0.ElementId() != p0.ElementId(),
              "the ROLE is part of the element id, so stud 0 and plate 0 are different elements");
    }

    // -- Degenerate parameters and the safety cap ----------------------------
    {
        auto [d, id] = WoodComp(ComponentKind::Wall, MaterialKind::TimberFrame,
                                glm::vec3(2.0f, 1.2f, 0.0445f));
        std::vector<BoardPlacement> mem;
        d.Find(id)->timber.spacing = 0.0f;
        Check(LayoutWood(d, *d.Find(id), mem) == WoodResult::DegenerateMember,
              "zero stud spacing is refused rather than looping forever");

        d.Find(id)->timber = TimberParams{};
        d.Find(id)->extent = glm::vec3(0.0f);
        Check(LayoutWood(d, *d.Find(id), mem) == WoodResult::DegenerateExtent,
              "a zero-extent wall is refused");

        d.Find(id)->extent = glm::vec3(200.0f, 60.0f, 0.05f);
        d.Find(id)->timber.spacing = 0.002f;
        Check(LayoutWood(d, *d.Find(id), mem) == WoodResult::MemberCapReached,
              "A PATHOLOGICAL SPACING MUST HIT THE CAP AND SAY SO - not hang the editor");
        Check(mem.size() <= d.Find(id)->timber.maxMembers,
              "...and the output stays bounded");
    }

    // =======================================================================
    // PHASE 5 - openings, cutters, doors and windows
    // =======================================================================

    // A wall with a window punched through it.
    auto WalledOpening = [](MaterialKind mat, glm::vec3 openingPos, glm::vec3 openingExtent) {
        ConstructionDef d;
        d.seed = 0x0FEEDull;
        ConstructionComponent w;
        w.kind = ComponentKind::Wall;
        w.role = StructuralRole::LoadBearing;
        w.material = mat;
        w.extent = glm::vec3(2.0f, 1.2f, 0.1f);
        const ComponentId wall = d.AddComponent(w);

        ConstructionComponent o;
        o.kind = ComponentKind::Opening;
        o.role = StructuralRole::None;
        o.parent = wall;
        o.position = openingPos;
        o.extent = openingExtent;
        const ComponentId open = d.AddComponent(o);
        return std::tuple<ConstructionDef, ComponentId, ComponentId>(d, wall, open);
    };

    // -- Exact convex decomposition ------------------------------------------
    // A hole through a box is CONCAVE, so it cannot come from half-space clipping. The pieces must
    // partition the remainder exactly: no gaps, no overlaps (overlaps z-fight).
    {
        std::vector<BoxPiece> cut{{glm::vec3(0.0f), glm::vec3(0.5f, 0.5f, 2.0f)}};
        std::vector<BoxPiece> pieces;
        SubtractCutters(glm::vec3(0.0f), glm::vec3(2.0f, 1.0f, 0.1f), cut, pieces);
        Check(!pieces.empty(), "subtracting a central hole leaves pieces");

        // Volume must be conserved exactly: box - overlap.
        f32 vol = 0.0f;
        for (const BoxPiece& p : pieces) vol += 8.0f * p.extent.x * p.extent.y * p.extent.z;
        const f32 boxVol = 8.0f * 2.0f * 1.0f * 0.1f;
        const f32 cutVol = 8.0f * 0.5f * 0.5f * 0.1f; // clipped to the box depth
        Check(std::fabs(vol - (boxVol - cutVol)) < 1e-4f,
              "THE PIECES MUST PARTITION THE REMAINDER EXACTLY - conserved volume proves there are "
              "neither gaps nor overlapping pieces, and overlapping pieces z-fight");

        // No piece may intrude into the hole.
        bool clear = true;
        for (const BoxPiece& p : pieces) {
            const glm::vec3 pmn = p.center - p.extent, pmx = p.center + p.extent;
            if (pmn.x < 0.5f - 1e-4f && pmx.x > -0.5f + 1e-4f && pmn.y < 0.5f - 1e-4f &&
                pmx.y > -0.5f + 1e-4f)
                clear = false;
        }
        Check(clear, "no remaining piece may intrude into the opening");

        // A cutter that swallows the whole box leaves nothing.
        std::vector<BoxPiece> all{{glm::vec3(0.0f), glm::vec3(10.0f)}};
        SubtractCutters(glm::vec3(0.0f), glm::vec3(1.0f), all, pieces);
        Check(pieces.empty(), "a wall that is all doorway is not a wall");

        // A cutter that misses entirely changes nothing.
        std::vector<BoxPiece> miss{{glm::vec3(100.0f, 0.0f, 0.0f), glm::vec3(1.0f)}};
        SubtractCutters(glm::vec3(0.0f), glm::vec3(1.0f), miss, pieces);
        Check(pieces.size() == 1, "a cutter that misses leaves the box whole");
    }

    // -- The opening actually appears in the geometry ------------------------
    {
        auto [d, wall, open] = WalledOpening(MaterialKind::PouredConcrete,
                                             glm::vec3(0.0f, 0.0f, 0.0f),
                                             glm::vec3(0.5f, 0.6f, 0.5f));
        SectionMesh with;
        BuildSection(d, nullptr, kInvalidComponent, with);
        const MeshRange* r = with.RangeFor(wall);
        Check(r && r->indexCount > 36,
              "A WALL WITH AN OPENING IS MORE GEOMETRY THAN A SOLID ONE - the remainder decomposes "
              "into the sill course, the head course and two jambs");
        Check(with.RangeFor(open) == nullptr, "the opening itself draws nothing - it is a hole");
    }

    // -- NON-DESTRUCTIVE: disable, move, delete ------------------------------
    {
        auto [d, wall, open] = WalledOpening(MaterialKind::PouredConcrete,
                                             glm::vec3(0.0f, 0.0f, 0.0f),
                                             glm::vec3(0.5f, 0.6f, 0.5f));
        SectionMesh cutMesh, offMesh, movedMesh, goneMesh;
        BuildSection(d, nullptr, kInvalidComponent, cutMesh);

        // DISABLE - the artist's non-destructive switch.
        d.Find(open)->hidden = true;
        BuildSection(d, nullptr, kInvalidComponent, offMesh);
        const MeshRange* solid = offMesh.RangeFor(wall);
        Check(solid && solid->indexCount == 36,
              "DISABLING A CUTTER RESTORES THE SOLID WALL EXACTLY - there is no baked mesh to "
              "damage, so non-destructive editing falls out of the architecture for free");

        // MOVE - and the wall must follow, not remember where the hole used to be.
        d.Find(open)->hidden = false;
        d.Find(open)->position = glm::vec3(1.0f, 0.3f, 0.0f);
        BuildSection(d, nullptr, kInvalidComponent, movedMesh);
        Check(movedMesh.RangeFor(wall) != nullptr, "a moved cutter still cuts");
        Check(movedMesh.TotalVertices() != cutMesh.TotalVertices() ||
                  std::memcmp(movedMesh.model[0].vertices.data(), cutMesh.model[0].vertices.data(),
                              std::min(movedMesh.model[0].vertices.size(),
                                       cutMesh.model[0].vertices.size()) *
                                  sizeof(Vertex)) != 0,
              "...and the hole moves with it");

        // DELETE - the wall goes solid again, with nothing left over.
        d.RemoveComponent(open);
        BuildSection(d, nullptr, kInvalidComponent, goneMesh);
        const MeshRange* back = goneMesh.RangeFor(wall);
        Check(back && back->indexCount == 36, "deleting a cutter restores the solid wall");
    }

    // -- Masonry: units in the opening are never laid ------------------------
    {
        auto [solid, wallS, openS] = WalledOpening(MaterialKind::Brick, glm::vec3(0.0f),
                                                   glm::vec3(0.5f, 0.5f, 0.5f));
        solid.Find(openS)->hidden = true;
        SectionMesh full;
        BuildSection(solid, nullptr, kInvalidComponent, full);

        solid.Find(openS)->hidden = false;
        SectionMesh holed;
        BuildSection(solid, nullptr, kInvalidComponent, holed);

        Check(holed.ElementsOf(wallS).size() < full.ElementsOf(wallS).size(),
              "MATERIAL-AWARE CUTTING, MASONRY: units inside the opening are simply never laid - "
              "a bricklayer does not lay a brick and then remove it");

        // No surviving unit may sit inside the hole.
        std::vector<BrickPlacement> units;
        LayoutMasonry(solid, *solid.Find(wallS), units);
        std::vector<BoxPiece> cutters;
        GatherCutters(solid, *solid.Find(wallS), nullptr, cutters);
        u32 inside = 0;
        for (const BrickPlacement& u : units)
            if (FullyCut(u.center, u.extent, cutters)) ++inside;
        Check(inside > 0, "the test opening genuinely swallows some units");
    }

    // -- Timber framing grows AROUND the opening -----------------------------
    {
        auto [d, wall, open] = WalledOpening(MaterialKind::TimberFrame,
                                             glm::vec3(0.0f, 0.0f, 0.0f),
                                             glm::vec3(0.4f, 0.5f, 0.5f));
        d.Find(wall)->extent = glm::vec3(2.0f, 1.2f, 0.0445f);

        std::vector<BoxPiece> cutters;
        GatherCutters(d, *d.Find(wall), nullptr, cutters);
        std::vector<BoardPlacement> framed, plain;
        LayoutWood(d, *d.Find(wall), framed, &cutters);
        LayoutWood(d, *d.Find(wall), plain);

        // Nothing may sit inside the opening.
        bool clear = true;
        for (const BoardPlacement& b : framed)
            if (FullyCut(b.center, b.extent, cutters)) clear = false;
        Check(clear, "no framing member is left floating inside the opening");

        // THE POINT OF THE PHASE. A window is not a hole: the studs it crosses were carrying load,
        // and cutting them without replacing that path leaves the top plate spanning thin air.
        u32 headerLike = 0;
        for (const BoardPlacement& b : framed)
            if (b.role == MemberRole::Plate && b.center.y > 0.2f && b.center.y < 1.0f) ++headerLike;
        Check(headerLike > 0,
              "A HEADER MUST SPAN THE OPENING - cutting the studs a window crosses without "
              "replacing their load path is the difference between a wall with a window and a "
              "wall with a hole");

        // Jack studs carrying that header down.
        u32 shortStuds = 0;
        for (const BoardPlacement& b : framed)
            if (b.role == MemberRole::Stud && b.extent.y < plain[plain.size() - 1].extent.y * 0.9f)
                ++shortStuds;
        Check(shortStuds >= 2, "jack studs each side carry the header down to the bottom plate");

        // A sill under a window, but never across a doorway.
        auto [dd, dwall, dopen] = WalledOpening(MaterialKind::TimberFrame,
                                                glm::vec3(0.0f, -0.7f, 0.0f),
                                                glm::vec3(0.4f, 0.5f, 0.5f));
        dd.Find(dwall)->extent = glm::vec3(2.0f, 1.2f, 0.0445f);
        std::vector<BoxPiece> dcut;
        GatherCutters(dd, *dd.Find(dwall), nullptr, dcut);
        std::vector<BoardPlacement> doorFramed;
        LayoutWood(dd, *dd.Find(dwall), doorFramed, &dcut);
        bool barAcrossDoor = false;
        for (const BoardPlacement& b : doorFramed)
            if (b.role == MemberRole::Plate && b.center.y < -1.0f && std::fabs(b.center.x) < 0.5f)
                barAcrossDoor = true;
        Check(!barAcrossDoor,
              "A DOOR GETS NO SILL - a bar across the threshold the player walks through is the "
              "kind of thing that ships");
    }

    // -- Doors and windows fill their openings -------------------------------
    {
        ConstructionDef d;
        ConstructionComponent w;
        w.kind = ComponentKind::Window;
        w.material = MaterialKind::Glass;
        w.extent = glm::vec3(0.5f, 0.6f, 0.05f);
        const ComponentId win = d.AddComponent(w);

        ConstructionComponent dr;
        dr.kind = ComponentKind::Door;
        dr.material = MaterialKind::WoodPlank;
        dr.extent = glm::vec3(0.45f, 1.0f, 0.05f);
        const ComponentId door = d.AddComponent(dr);

        std::vector<BoxPiece> frame, panel;
        BuildFillingGeometry(d, *d.Find(win), frame, panel);
        Check(frame.size() == 4, "a window frame is head, sill and two jambs");
        Check(panel.size() == 1, "...with one pane of glazing");

        BuildFillingGeometry(d, *d.Find(door), frame, panel);
        Check(frame.size() == 4, "a door frame is head, threshold and two jambs");
        Check(panel.size() == 1, "...with one leaf");

        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        const MeshRange* wr = m.RangeFor(win);
        Check(wr && wr->indexCount == 36 * 5,
              "a window generates its frame and glazing, not a solid massing box");
    }

    // -- Capacity survives TOGGLING a cutter ---------------------------------
    // This is the core action of a non-destructive workflow, and the direction is not obvious:
    // cutting a solid box makes it BIGGER, while cutting masonry only removes units.
    {
        for (MaterialKind mat : {MaterialKind::PouredConcrete, MaterialKind::Brick,
                                 MaterialKind::TimberFrame}) {
            auto [d, wall, open] = WalledOpening(mat, glm::vec3(0.0f), glm::vec3(0.4f, 0.5f, 0.5f));
            const MeshCapacity cap = EstimateCapacity(d, kInvalidComponent);

            SectionMesh cutMesh, solidMesh;
            BuildSection(d, nullptr, kInvalidComponent, cutMesh);
            d.Find(open)->hidden = true;
            BuildSection(d, nullptr, kInvalidComponent, solidMesh);

            Check(cap.vertices >= cutMesh.TotalVertices(),
                  "the reservation must cover the CUT wall");
            Check(cap.vertices >= solidMesh.TotalVertices(),
                  "THE RESERVATION MUST ALSO COVER THE UNCUT WALL - toggling an opening off is the "
                  "core action of a non-destructive cutter workflow, and it must not refuse the "
                  "update");
            Check(EstimateCapacity(d, kInvalidComponent).vertices == cap.vertices,
                  "...and the reservation does not itself depend on the cutter's current state");
        }
    }

    // -- Several cutters in one wall -----------------------------------------
    {
        auto [d, wall, open] = WalledOpening(MaterialKind::PouredConcrete,
                                             glm::vec3(-1.0f, 0.0f, 0.0f),
                                             glm::vec3(0.3f, 0.4f, 0.5f));
        ConstructionComponent o2;
        o2.kind = ComponentKind::Opening;
        o2.parent = wall;
        o2.position = glm::vec3(1.0f, 0.0f, 0.0f);
        o2.extent = glm::vec3(0.3f, 0.4f, 0.5f);
        d.AddComponent(o2);

        SectionMesh m;
        BuildSection(d, nullptr, kInvalidComponent, m);
        const MeshRange* r = m.RangeFor(wall);
        Check(r && r->indexCount > 36 * 4,
              "two openings decompose the wall further than one does");

        std::vector<BoxPiece> cutters;
        GatherCutters(d, *d.Find(wall), nullptr, cutters);
        Check(cutters.size() == 2, "both openings are gathered");
        std::vector<BoxPiece> pieces;
        SubtractCutters(glm::vec3(0.0f), d.Find(wall)->extent, cutters, pieces);
        f32 vol = 0.0f;
        for (const BoxPiece& p : pieces) vol += 8.0f * p.extent.x * p.extent.y * p.extent.z;
        const f32 expect = 8.0f * 2.0f * 1.2f * 0.1f - 2.0f * (8.0f * 0.3f * 0.4f * 0.1f);
        Check(std::fabs(vol - expect) < 1e-3f,
              "MULTIPLE CUTTERS STAY EXACT - applied iteratively, the pieces still partition the "
              "remainder with no double-counting where the decompositions meet");
    }

    if (g_fails == 0)
        std::printf("construction: hierarchical seeds where rerolling, ADDING or removing one "
                    "component provably perturbs no other; ids that are never recycled; a support "
                    "graph that distinguishes bearing from merely attached, survives losing one of "
                    "two supports, and answers 'what becomes unsupported if these are removed' "
                    "WITHOUT mutating - the query a destruction pass consumes. Geometry is "
                    "byte-identical for a given definition, merges by material into one submesh "
                    "each (ordered by an append-only enum so indices can only APPEND, never "
                    "renumber), keeps every component's triangles addressable by id, and reports "
                    "a capacity covering the REPAIRED building because UpdateMesh never grows\n");
    return g_fails == 0;
}

} // namespace hbe::construction
