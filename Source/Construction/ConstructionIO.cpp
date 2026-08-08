#include "Construction/ConstructionIO.h"

#include "Assets/VFS.h"
#include "Construction/ConstructionChunk.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace hbe::construction {

namespace {

using json = nlohmann::json;

// Bumped only when the format changes in a way a reader must know about. Readers accept anything
// at or below their own version and fill missing fields from defaults, so adding a parameter is
// never a format break - which matters because parameters are exactly the thing that keeps growing.
constexpr u32 kVersion = 1;

json V3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
glm::vec3 ReadV3(const json& j, const glm::vec3& fallback) {
    if (!j.is_array() || j.size() != 3) return fallback;
    return glm::vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
}
json V4(const glm::quat& q) { return json::array({q.x, q.y, q.z, q.w}); }
glm::quat ReadQuat(const json& j) {
    if (!j.is_array() || j.size() != 4) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::quat(j[3].get<f32>(), j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
}

template <typename T>
T Get(const json& j, const char* key, T fallback) {
    if (!j.is_object()) return fallback;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    // Never throws on a wrong type: a hand-edited or newer file must degrade to the default rather
    // than abort the whole load and lose the artist's building.
    try {
        return it->get<T>();
    } catch (...) {
        return fallback;
    }
}

json WriteMasonry(const MasonryParams& m) {
    return json{{"unitLength", m.unitLength}, {"unitHeight", m.unitHeight},
                {"unitDepth", m.unitDepth},   {"joint", m.joint},
                {"bond", static_cast<int>(m.bond)}, {"sizeJitter", m.sizeJitter},
                {"depthJitter", m.depthJitter}, {"rotJitter", m.rotJitter},
                {"mortar", m.generateMortar}, {"maxUnits", m.maxUnits}};
}
void ReadMasonry(const json& j, MasonryParams& m) {
    const MasonryParams d;
    m.unitLength = Get(j, "unitLength", d.unitLength);
    m.unitHeight = Get(j, "unitHeight", d.unitHeight);
    m.unitDepth = Get(j, "unitDepth", d.unitDepth);
    m.joint = Get(j, "joint", d.joint);
    m.bond = static_cast<BondPattern>(Get(j, "bond", static_cast<int>(d.bond)));
    m.sizeJitter = Get(j, "sizeJitter", d.sizeJitter);
    m.depthJitter = Get(j, "depthJitter", d.depthJitter);
    m.rotJitter = Get(j, "rotJitter", d.rotJitter);
    m.generateMortar = Get(j, "mortar", d.generateMortar);
    m.maxUnits = Get(j, "maxUnits", d.maxUnits);
}

json WriteTimber(const TimberParams& t) {
    return json{{"memberWidth", t.memberWidth}, {"memberDepth", t.memberDepth},
                {"spacing", t.spacing},         {"topPlates", t.topPlates},
                {"bottomPlate", t.bottomPlate}, {"warp", t.warp},
                {"maxMembers", t.maxMembers}};
}
void ReadTimber(const json& j, TimberParams& t) {
    const TimberParams d;
    t.memberWidth = Get(j, "memberWidth", d.memberWidth);
    t.memberDepth = Get(j, "memberDepth", d.memberDepth);
    t.spacing = Get(j, "spacing", d.spacing);
    t.topPlates = Get(j, "topPlates", d.topPlates);
    t.bottomPlate = Get(j, "bottomPlate", d.bottomPlate);
    t.warp = Get(j, "warp", d.warp);
    t.maxMembers = Get(j, "maxMembers", d.maxMembers);
}

json WritePlank(const PlankParams& p) {
    return json{{"boardWidth", p.boardWidth}, {"boardThickness", p.boardThickness},
                {"gap", p.gap},               {"direction", static_cast<int>(p.direction)},
                {"profile", static_cast<int>(p.profile)}, {"overlap", p.overlap},
                {"battenWidth", p.battenWidth}, {"lengthJitter", p.lengthJitter},
                {"warp", p.warp}, {"maxBoards", p.maxBoards}};
}
void ReadPlank(const json& j, PlankParams& p) {
    const PlankParams d;
    p.boardWidth = Get(j, "boardWidth", d.boardWidth);
    p.boardThickness = Get(j, "boardThickness", d.boardThickness);
    p.gap = Get(j, "gap", d.gap);
    p.direction = static_cast<BoardDirection>(Get(j, "direction", static_cast<int>(d.direction)));
    p.profile = static_cast<SidingProfile>(Get(j, "profile", static_cast<int>(d.profile)));
    p.overlap = Get(j, "overlap", d.overlap);
    p.battenWidth = Get(j, "battenWidth", d.battenWidth);
    p.lengthJitter = Get(j, "lengthJitter", d.lengthJitter);
    p.warp = Get(j, "warp", d.warp);
    p.maxBoards = Get(j, "maxBoards", d.maxBoards);
}

json WriteShingle(const ShingleParams& s) {
    return json{{"width", s.width},         {"length", s.length},
                {"exposure", s.exposure},   {"thickness", s.thickness},
                {"jitter", s.jitter},       {"maxShingles", s.maxShingles}};
}
void ReadShingle(const json& j, ShingleParams& s) {
    const ShingleParams d;
    s.width = Get(j, "width", d.width);
    s.length = Get(j, "length", d.length);
    s.exposure = Get(j, "exposure", d.exposure);
    s.thickness = Get(j, "thickness", d.thickness);
    s.jitter = Get(j, "jitter", d.jitter);
    s.maxShingles = Get(j, "maxShingles", d.maxShingles);
}

json WriteParams(const PresetParams& p) {
    return json{{"width", p.width},
                {"depth", p.depth},
                {"height", p.height},
                {"thickness", p.thickness},
                {"floorCount", p.floorCount},
                {"floorHeight", p.floorHeight},
                {"structureMaterial", p.structureMaterial},
                {"exteriorMaterial", p.exteriorMaterial},
                {"roofMaterial", p.roofMaterial},
                {"roofPitch", p.roofPitch},
                {"roofOverhang", p.roofOverhang},
                {"windowCount", p.windowCount},
                {"windowWidth", p.windowWidth},
                {"windowHeight", p.windowHeight},
                {"windowSill", p.windowSill},
                {"doorCount", p.doorCount},
                {"doorWidth", p.doorWidth},
                {"doorHeight", p.doorHeight},
                {"seed", p.seed},
                {"masonry", WriteMasonry(p.masonry)},
                {"timber", WriteTimber(p.timber)},
                {"plank", WritePlank(p.plank)},
                {"shingle", WriteShingle(p.shingle)}};
}

void ReadParams(const json& j, PresetParams& p) {
    const PresetParams d;
    p.width = Get(j, "width", d.width);
    p.depth = Get(j, "depth", d.depth);
    p.height = Get(j, "height", d.height);
    p.thickness = Get(j, "thickness", d.thickness);
    p.floorCount = Get(j, "floorCount", d.floorCount);
    p.floorHeight = Get(j, "floorHeight", d.floorHeight);
    p.structureMaterial = Get(j, "structureMaterial", d.structureMaterial);
    p.exteriorMaterial = Get(j, "exteriorMaterial", d.exteriorMaterial);
    p.roofMaterial = Get(j, "roofMaterial", d.roofMaterial);
    p.roofPitch = Get(j, "roofPitch", d.roofPitch);
    p.roofOverhang = Get(j, "roofOverhang", d.roofOverhang);
    p.windowCount = Get(j, "windowCount", d.windowCount);
    p.windowWidth = Get(j, "windowWidth", d.windowWidth);
    p.windowHeight = Get(j, "windowHeight", d.windowHeight);
    p.windowSill = Get(j, "windowSill", d.windowSill);
    p.doorCount = Get(j, "doorCount", d.doorCount);
    p.doorWidth = Get(j, "doorWidth", d.doorWidth);
    p.doorHeight = Get(j, "doorHeight", d.doorHeight);
    p.seed = Get(j, "seed", d.seed);
    if (j.contains("masonry")) ReadMasonry(j["masonry"], p.masonry);
    if (j.contains("timber")) ReadTimber(j["timber"], p.timber);
    if (j.contains("plank")) ReadPlank(j["plank"], p.plank);
    if (j.contains("shingle")) ReadShingle(j["shingle"], p.shingle);
}

json WriteDef(const ConstructionDef& d) {
    json comps = json::array();
    for (const ConstructionComponent& c : d.components) {
        json jc{{"id", c.id},
                {"kind", static_cast<int>(c.kind)},
                {"role", static_cast<int>(c.role)},
                {"material", static_cast<int>(c.material)},
                {"parent", c.parent},
                {"pos", V3(c.position)},
                {"rot", V4(c.rotation)},
                {"extent", V3(c.extent)},
                {"salt", c.seedSalt}};
        // Only written when set. An override flag on every component would quadruple the file for
        // no information - and these are exactly the fields an artist sets on a handful of pieces.
        if (c.subtract) jc["subtract"] = true;
        if (c.locked) jc["locked"] = true;
        if (c.hidden) jc["hidden"] = true;
        if (!c.name.empty()) jc["name"] = c.name;
        // Method blocks are only meaningful for the matching material, so writing all four on
        // every component would dominate the file. Written when the component could read one.
        if (IsMasonry(c.material)) jc["masonry"] = WriteMasonry(c.masonry);
        if (c.material == MaterialKind::TimberFrame) jc["timber"] = WriteTimber(c.timber);
        if (IsPlankMaterial(c.material)) jc["plank"] = WritePlank(c.plank);
        if (c.material == MaterialKind::WoodShingle) jc["shingle"] = WriteShingle(c.shingle);
        comps.push_back(std::move(jc));
    }

    json edges = json::array();
    for (const SupportEdge& e : d.edges)
        edges.push_back(json{{"supported", e.supported},
                             {"supporter", e.supporter},
                             {"kind", static_cast<int>(e.kind)},
                             {"capacity", e.capacity}});

    return json{{"seed", d.seed}, {"nextId", d.nextId}, {"components", comps}, {"edges", edges}};
}

void ReadDef(const json& j, ConstructionDef& d) {
    d = ConstructionDef{};
    d.seed = Get(j, "seed", u64{0});
    d.nextId = Get(j, "nextId", ComponentId{1});

    if (j.contains("components") && j["components"].is_array()) {
        for (const json& jc : j["components"]) {
            ConstructionComponent c;
            c.id = Get(jc, "id", ComponentId{0});
            c.kind = static_cast<ComponentKind>(Get(jc, "kind", 0));
            c.role = static_cast<StructuralRole>(Get(jc, "role", 0));
            c.material = static_cast<MaterialKind>(Get(jc, "material", 0));
            c.parent = Get(jc, "parent", ComponentId{0});
            c.position = ReadV3(jc.value("pos", json()), glm::vec3(0.0f));
            c.rotation = ReadQuat(jc.value("rot", json()));
            c.extent = ReadV3(jc.value("extent", json()), glm::vec3(1.0f));
            c.seedSalt = Get(jc, "salt", u64{0});
            c.subtract = Get(jc, "subtract", false);
            c.locked = Get(jc, "locked", false);
            c.hidden = Get(jc, "hidden", false);
            c.name = Get(jc, "name", std::string());
            if (jc.contains("masonry")) ReadMasonry(jc["masonry"], c.masonry);
            if (jc.contains("timber")) ReadTimber(jc["timber"], c.timber);
            if (jc.contains("plank")) ReadPlank(jc["plank"], c.plank);
            if (jc.contains("shingle")) ReadShingle(jc["shingle"], c.shingle);
            // PUSHED DIRECTLY, not through AddComponent - the id is authored data and must survive
            // the round trip. Minting a fresh one here would silently repoint every damage record.
            d.components.push_back(std::move(c));
        }
    }
    if (j.contains("edges") && j["edges"].is_array()) {
        for (const json& je : j["edges"]) {
            SupportEdge e;
            e.supported = Get(je, "supported", ComponentId{0});
            e.supporter = Get(je, "supporter", ComponentId{0});
            e.kind = static_cast<EdgeKind>(Get(je, "kind", 0));
            e.capacity = Get(je, "capacity", 1.0f);
            d.edges.push_back(e);
        }
    }

    // A file whose nextId is behind its own components would hand out a COLLIDING id on the next
    // add. Repair rather than refuse: the building is still perfectly usable.
    for (const ConstructionComponent& c : d.components)
        if (c.id >= d.nextId) d.nextId = c.id + 1;
}

} // namespace

std::string BuildToString(const BuildAsset& a) {
    json damage = json::array();
    for (ComponentId id : a.damage.destroyed) damage.push_back(id);
    json broken = json::array();
    for (const DamageState::EdgeBreak& b : a.damage.brokenEdges)
        broken.push_back(json::array({b.supported, b.supporter}));

    const json root{{"version", kVersion},
                    {"preset", a.presetId},
                    {"chunkSize", a.chunkSize},
                    {"params", WriteParams(a.params)},
                    {"def", WriteDef(a.def)},
                    {"damage", json{{"destroyed", damage}, {"brokenEdges", broken}}}};
    return root.dump(2);
}

bool BuildFromString(const std::string& text, BuildAsset& out, std::string& outError) {
    outError.clear();
    out = BuildAsset{};
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        outError = "not valid JSON";
        return false;
    }
    const u32 version = Get(root, "version", u32{0});
    if (version > kVersion) {
        // Forward compatibility is a REPORT, not a refusal. Unknown fields already fall back to
        // defaults, so the building loads; the artist just needs to know it may be missing
        // something a newer editor wrote.
        outError = "written by a newer editor (version " + std::to_string(version) +
                   "); unknown settings use defaults";
    }
    out.presetId = Get(root, "preset", std::string("wall"));
    out.chunkSize = Get(root, "chunkSize", 4.0f);
    if (root.contains("params")) ReadParams(root["params"], out.params);
    if (root.contains("def")) ReadDef(root["def"], out.def);

