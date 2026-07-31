// Assets/AssetRefsTest.cpp - --test-packclosure.
//
// THE GATE for "Pack only referenced assets". It builds a synthetic project on
// disk that exercises every format in the engine's reference matrix, each one
// referencing the next, then proves four things the old walk got wrong and
// nobody could have noticed:
//
//   1. TOTALITY. Every format the closure can reach is reached - including the
//      ones that were never followed at all (.hbprefab, .hbschem, .hbchar,
//      .hbfrac, .hbmusic stems, .hbcutscene -> .hbdialogue -> voiceline,
//      .hbuianim, .hbgi, .hbprobe, terrain splat materials, particle sprites,
//      animator/motion-matching clips, world-text fonts, .hbmat thickness).
//   2. TRANSITIVITY. An asset referenced ONLY through a chain - a texture named
//      by a .hbmat named by a .hbprefab named by a .hbscene - is included. The
//      old walk was a single flat pass, so nothing it discovered was re-opened.
//   3. THE FILTER ACTUALLY FILTERS, AND ITS STRINGS MATCH. The project is really
//      cooked, both with and without the filter, and the resulting packs are
//      opened: an orphan is absent when the filter is on and present when it is
//      off. Cooking for real is the only way to catch the normalisation hazard -
//      uap::WritePacks matches its filter by EXACT STRING against the pack key,
//      so a reference authored as a bare filename must have been canonicalised
//      to "Textures/x.uaf" or the asset silently drops out.
//   4. EVERY REFERENCE OF EVERY PACKED ASSET RESOLVES INSIDE THE PACKS. The
//      closure is re-walked over the cooked pack set itself. Nothing in the tree
//      did this before: the two existing verify blocks read back entries.front()
//      and stop.
//
// Plus the failure policy: a reference naming no file, and an ambiguous bare
// filename, are both REPORTED (closure.ok == false) rather than silently packed
// around - and a clean project reports neither.
//
// Headless: no GPU, no window, no Project. Same contract as --test-seamweld.
//
// THIS TEST USED TO CRASH IN DEBUG, and the recorded reason was wrong. Step 6
// feeds the walk a deliberately malformed `.hbscene`; in Debug that aborted with
// _CrtIsValidHeapPointer, which was written up as "the vendored nlohmann/json
// 3.11.3 corrupts the heap on any malformed input". It does not. The actual cause
// was an ODR violation on std::exception across the whole build: JoltPhysics
// pushes `_HAS_EXCEPTIONS=0` PUBLIC unless CPP_EXCEPTIONS_ENABLED is ON, the
// engine links Jolt PRIVATE, so `hbe`/`hbe_editor` compiled with the 16-byte
// no-exceptions std::exception while the executables compiled with the 24-byte
// normal one - and every throw/catch inside an engine TU then freed a pointer the
// CRT had never allocated. Measured directly (sizeof printed from both sides of
// the boundary), not inferred. cmake/Dependencies.cmake forces the flag ON and
// Core/Types.h #errors if it ever returns, so step 6 now passes in all three
// configurations. DO NOT delete step 6 - a project full of malformed JSON is
// exactly what the closure exists to survive.
#include "Assets/AssetRefs.h"

#include "Assets/AssetFormats.h"
#include "Assets/AudioEvent.h"
#include "Assets/CharacterAsset.h"
#include "Assets/CutsceneAsset.h"
#include "Assets/DialogueAsset.h"
#include "Assets/Fracture.h"
#include "Assets/MaterialAsset.h"
#include "Assets/MusicGraph.h"
#include "Assets/UAF.h"
#include "Assets/UAP.h"
#include "Assets/VFS.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace hbe::assets {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

u32 g_failures = 0;

bool Check(bool cond, const std::string& what) {
    if (!cond) {
        HBE_ERROR("packclosure: FAIL - {}", what);
        ++g_failures;
    }
    return cond;
}

// --- Synthetic-asset writers ------------------------------------------------

void WriteJson(const fs::path& path, const json& j) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    out << j.dump(2);
}

// A binary stub for the three LEAF formats. Their content is irrelevant to the
// closure by declaration (RefScan::Leaf), which is exactly what this asserts:
// they must be PACKED without being PARSED.
void WriteLeafStub(const fs::path& path, u32 magic) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    const u32 ver = 1;
    out.write(reinterpret_cast<const char*>(&magic), 4);
    out.write(reinterpret_cast<const char*>(&ver), 4);
    const char pad[64] = {};
    out.write(pad, sizeof(pad));
}