    if (root.contains("damage")) {
        const json& jd = root["damage"];
        if (jd.contains("destroyed") && jd["destroyed"].is_array())
            for (const json& id : jd["destroyed"]) out.damage.Destroy(id.get<ComponentId>());
        if (jd.contains("brokenEdges") && jd["brokenEdges"].is_array())
            for (const json& b : jd["brokenEdges"])
                if (b.is_array() && b.size() == 2)
                    out.damage.BreakEdge(b[0].get<ComponentId>(), b[1].get<ComponentId>());
    }

    // Clamp on load. A hand-edited or newer file can carry anything, and an out-of-range enum
    // would index a name table out of bounds the first time the inspector drew it.
    if (const PresetDesc* d = FindPreset(out.presetId.c_str()))
        ClampAll(out.params, d->params, d->paramCount);
    return true;
}

bool SaveBuild(const std::filesystem::path& file, const BuildAsset& a, std::string& outError) {
    outError.clear();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream o(file, std::ios::binary);
    if (!o) {
        outError = "could not open '" + file.string() + "' for writing";
        return false;
    }
    o << BuildToString(a);
    if (!o.good()) {
        outError = "write failed";
        return false;
    }
    return true;
}

bool LoadBuild(const std::filesystem::path& file, BuildAsset& out, std::string& outError) {
    outError.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        outError = "could not read '" + file.string() + "'";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return BuildFromString(text, out, outError);
}