void WriteTex(const fs::path& path) {
    uaf::Texture t;
    t.width = t.height = 2;
    t.format = 1;
    t.mipCount = 1;
    t.pixels.assign(2 * 2 * 4, 0xFFu);
    uaf::WriteTexture(path, t, 0);
}

void WriteAudioClip(const fs::path& path) {
    uaf::Audio a;
    a.channels = 1;
    a.sampleRate = 8000;
    a.bitsPerSample = 16;
    a.pcm.assign(64, 0);
    uaf::WriteAudio(path, a, 0);
}

void WriteFontFile(const fs::path& path) {
    uaf::WriteFont(path, std::vector<u8>(32, 0x41u), 0);
}

// A one-submesh mesh whose material carries texture refs and (the field both
// previous add-sites dropped) a generated `.hbmat`.
void WriteMeshAsset(const fs::path& path, const std::string& baseColorTex,
                    const std::string& materialAsset) {
    Model model;
    MeshData md;
    md.name = "part";
    md.vertices.resize(3);
    md.indices = {0, 1, 2};
    md.material.name = "m";
    md.material.baseColorTex = baseColorTex;
    md.material.materialAsset = materialAsset;
    model.push_back(std::move(md));
    uaf::WriteMesh(path, model, 0, nullptr);
}

void WriteMat(const fs::path& path, const std::string& albedo,
              const std::string& thickness = std::string()) {
    MaterialAsset m;
    m.name = path.stem().string();
    m.albedoTex = albedo;
    m.thicknessTex = thickness;
    SaveMaterial(path, m);
}

// --- The synthetic project --------------------------------------------------

// Every asset the closure must reach, keyed by pack key, with the chain that
// reaches it. Kept as data so the assertions read as the reference matrix.
struct Expect {
    const char* key;
    const char* via;
};

const std::vector<Expect> kExpected = {
    // Roots (swept entry points + project settings).
    {"Scenes/main.hbscene",        "root sweep"},
    {"Prefabs/spawn.hbprefab",     "root sweep + scene spawner.prefab"},
    {"UI/menu.hbui",               "root sweep + ProjectSettings.uiDocuments[0]"},
    {"Music/score.hbmusic",        "ProjectSettings.musicGraph"},
    {"Textures/icon_general.uaf",  "ProjectSettings.inputIcons.general"},
    {"Textures/icon_key_e.uaf",    "ProjectSettings.inputIcons.keyboard"},
    // Scene -> mesh -> its material textures AND its generated .hbmat.
    {"Models/hero.uaf",            "scene mesh.source (uaf:...#0)"},
    {"Textures/hero_albedo.uaf",   "mesh material baseColorTex"},
    {"Materials/hero.hbmat",       "mesh material materialAsset (was DROPPED)"},
    {"Textures/hero_thick.uaf",    ".hbmat thicknessTex (was DROPPED)"},
    // Scene -> .hbmat directly.
    {"Materials/scene.hbmat",      "scene mesh.material"},
    {"Textures/scene_albedo.uaf",  ".hbmat albedo"},
    // Bare-filename reference: must normalise to the real pack key.
    {"Textures/bare_ref.uaf",      "scene particles.texture authored as a BARE filename"},
    // The purely transitive chain the task calls out by name.
    {"Models/prefab_mesh.uaf",     "scene -> .hbprefab -> mesh"},
    {"Materials/prefab.hbmat",     "scene -> .hbprefab -> material"},
    {"Textures/deep_only.uaf",     "scene -> .hbprefab -> .hbmat -> texture (3 hops)"},
    // Schematic literals.
    {"Scripts/logic.hbschem",      "scene schematic.asset"},
    {"Dialogue/talk.hbdialogue",   "-> .hbschem PlayDialogue literal"},
    {"Audio/voice_talk.uaf",       "-> .hbdialogue line clip"},
    {"Audio/foot.hbevent",         "-> .hbschem literal"},
    {"Audio/step.uaf",             "-> .hbevent sound"},
    // Interactable / trigger / encounter -> cutscene -> dialogue -> voiceline.
    {"Cutscenes/intro.hbcutscene", "scene interactable.asset"},
    {"Dialogue/cut.hbdialogue",    "-> .hbcutscene dialogue marker"},
    {"Audio/voice_cut.uaf",        "-> .hbcutscene voiceline"},
    // Character rig.
    {"Chars/hero.hbchar",          "scene characterRig.asset"},
    {"Models/skeleton.uaf",        "-> .hbchar skeleton"},
    {"Models/part_torso.uaf",      "-> .hbchar variant mesh"},
    // Destructible.
    {"Fx/wall.hbfrac",             "scene destructible.asset"},
    {"Materials/interior.hbmat",   "-> .hbfrac interiorMaterial (binary HOOK)"},
    {"Textures/interior.uaf",      "-> that .hbmat's albedo"},
    // Terrain splat, particles, animation, world text.
    {"Materials/splat0.hbmat",     "scene terrain.splat.layers[0]"},
    {"Materials/splat1.hbmat",     "scene terrain.splat.layers[1]"},
    {"Textures/splat0.uaf",        "-> splat0 albedo"},
    {"Textures/splat1.uaf",        "-> splat1 albedo"},
    {"Anim/retarget.uaf",          "scene animator.source"},
    {"Anim/locomotion.uaf",        "scene motionMatching.sourceAsset"},
    {"Fonts/world.uaf",            "scene worldText.font"},
    // Bakes: leaves that were never packed at all.
    {"GI/volume.hbgi",             "scene header giSource"},
    {"Probes/probe0.hbprobe",      "scene reflectionProbe.source (was NOT PACKABLE)"},
    {"Paint/canvas.hbpaint",       "scene paint.source"},
    {"Audio/ambience.uaf",         "scene audio.asset"},
    // UI document -> its widget art, font, sounds and clip.
    {"Textures/ui_button.uaf",     ".hbui element texture"},
    {"Textures/ui_disabled.uaf",   ".hbui element disabledTexture (1 of 14)"},
    {"Fonts/ui.uaf",               ".hbui element font"},
    {"Audio/click.uaf",            ".hbui element clickSound"},
    {"UI/fade.hbuianim",           ".hbui uiAnimator.clip"},
    // Music stems.
    {"Audio/stem_a.uaf",           "-> .hbmusic layer asset"},
    {"Audio/stem_b.uaf",           "-> .hbmusic layer asset"},
};

// Reachable from nothing. Must be excluded with the filter on and present with
// it off - including `orphan_deep.uaf`, which is only named by another orphan
// (so a closure that leaked one orphan would leak both).
const std::vector<const char*> kOrphans = {
    "Orphans/unused.uaf",
    "Orphans/unused.hbmat",
    "Orphans/orphan_deep.uaf",
    "Orphans/unused.hbdialogue",
};

void BuildSyntheticProject(const fs::path& a) {
    // Not every asset writer in the tree creates its parent directory (uaf::Write*
    // does not), so make them all up front rather than discovering it per format.
    std::error_code ec;
    for (const char* d : {"Textures", "Models", "Materials", "Audio", "Fonts", "Anim", "UI",
                          "Scenes", "Prefabs", "Scripts", "Dialogue", "Cutscenes", "Music",
                          "Chars", "Fx", "GI", "Probes", "Paint", "Orphans"})
        fs::create_directories(a / d, ec);

    // --- leaves -------------------------------------------------------------
    for (const char* t : {"Textures/hero_albedo.uaf", "Textures/hero_thick.uaf",
                          "Textures/scene_albedo.uaf", "Textures/bare_ref.uaf",
                          "Textures/deep_only.uaf", "Textures/interior.uaf",
                          "Textures/splat0.uaf", "Textures/splat1.uaf",
                          "Textures/ui_button.uaf", "Textures/ui_disabled.uaf",
                          "Textures/icon_general.uaf", "Textures/icon_key_e.uaf",
                          "Orphans/unused.uaf", "Orphans/orphan_deep.uaf"})
        WriteTex(a / t);
    for (const char* f : {"Fonts/world.uaf", "Fonts/ui.uaf"}) WriteFontFile(a / f);
    for (const char* s : {"Audio/voice_talk.uaf", "Audio/voice_cut.uaf", "Audio/step.uaf",
                          "Audio/click.uaf", "Audio/ambience.uaf", "Audio/stem_a.uaf",
                          "Audio/stem_b.uaf", "Anim/retarget.uaf", "Anim/locomotion.uaf"})
        WriteAudioClip(a / s);

    // --- meshes (each names its own textures + generated .hbmat) -------------
    WriteMeshAsset(a / "Models/hero.uaf", "Textures/hero_albedo.uaf", "Materials/hero.hbmat");
    WriteMeshAsset(a / "Models/prefab_mesh.uaf", "", "");
    WriteMeshAsset(a / "Models/skeleton.uaf", "", "");
    WriteMeshAsset(a / "Models/part_torso.uaf", "", "");

    // --- materials ----------------------------------------------------------
    WriteMat(a / "Materials/hero.hbmat", "Textures/hero_albedo.uaf", "Textures/hero_thick.uaf");
    WriteMat(a / "Materials/scene.hbmat", "Textures/scene_albedo.uaf");
    WriteMat(a / "Materials/prefab.hbmat", "Textures/deep_only.uaf");
    WriteMat(a / "Materials/interior.hbmat", "Textures/interior.uaf");
    WriteMat(a / "Materials/splat0.hbmat", "Textures/splat0.uaf");
    WriteMat(a / "Materials/splat1.hbmat", "Textures/splat1.uaf");
    WriteMat(a / "Orphans/unused.hbmat", "Orphans/orphan_deep.uaf");

    // --- binary leaves ------------------------------------------------------
    WriteLeafStub(a / "GI/volume.hbgi", 0x56494748u);
    WriteLeafStub(a / "Probes/probe0.hbprobe", 0x42525048u);
    WriteLeafStub(a / "Paint/canvas.hbpaint", 0x544E5048u);

    // --- .hbfrac (the binary HOOK: its header names an interior material) ----
    {
        FractureAsset f;
        f.boundsMin = glm::vec3(-1.0f);
        f.boundsMax = glm::vec3(1.0f);
        f.interiorMaterial = "Materials/interior.hbmat";
        SaveFracture(a / "Fx/wall.hbfrac", f);
    }

    // --- dialogue / cutscene / music / audio event / character --------------
    {
        DialogueAsset d;
        d.lines.push_back({"Hero", "hello", "Audio/voice_talk.uaf", 1.0f});
        SaveDialogue(a / "Dialogue/talk.hbdialogue", d);
        DialogueAsset c;
        c.lines.push_back({"Hero", "cut", "", 1.0f});
        SaveDialogue(a / "Dialogue/cut.hbdialogue", c);
        DialogueAsset o;
        o.lines.push_back({"Nobody", "orphan", "", 1.0f});
        SaveDialogue(a / "Orphans/unused.hbdialogue", o);
    }
    {
        CutsceneAsset c;
        CutsceneDialogueMarker m;
        m.dialogue = "Dialogue/cut.hbdialogue";
        m.voiceline = "Audio/voice_cut.uaf";
        c.dialogue.push_back(m);
        SaveCutscene(a / "Cutscenes/intro.hbcutscene", c);
    }
    {
        MusicGraph g;
        MusicState s;
        s.name = "explore";
        MusicLayer l0;
        l0.name = "bed";
        l0.asset = "Audio/stem_a.uaf";
        MusicLayer l1;
        l1.name = "tension";
        l1.asset = "Audio/stem_b.uaf";
        s.layers = {l0, l1};
        g.states.push_back(s);
        g.initialState = "explore";
        SaveMusicGraph(a / "Music/score.hbmusic", g);
    }
    {
        AudioEvent ev;
        AudioEventSound s;
        s.asset = "Audio/step.uaf";
        ev.sounds.push_back(s);
        SaveAudioEvent(a / "Audio/foot.hbevent", ev);
    }
    {
        CharacterAsset c;
        c.skeleton = "uaf:Models/skeleton.uaf#0";
        c.slots.push_back({"torso", {}});
        CharacterVariant v;
        v.id = "bare";
        v.slot = "torso";
        v.mesh = "uaf:Models/part_torso.uaf#0";
        v.material = "Materials/hero.hbmat";
        v.isDefault = true;
        c.variants.push_back(v);
        SaveCharacter(a / "Chars/hero.hbchar", c);
    }

    // --- schematic (asset paths live in node LITERALS) ----------------------
    WriteJson(a / "Scripts/logic.hbschem",
              json{{"version", 1},
                   {"nextId", 4},
                   {"nodes",
                    json::array({json{{"id", 1},
                                      {"type", 40},
                                      {"pos", json::array({0, 0})},
                                      {"literals", json::array({json{{"t", 4},
                                                                     {"s", "Dialogue/"
                                                                           "talk.hbdialogue"}}})}},
                                 json{{"id", 2},
                                      {"type", 41},
                                      {"pos", json::array({0, 60})},
                                      {"literals", json::array({json{{"t", 4},
                                                                     {"s", "Audio/"
                                                                           "foot.hbevent"}}})}}})},
                   {"links", json::array()}});

    // --- .hbuianim ----------------------------------------------------------
    WriteJson(a / "UI/fade.hbuianim",
              json{{"version", 1}, {"duration", 1.0}, {"loop", false}, {"tracks", json::array()}});

    // --- .hbui --------------------------------------------------------------
    WriteJson(a / "UI/menu.hbui",
              json{{"version", 3},
                   {"entities",
                    json::array({json{{"name", "Play"},
                                      {"ui", json{{"texture", "Textures/ui_button.uaf"},
                                                  {"font", "Fonts/ui.uaf"},
                                                  {"disabledTexture", "Textures/ui_disabled.uaf"},
                                                  {"frames", json::array()},
                                                  {"clickSound", "Audio/click.uaf"}}},
                                      {"uiAnimator", json{{"clip", "UI/fade.hbuianim"}}}}})}});

    // --- .hbprefab (byte-identical in shape to a .hbscene) -------------------
    WriteJson(a / "Prefabs/spawn.hbprefab",
              json{{"version", 1},
                   {"entities", json::array({json{{"name", "Spawned"},
                                                  {"mesh", json{{"source",
                                                                 "uaf:Models/prefab_mesh.uaf#0"},
                                                                {"material",
                                                                 "Materials/prefab.hbmat"}}}}})}});

    // --- the scene: one entity per row of the reference matrix ---------------
    WriteJson(
        a / "Scenes/main.hbscene",
        json{
            {"version", 1},
            {"giSource", "GI/volume.hbgi"},
            {"entities",
             json::array(
                 {json{{"name", "Hero"},
                       {"mesh", json{{"source", "uaf:Models/hero.uaf#0"},
                                     {"material", "Materials/scene.hbmat"}}},
                       {"characterRig", json{{"asset", "Chars/hero.hbchar"}}},
                       {"animator", json{{"source", "Anim/retarget.uaf"}}},
                       {"motionMatching", json{{"sourceAsset", "Anim/locomotion.uaf"}}}},
                  json{{"name", "Script"},
                       {"schematic", json{{"asset", "Scripts/logic.hbschem"}}}},
                  json{{"name", "Talker"},
                       {"interactable", json{{"asset", "Cutscenes/intro.hbcutscene"}}},
                       {"trigger", json{{"asset", "Dialogue/talk.hbdialogue"}}}},
                  json{{"name", "Wall"},
                       {"destructible", json{{"asset", "Fx/wall.hbfrac"}}},
                       {"paint", json{{"source", "Paint/canvas.hbpaint"}}}},
                  json{{"name", "Spawner"},
                       {"spawner", json{{"prefab", "Prefabs/spawn.hbprefab"}}},
                       {"encounter", json{{"clearedAsset", "Cutscenes/intro.hbcutscene"}}}},
                  json{{"name", "Ground"},
                       {"terrain", json{{"splat", json{{"layers",
                                                        json::array({"Materials/splat0.hbmat",
                                                                     "Materials/splat1.hbmat"})}}}}}},
                  // `particles.texture` authored as a BARE FILENAME - the H1
                  // normalisation case. It must come out as "Textures/bare_ref.uaf".
                  json{{"name", "Fx"}, {"particles", json{{"texture", "bare_ref.uaf"}}}},
                  json{{"name", "Sign"}, {"worldText", json{{"font", "Fonts/world.uaf"}}}},
                  json{{"name", "Probe"},
                       {"reflectionProbe", json{{"source", "Probes/probe0.hbprobe"}}}},
                  json{{"name", "Amb"}, {"audio", json{{"asset", "Audio/ambience.uaf"}}}}})}});
}