bool LoadBuildVfs(const std::string& relPath, BuildAsset& out, std::string& outError) {
    outError.clear();
    // VFS, NEVER std::ifstream. A shipped build serves assets from mounted .uap packs, not from
    // loose files - `.hbgi`, `.hbuianim` and `.hbworld` all shipped broken for exactly this.
    const auto bytes = vfs::ReadFile(relPath);
    if (!bytes) {
        outError = "could not read '" + relPath + "' from the VFS";
        return false;
    }
    return BuildFromString(std::string(bytes->begin(), bytes->end()), out, outError);
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("buildio FAIL: %s\n", what);
        ++g_fails;
    }
}
} // namespace

bool IoSelfTest() {
    g_fails = 0;

    // Round-trip a real, fully populated house.
    BuildAsset a;
    a.presetId = "house";
    a.params = DefaultParams("house");
    a.params.floorCount = 2;
    a.params.width = 14.5f;
    a.params.exteriorMaterial = static_cast<i32>(MaterialKind::Brick);
    a.params.masonry.bond = BondPattern::Flemish;
    a.params.timber.spacing = 0.6f;
    a.params.seed = 0xABCDEF12ull;
    a.chunkSize = 6.0f;
    BuildPreset(a.presetId.c_str(), a.params, a.def);

    // A manual override, which is the whole reason the DEFINITION is stored alongside the
    // parameters rather than instead of them.
    Check(!a.def.components.empty(), "the fixture builds components");
    a.def.components[1].locked = true;
    a.def.components[1].name = "load-bearing front wall";
    a.def.components[1].position.x += 0.75f;
    a.damage.Destroy(a.def.components[2].id);

    const std::string text = BuildToString(a);
    BuildAsset b;
    std::string err;
    Check(BuildFromString(text, b, err), "a written asset reads back");
    Check(err.empty(), "...with no error");

    Check(b.presetId == a.presetId, "the preset id round-trips");
    Check(b.chunkSize == a.chunkSize, "the chunk size round-trips");
    Check(b.params.floorCount == 2 && std::fabs(b.params.width - 14.5f) < 1e-4f,
          "parameters round-trip");
    Check(b.params.masonry.bond == BondPattern::Flemish, "a method-block enum round-trips");
    Check(std::fabs(b.params.timber.spacing - 0.6f) < 1e-4f, "a method-block float round-trips");
    Check(b.params.seed == 0xABCDEF12ull, "THE SEED ROUND-TRIPS AT FULL 64-BIT WIDTH - a truncated "
                                          "seed would silently regenerate a different building");

    Check(b.def.components.size() == a.def.components.size(), "every component round-trips");
    Check(b.def.edges.size() == a.def.edges.size(), "every support edge round-trips");
    Check(b.def.nextId == a.def.nextId, "the id mint counter round-trips");

    // IDS ARE AUTHORED DATA. If a load minted fresh ones, every damage record in the file would
    // silently name a different component.
    bool idsMatch = b.def.components.size() == a.def.components.size();
    for (usize i = 0; idsMatch && i < a.def.components.size(); ++i)
        idsMatch = a.def.components[i].id == b.def.components[i].id;
    Check(idsMatch,
          "COMPONENT IDS SURVIVE THE ROUND TRIP - minting fresh ones on load would repoint every "
          "damage record at a different component");

    Check(b.def.components[1].locked, "a manual override survives the round trip");
    Check(b.def.components[1].name == "load-bearing front wall", "...including its name");
    Check(std::fabs(b.def.components[1].position.x - a.def.components[1].position.x) < 1e-4f,
          "...and its moved position, which is exactly why the DEFINITION is stored and not just "
          "the parameters");
    Check(b.damage.destroyed.size() == 1, "persistent destruction round-trips");
    Check(b.damage.IsDestroyed(a.def.components[2].id), "...naming the same component");

    // The loaded definition must generate the SAME geometry.
    {
        SectionMesh ma, mb;
        BuildSection(a.def, &a.damage, kInvalidComponent, ma);
        BuildSection(b.def, &b.damage, kInvalidComponent, mb);
        Check(ma.TotalIndices() == mb.TotalIndices(),
              "A LOADED BUILDING GENERATES IDENTICAL GEOMETRY - anything less means the file is "
              "not actually the source of truth");
        Check(ma.TotalVertices() == mb.TotalVertices(), "...vertex for vertex");
    }

    // Malformed input must fail cleanly, never throw and never half-load.
    {
        BuildAsset bad;
        std::string e2;
        Check(!BuildFromString("{ this is not json", bad, e2), "malformed JSON is refused");
        Check(!e2.empty(), "...with a reason");
        Check(!BuildFromString("[]", bad, e2), "a non-object root is refused");

        // A file with wrong TYPES must degrade to defaults rather than abort - losing an artist's
        // building because one field was hand-edited to a string is not an acceptable outcome.
        Check(BuildFromString(R"({"version":1,"preset":"wall","params":{"width":"oops"}})", bad, e2),
              "a wrong-typed field falls back to its default rather than failing the load");
        Check(bad.params.width == PresetParams{}.width, "...to the DEFAULT value");
    }

    // A future version loads with a warning, not a refusal.
    {
        BuildAsset fut;
        std::string e3;
        Check(BuildFromString(R"({"version":9999,"preset":"wall"})", fut, e3),
              "a newer file still loads");
        Check(!e3.empty(), "...but says it was written by a newer editor");
    }

    // A file whose nextId trails its components would hand out a colliding id on the next add.
    {
        BuildAsset repaired;
        std::string e4;
        BuildFromString(
            R"({"version":1,"preset":"wall","def":{"nextId":1,"components":[{"id":77,"kind":5}]}})",
            repaired, e4);
        Check(repaired.def.nextId > 77,
              "A TRAILING nextId IS REPAIRED ON LOAD - otherwise the next AddComponent would mint "
              "an id that already exists and two components would share an identity");
    }

    // Disk round-trip.
    {
        const std::filesystem::path f =
            std::filesystem::temp_directory_path() / "hbe_build_selftest.hbbuild";
        std::string e5;
        Check(SaveBuild(f, a, e5), "an asset saves to disk");
        BuildAsset onDisk;
        Check(LoadBuild(f, onDisk, e5), "...and loads back");
        Check(onDisk.def.components.size() == a.def.components.size(), "...intact");
        std::error_code ec;
        std::filesystem::remove(f, ec);
        Check(!LoadBuild(f, onDisk, e5), "a missing file fails cleanly");
    }

    if (g_fails == 0)
        std::printf(".hbbuild: stores the PRESET+PARAMS *and* the graph, so manual overrides "
                    "survive while the dials keep working; ids and the 64-bit seed round-trip "
                    "exactly (a loaded building generates identical geometry); a trailing nextId "
                    "is repaired; and wrong types degrade to defaults rather than losing the "
                    "building\n");
    return g_fails == 0;
}

} // namespace hbe::construction