ClosureOptions SyntheticRoots() {
    ClosureOptions o;
    o.sweepEntryPoints = true;
    // Exactly what Editor::ProjectRootRefs() feeds in from ProjectSettings.
    o.roots = {"Scenes/main.hbscene",       // startupScene
               "UI/menu.hbui",              // uiDocuments[0]
               "Music/score.hbmusic",       // musicGraph
               "Textures/icon_general.uaf", // inputIcons.general
               "Textures/icon_key_e.uaf"};  // inputIcons.keyboard["Interact"]
    return o;
}

// Cooks the project (optionally filtered) and returns the opened pack set.
//
// The cook always carries EXTRAS, because BuildShipping does: the engine's
// compiled shaders and the `.hbproj` are sourced from outside Assets/ and are
// EXEMPT from the filter. They are ~29% of a real shipped build's entries, and a
// filter that swallowed them would produce a game with no shaders and no project
// file - so the exemption is asserted rather than assumed.
bool CookAndOpen(const fs::path& assetsDir, const fs::path& outDir, const std::string& base,
                 const std::set<std::string>* filter, uap::PackSet& outSet) {
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);
    const fs::path shaderSrc = outDir / "_src_shader.bin";
    const fs::path projSrc = outDir / "_src_project.hbproj";
    std::ofstream(shaderSrc, std::ios::binary) << "DXBC-ish";
    std::ofstream(projSrc) << "{\"name\":\"Test\"}";
    const std::vector<uap::ExtraFile> extras = {{"Shaders/Mesh.dxil", shaderSrc},
                                                {"__project.hbproj", projSrc}};
    uap::WriteOptions wo;
    wo.compress = false;
    wo.filter = filter;
    wo.extras = &extras;
    if (!uap::WritePacks(outDir, base, assetsDir, outDir / (base + ".uapmanifest"), wo))
        return false;
    return outSet.Open(outDir, base);
}

} // namespace

bool PackClosureSelfTest() {
    g_failures = 0;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "hbe_packclosure";
    fs::remove_all(root, ec);
    const fs::path assetsDir = root / "Assets";
    fs::create_directories(assetsDir, ec);
    if (!Check(!ec, "could not create the synthetic project directory")) return false;

    BuildSyntheticProject(assetsDir);

    // === 1. The registry must be complete before anything else means anything.
    Check(RegistrySelfTest(), "the asset-format registry itself is inconsistent");

    // === 2. Totality + transitivity.
    const ClosureResult c = ComputeClosure(assetsDir, SyntheticRoots());
    LogClosureReport(c, /*onlyReferenced*/ true, /*allowMissingRefs*/ false);
    Check(c.ok, "a clean synthetic project reported unresolvable references");
    Check(c.missing.empty(), "a clean synthetic project reported missing refs");
    Check(c.unreadable.empty(), "a clean synthetic project reported unreadable files");

    for (const Expect& e : kExpected) {
        Check(c.included.count(e.key) != 0,
              std::string("NOT reached: '") + e.key + "' (should arrive via " + e.via + ")");
    }
    for (const char* o : kOrphans) {
        Check(c.included.count(o) == 0,
              std::string("orphan '") + o + "' was reached; the closure is over-broad");
        Check(std::find(c.excluded.begin(), c.excluded.end(), o) != c.excluded.end(),
              std::string("orphan '") + o + "' is missing from the EXCLUDED report");
    }
    // Every packable file is accounted for exactly once, either in or out - the
    // report is the artifact that makes an exclusion provable, so it must total.
    {
        usize packable = 0;
        for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!ec && it->is_regular_file() && IsPackable(NormalizeExtension(it->path())))
                ++packable;
        }
        Check(c.included.size() + c.excluded.size() == packable,
              "INCLUDED + EXCLUDED does not equal the packable files on disk");
    }

    // === 3. Cook for real, filtered and unfiltered.
    const fs::path packOn = root / "PackFiltered";
    const fs::path packOff = root / "PackAll";
    uap::PackSet setOn, setOff;
    if (Check(CookAndOpen(assetsDir, packOn, "Test", &c.included, setOn),
              "filtered cook failed") &&
        Check(CookAndOpen(assetsDir, packOff, "Test", nullptr, setOff), "full cook failed")) {
        for (const Expect& e : kExpected) {
            Check(setOn.Contains(e.key),
                  std::string("filtered PACK is missing '") + e.key +
                      "' - the closure named it but the filter string did not match the "
                      "pack key (normalisation)");
        }
        for (const char* o : kOrphans) {
            Check(!setOn.Contains(o),
                  std::string("orphan '") + o + "' IS in the filtered pack");
            Check(setOff.Contains(o),
                  std::string("orphan '") + o +
                      "' is NOT in the unfiltered pack - 'only referenced' off must pack "
                      "everything");
        }
        Check(setOff.AssetCount() > setOn.AssetCount(),
              "the filtered pack is not smaller than the unfiltered one");
        // Extras (compiled shaders + the .hbproj) live outside Assets/ and must
        // survive the filter untouched.
        Check(setOn.Contains("Shaders/Mesh.dxil"),
              "the filter swallowed a compiled shader (extras must be exempt)");
        Check(setOn.Contains("__project.hbproj"),
              "the filter swallowed the packed .hbproj (extras must be exempt)");

        // === 4. Every reference of every PACKED asset resolves inside the packs.
        const ReferenceResolver resolver(assetsDir);
        u32 dangling = 0;
        for (const uap::Entry& entry : setOn.Entries()) {
            std::vector<std::string> raws;
            if (!CollectFileRefs(assetsDir / fs::path(entry.path), raws)) continue;
            for (const std::string& raw : raws) {
                if (!ReferenceResolver::LooksLikeAssetRef(raw)) continue;
                const ResolvedRef rr = resolver.Resolve(raw);
                if (rr.status != RefStatus::Resolved || setOn.Contains(rr.key)) continue;
                HBE_ERROR("packclosure: packed '{}' references '{}' -> '{}', which is NOT in "
                          "the packs.",
                          entry.path, raw, rr.key);
                ++dangling;
            }
        }
        Check(dangling == 0, "a packed asset references something outside the pack set");

        // === 4b. vfs::Exists and vfs::ReadFile must agree, against real mounted
        // packs. They did not: ReadFile had the bare-filename fallback and Exists
        // did not, so a project naming `"main.hbscene"` for a file that lives at
        // `Scenes/main.hbscene` cooked cleanly (the closure applies the same
        // filename rule) and was then SKIPPED at boot - Engine gates the startup
        // scene, the boot document and every UI screen on Exists(). Symptom: a
        // shipped build with no world and no menu, one warning, exit 0.
        {
            vfs::MountPacks(packOn, "Test", assetsDir);
            for (const char* probe : {"Scenes/main.hbscene", "main.hbscene", "menu.hbui"}) {
                const fs::path p = assetsDir / probe;
                Check(vfs::Exists(p) == vfs::ReadFile(p).has_value(),
                      std::string("vfs::Exists and vfs::ReadFile disagree about '") + probe +
                          "' - the cook certifies what the boot path then rejects");
            }
            Check(!vfs::Exists(assetsDir / "no_such_asset_anywhere.hbscene"),
                  "vfs::Exists invented a file that does not exist");
            vfs::Unmount();
        }
    }

    // === 5. The failure policy: unresolvable and ambiguous references.
    {
        WriteJson(assetsDir / "Scenes/broken.hbscene",
                  json{{"version", 1},
                       {"entities",
                        json::array({json{{"name", "Bad"},
                                          {"mesh", json{{"source", "uaf:Models/ghost.uaf#0"},
                                                        {"material", "Materials/ghost.hbmat"}}}}})}});
        fs::create_directories(assetsDir / "DupA", ec);
        fs::create_directories(assetsDir / "DupB", ec);
        WriteTex(assetsDir / "DupA/dup.uaf");
        WriteTex(assetsDir / "DupB/dup.uaf");
        WriteJson(assetsDir / "Scenes/ambiguous.hbscene",
                  json{{"version", 1},
                       {"entities", json::array({json{{"name", "Amb"},
                                                      {"particles", json{{"texture", "dup.uaf"}}}}})}});

        const ClosureResult bad = ComputeClosure(assetsDir, SyntheticRoots());
        Check(!bad.ok, "a dangling reference did not fail the closure");
        const auto has = [&bad](const char* raw, const char* reason) {
            return std::any_of(bad.missing.begin(), bad.missing.end(), [&](const MissingRef& m) {
                return m.raw.find(raw) != std::string::npos && m.reason == reason;
            });
        };
        Check(has("Models/ghost.uaf", "not found"),
              "the dangling mesh reference was not reported as not found");
        Check(has("Materials/ghost.hbmat", "not found"),
              "the dangling material reference was not reported as not found");
        // An ambiguous bare filename is UNRESOLVED, not a coin flip: the runtime's
        // own filename fallback would pick whichever pack entry it hit first.
        Check(has("dup.uaf", "ambiguous filename"),
              "an ambiguous bare filename was silently resolved instead of reported");

        // ...and removing them makes the project clean again (so the failure was
        // caused by the dangling refs, not by anything else the test does).
        fs::remove(assetsDir / "Scenes/broken.hbscene", ec);
        fs::remove(assetsDir / "Scenes/ambiguous.hbscene", ec);
        fs::remove_all(assetsDir / "DupA", ec);
        fs::remove_all(assetsDir / "DupB", ec);
    }

    // === 5b. WRONG CASE is a failure, not a resolution.
    //
    // This is the one that gets through every other check. Windows is
    // case-insensitive, so the editor loads `textures/HERO_ALBEDO.uaf` off disk
    // without complaint and the author sees nothing wrong - but the cook would pack
    // it under its REAL key and the shipped runtime looks it up under the authored
    // one, byte for byte (uap::PackSet::Read hashes the string; vfs::ReadByFilename
    // compares with ==). The asset is then missing in the SHIPPED BUILD ONLY. A
    // case-insensitive resolver would have called this Resolved and packed a build
    // that boots untextured with a clean cook log.
    {
        WriteJson(assetsDir / "Scenes/wrongcase.hbscene",
                  json{{"version", 1},
                       {"entities", json::array({json{{"name", "Case"},
                                                      {"mesh", json{{"material",
                                                                     "materials/SCENE.hbmat"}}}}})}});
        const ClosureResult bad = ComputeClosure(assetsDir, SyntheticRoots());
        Check(!bad.ok, "a wrong-case reference did not fail the closure");
        Check(std::any_of(bad.missing.begin(), bad.missing.end(),
                          [](const MissingRef& m) {
                              return m.raw == "materials/SCENE.hbmat" &&
                                     m.reason.find("WRONG CASE") != std::string::npos &&
                                     m.reason.find("Materials/scene.hbmat") != std::string::npos;
                          }),
              "a wrong-case reference was not reported as a case mismatch naming the real file");
        Check(bad.included.count("materials/SCENE.hbmat") == 0,
              "a wrong-case reference was packed under the AUTHORED spelling");
        // The resolver's verdict directly, so the failure is attributed to Resolve
        // rather than to anything the closure does around it.
        const ReferenceResolver resolver(assetsDir);
        Check(resolver.Resolve("Materials/scene.hbmat").status == RefStatus::Resolved,
              "the correctly-cased reference stopped resolving");
        Check(resolver.Resolve("materials/SCENE.hbmat").status == RefStatus::CaseMismatch,
              "a wrong-case path was not reported as CaseMismatch");
        Check(resolver.Resolve("SCENE.hbmat").status == RefStatus::CaseMismatch,
              "a wrong-case BARE FILENAME was not reported as CaseMismatch");
        fs::remove(assetsDir / "Scenes/wrongcase.hbscene", ec);
        Check(ComputeClosure(assetsDir, SyntheticRoots()).ok,
              "the project did not go back to clean after removing the bad references");
    }

    // === 6. A reachable file that will not parse is reported, not assumed inert.
    {
        std::ofstream(assetsDir / "Scenes/corrupt.hbscene") << "{ this is not json";
        const ClosureResult bad = ComputeClosure(assetsDir, SyntheticRoots());
        Check(!bad.ok && std::find(bad.unreadable.begin(), bad.unreadable.end(),
                                   "Scenes/corrupt.hbscene") != bad.unreadable.end(),
              "an unparseable reachable file was not reported as unreadable");
        fs::remove(assetsDir / "Scenes/corrupt.hbscene", ec);
    }

    // === 7. Gate 1 (shape) rejects the strings a document is full of.
    for (const char* noise : {"Hero", "player_start", "{objective}", "#ff8800", "setting:volume",
                              "Speaker: hello.", "", "explore", "torso"}) {
        Check(!ReferenceResolver::LooksLikeAssetRef(noise),
              std::string("'") + noise + "' was treated as an asset reference");
    }
    for (const char* real : {"Textures/a.uaf", "uaf:Models/a.uaf#3", "Materials\\a.hbmat",
                             "./UI/a.hbui", "a.hbschem"}) {
        Check(ReferenceResolver::LooksLikeAssetRef(real),
              std::string("'") + real + "' was NOT treated as an asset reference");
    }

    fs::remove_all(root, ec);
    if (g_failures != 0) {
        HBE_ERROR("packclosure: {} failure(s).", g_failures);
        return false;
    }
    HBE_INFO("packclosure: {} matrix entries reached, {} orphans excluded, cooked packs "
             "verified - closure is total.",
             kExpected.size(), kOrphans.size());
    return true;
}

} // namespace hbe::assets
