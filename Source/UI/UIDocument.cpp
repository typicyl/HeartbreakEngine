// UI/UIDocument.cpp - the `.hbui` document: shared component JSON, loader,
// writer, scene->document converter and the --test-uidoc gate.
#include "UI/UIDocument.h"

#include "UI/UIDocumentJson.h"

#include "Assets/VFS.h"
#include "Core/Log.h"
#include "Project/Project.h"      // AssetsDir / RelativeAssetPath (preload + display)
#include "Renderer/Renderer.h"    // --test-uicanvas: TextureUIId / API (the canvas gate)
#include "Scene/EntityGuid.h"
#include "Scene/PostSettingsSerialization.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/StreamingSalvage.h" // PartitionEntitiesRemappingParents (SALVAGE 1)
#include "UI/UIWorld.h"             // AcquireUITarget (--test-uicanvas check 4)

#include <algorithm>
#include <cstring> // std::memcmp (byte parity of the two vertex streams)
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility> // std::pair (CaptureDocument's document-order sort key)
#include <vector>

namespace hbe::ui {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Verbatim from Scene/SceneSerializer.cpp's anonymous namespace. Duplicated
// rather than shared because those helpers are file-local there and the whole
// point of this file is that the two serializers agree BIT FOR BIT - so the
// helpers must be identical, not merely equivalent.
json ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json ToJson(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }
json ToJson(const glm::quat& q) { return json::array({q.w, q.x, q.y, q.z}); }

glm::vec3 Vec3(const json& j, glm::vec3 def = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
}
glm::vec4 Vec4(const json& j, glm::vec4 def = glm::vec4(1.0f)) {
    if (!j.is_array() || j.size() < 4) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()};
}
glm::quat Quat(const json& j) {
    if (!j.is_array() || j.size() < 4) return glm::quat(1, 0, 0, 0);
    return glm::quat(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>());
}

} // namespace

// =============================================================================
// The fourteen shared blocks. LIFTED VERBATIM from SceneSerializer.cpp's
// EntityToJson (writers) and ParseSceneJson (readers). Do not "tidy" them: the
// key spelling, the defaults and every clamp are the on-disk contract for every
// .hbscene already authored, and --test-uidoc diffs them against a frozen copy
// of the pre-extraction code.
// =============================================================================

json WriteElement(const UIElement& e) {
    const UIElement* el = &e;
    return json{{"type", static_cast<int>(el->type)},
                {"text", el->text},
                {"anchorMin", json::array({el->anchorMin.x, el->anchorMin.y})},
                {"anchorMax", json::array({el->anchorMax.x, el->anchorMax.y})},
                {"pivot", json::array({el->pivot.x, el->pivot.y})},
                {"offset", json::array({el->offset.x, el->offset.y})},
                {"size", json::array({el->size.x, el->size.y})},
                {"color", ToJson(el->color)},
                {"textSize", el->textSize},
                {"hAlign", static_cast<int>(el->hAlign)},
                {"vAlign", static_cast<int>(el->vAlign)},
                {"visible", el->visible},
                {"texture", el->texture},
                {"fill", el->fill},
                {"fillColor", ToJson(el->fillColor)},
                {"radial", el->radial},
                {"fullscreen", el->fullscreen},
                {"action", el->action},
                {"font", el->font},
                {"rotation", el->rotation},
                {"scale", json::array({el->scale.x, el->scale.y})},
                {"value", el->value},
                {"toggled", el->toggled},
                {"selected", el->selected},
                {"options", el->options},
                {"frames", el->frames},
                {"contentSize", json::array({el->contentSize.x, el->contentSize.y})},
                {"scrollPos", json::array({el->scrollPos.x, el->scrollPos.y})},
                {"scrollSpeed", el->scrollSpeed},
                {"scrollVertical", el->scrollVertical},
                {"scrollHorizontal", el->scrollHorizontal},
                {"autoScroll", el->autoScroll},
                {"autoScrollLoop", el->autoScrollLoop},
                {"placeholder", el->placeholder},
                {"maxLength", el->maxLength},
                {"hoverColor", ToJson(el->hoverColor)},
                {"pressedColor", ToJson(el->pressedColor)},
                {"disabledColor", ToJson(el->disabledColor)},
                {"enabled", el->enabled},
                {"hoverSound", el->hoverSound},
                {"clickSound", el->clickSound},
                {"trackTexture", el->trackTexture},
                {"fillTexture", el->fillTexture},
                {"handleTexture", el->handleTexture},
                {"handleSize", el->handleSize},
                {"onTexture", el->onTexture},
                {"offTexture", el->offTexture},
                {"hoverTexture", el->hoverTexture},
                {"pressedTexture", el->pressedTexture},
                {"disabledTexture", el->disabledTexture},
                {"cellTexture", el->cellTexture},
                {"slice", ToJson(el->slice)},
                {"wrap", el->wrap}};
}

json WriteCanvas(const UICanvas& c) {
    const UICanvas* canvas = &c;
    return json{{"scaleMode", canvas->scaleMode},
                {"refWidth", canvas->refWidth},
                {"refHeight", canvas->refHeight},
                {"sortOrder", canvas->sortOrder},
                {"visible", canvas->visible},
                {"worldSpace", canvas->worldSpace},
                {"worldWidth", canvas->worldWidth},
                {"emissive", canvas->emissive},
                {"rtWidth", canvas->rtWidth},
                {"rtHeight", canvas->rtHeight},
                {"occlude", canvas->occlude},
                {"interactRange", canvas->interactRange}};
}

json WriteAnimator(const UIAnimator& a) {
    const UIAnimator* an = &a;
    return json{{"clip", an->clip}, {"trigger", static_cast<int>(an->trigger)}};
}

json WritePanel(const UIPanel& panel) {
    const UIPanel* p = &panel;
    return json{{"name", p->name}, {"startVisible", p->startVisible}};
}

json WriteLayout(const UILayoutGroup& l) {
    const UILayoutGroup* lg = &l;
    return json{{"kind", static_cast<int>(lg->kind)},
                {"spacing", lg->spacing},
                {"cellSize", json::array({lg->cellSize.x, lg->cellSize.y})},
                {"padding", ToJson(lg->padding)},
                {"columns", lg->columns},
                {"fitContent", lg->fitContent}};
}

json WriteGroup(const UICanvasGroup& g) {
    const UICanvasGroup* cg = &g;
    return json{{"opacity", cg->opacity}, {"interactable", cg->interactable}};
}

json WriteWorldText(const WorldText& w) {
    const WorldText* wt = &w;
    return json{{"text", wt->text},
                {"size", wt->size},
                {"color", json::array({wt->color.r, wt->color.g, wt->color.b, wt->color.a})},
                {"font", wt->font},
                {"billboard", wt->billboard}};
}

void ReadElement(const json& j, UIElement& out) {
    const json* it = &j;
    const int type = it->value("type", 1);
    out.type = static_cast<UIElement::Type>(glm::clamp(type, 0, 9));
    out.text = it->value("text", "");
    const auto vec2Of = [&](const char* key, glm::vec2 def) {
        const json arr = it->value(key, json::array());
        if (arr.is_array() && arr.size() >= 2) {
            return glm::vec2{arr[0].get<f32>(), arr[1].get<f32>()};
        }
        return def;
    };
    // v2 scenes stored one "anchor" point: both anchors collapse to it.
    const glm::vec2 legacyAnchor = vec2Of("anchor", {0.5f, 0.5f});
    out.anchorMin = vec2Of("anchorMin", legacyAnchor);
    out.anchorMax = vec2Of("anchorMax", legacyAnchor);
    out.pivot = vec2Of("pivot", {0.5f, 0.5f});
    const json offset = it->value("offset", json::array());
    if (offset.is_array() && offset.size() >= 2) {
        out.offset = {offset[0].get<f32>(), offset[1].get<f32>()};
    }
    const json size = it->value("size", json::array());
    if (size.is_array() && size.size() >= 2) {
        out.size = {size[0].get<f32>(), size[1].get<f32>()};
    }
    out.color = Vec4(it->value("color", json()), glm::vec4(1.0f));
    // v1 scenes stored a multiplier ("textScale"); v2 stores canvas px.
    out.textSize = it->contains("textSize") ? it->value("textSize", 28.0f)
                                            : it->value("textScale", 1.0f) * 28.0f;
    out.hAlign = static_cast<UIElement::HAlign>(glm::clamp(it->value("hAlign", 1), 0, 2));
    out.vAlign = static_cast<UIElement::VAlign>(glm::clamp(it->value("vAlign", 1), 0, 2));
    out.visible = it->value("visible", true);
    out.texture = it->value("texture", "");
    out.fill = it->value("fill", 0.65f);
    out.fillColor = Vec4(it->value("fillColor", json()), {0.86f, 0.27f, 0.33f, 1.0f});
    out.radial = it->value("radial", false);
    out.fullscreen = it->value("fullscreen", false);
    out.action = it->value("action", "");
    out.font = it->value("font", "");
    out.rotation = it->value("rotation", 0.0f);
    out.scale = vec2Of("scale", {1.0f, 1.0f});
    out.value = it->value("value", 0.5f);
    out.toggled = it->value("toggled", false);
    out.selected = it->value("selected", 0);
    out.options = it->value("options", std::vector<std::string>{});
    out.frames = it->value("frames", std::vector<std::string>{});
    out.contentSize = glm::max(vec2Of("contentSize", {0.0f, 0.0f}), glm::vec2(0.0f));
    out.scrollPos = vec2Of("scrollPos", {0.0f, 0.0f});
    out.scrollSpeed = glm::clamp(it->value("scrollSpeed", 40.0f), 1.0f, 4000.0f);
    out.scrollVertical = it->value("scrollVertical", true);
    out.scrollHorizontal = it->value("scrollHorizontal", false);
    out.autoScroll = glm::clamp(it->value("autoScroll", 0.0f), -4000.0f, 4000.0f);
    out.autoScrollLoop = it->value("autoScrollLoop", false);
    out.placeholder = it->value("placeholder", "");
    out.maxLength = glm::clamp(it->value("maxLength", 64), 1, 4096);
    out.hoverColor = Vec4(it->value("hoverColor", json()), glm::vec4(0.0f));
    out.pressedColor = Vec4(it->value("pressedColor", json()), glm::vec4(0.0f));
    out.disabledColor = Vec4(it->value("disabledColor", json()), glm::vec4(0.0f));
    out.enabled = it->value("enabled", true);
    out.hoverSound = it->value("hoverSound", "");
    out.clickSound = it->value("clickSound", "");
    out.trackTexture = it->value("trackTexture", "");
    out.fillTexture = it->value("fillTexture", "");
    out.handleTexture = it->value("handleTexture", "");
    out.handleSize = glm::max(it->value("handleSize", 0.0f), 0.0f);
    out.onTexture = it->value("onTexture", "");
    out.offTexture = it->value("offTexture", "");
    out.hoverTexture = it->value("hoverTexture", "");
    out.pressedTexture = it->value("pressedTexture", "");
    out.disabledTexture = it->value("disabledTexture", "");
    out.cellTexture = it->value("cellTexture", "");
    out.slice = glm::max(Vec4(it->value("slice", json()), glm::vec4(0.0f)), glm::vec4(0.0f));
    out.wrap = it->value("wrap", false);
}

void ReadCanvas(const json& j, UICanvas& out) {
    const json* it = &j;
    out.scaleMode = it->value("scaleMode", 1u);
    out.refWidth = it->value("refWidth", 1920.0f);
    out.refHeight = it->value("refHeight", 1080.0f);
    out.sortOrder = it->value("sortOrder", 0);
    out.visible = it->value("visible", true);
    out.worldSpace = it->value("worldSpace", false);
    out.worldWidth = glm::clamp(it->value("worldWidth", 1.0f), 0.01f, 1000.0f);
    out.emissive = glm::clamp(it->value("emissive", 0.0f), 0.0f, 10.0f);
    const u32 rw = it->value("rtWidth", 0u);
    const u32 rh = it->value("rtHeight", 0u);
    out.rtWidth = rw ? glm::clamp(rw, 64u, 4096u) : 0u; // 0 = ref size
    out.rtHeight = rh ? glm::clamp(rh, 64u, 4096u) : 0u;
    // Two-places-default rule: these fallbacks MUST match the struct defaults in
    // Components.h (occlude = true, interactRange = 0 = unlimited).
    out.occlude = it->value("occlude", true);
    out.interactRange = glm::clamp(it->value("interactRange", 0.0f), 0.0f, 10000.0f);
}

void ReadAnimator(const json& j, UIAnimator& out) {
    out.clip = j.value("clip", "");
    out.trigger = static_cast<UIAnimator::Trigger>(glm::clamp(j.value("trigger", 1), 0, 5));
}

void ReadPanel(const json& j, UIPanel& out) {
    out.name = j.value("name", "");
    out.startVisible = j.value("startVisible", false);
}

void ReadLayout(const json& j, UILayoutGroup& out) {
    const json* it = &j;
    UILayoutGroup& lg = out;
    lg.kind = static_cast<UILayoutGroup::Kind>(glm::clamp(it->value("kind", 0), 0, 2));
    lg.spacing = it->value("spacing", 8.0f);
    if (const json cs = it->value("cellSize", json()); cs.is_array() && cs.size() >= 2) {
        lg.cellSize = {cs[0].get<f32>(), cs[1].get<f32>()};
    }
    lg.padding = Vec4(it->value("padding", json()), glm::vec4(0.0f));
    lg.columns = glm::max(it->value("columns", 1), 1);
    lg.fitContent = it->value("fitContent", false);
}

void ReadGroup(const json& j, UICanvasGroup& out) {
    out.opacity = glm::clamp(j.value("opacity", 1.0f), 0.0f, 1.0f);
    out.interactable = j.value("interactable", true);
}

void ReadWorldText(const json& j, WorldText& out) {
    out.text = j.value("text", "Text");
    out.size = glm::clamp(j.value("size", 0.25f), 0.001f, 100.0f);
    out.color = Vec4(j.value("color", json()), glm::vec4(1.0f));
    out.font = j.value("font", "");
    out.billboard = j.value("billboard", false);
}

// =============================================================================
// Document format
// =============================================================================

const std::vector<std::string>& DocumentComponentKeys() {
    // NOTE the deliberate absence of "worldText" - see UIDocument.h decision 4.
    static const std::vector<std::string> kKeys = {
        "ui", "uiCanvas", "uiAnimator", "uiPanel", "uiLayoutGroup", "uiCanvasGroup"};
    return kKeys;
}

namespace {

// Everything a lifted entity is allowed to keep. Anything else is DROPPED and
// counted, so the migration report can name what it threw away rather than
// losing it silently.
bool IsDocumentEntityKey(const std::string& k) {
    if (k == "name" || k == "parent" || k == "transform") return true;
    const auto& keys = DocumentComponentKeys();
    return std::find(keys.begin(), keys.end(), k) != keys.end();
}

json DocHeader(const DocData& doc) {
    json root;
    root["version"] = kDocVersion;
    root["kind"] = kDocKind;
    root["canvas"] = {{"scaleMode", static_cast<u32>(doc.canvas.mode)},
                      {"refWidth", doc.canvas.refWidth},
                      {"refHeight", doc.canvas.refHeight}};
    root["ambientIntensity"] = doc.ambientIntensity;
    root["exposure"] = doc.exposure;
    // MANDATORY. Always written, even at defaults - see UIDocument.h decision 1.
    root["post"] = scene::PostToJson(doc.post);
    return root;
}

void ReadDocHeader(const json& root, DocData& out) {
    if (const auto c = root.find("canvas"); c != root.end() && c->is_object()) {
        out.canvas.mode = static_cast<ScaleMode>(glm::clamp(c->value("scaleMode", 1u), 0u, 2u));
        out.canvas.refWidth = glm::max(c->value("refWidth", rhi::kUICanvasWidth), 64.0f);
        out.canvas.refHeight = glm::max(c->value("refHeight", rhi::kUICanvasHeight), 64.0f);
    }
    out.ambientIntensity = root.value("ambientIntensity", 1.0f);
    out.exposure = root.value("exposure", 1.0f);
    if (const auto p = root.find("post"); p != root.end() && p->is_object())
        scene::PostFromJson(*p, out.post);
}

json DocEntityToJson(const DocEntity& d) {
    json je;
    if (!d.name.empty()) je["name"] = d.name;
    if (d.hasTransform) {
        je["transform"] = {{"p", ToJson(d.transform.position)},
                           {"r", ToJson(d.transform.rotation)},
                           {"s", ToJson(d.transform.scale)}};
    }
    if (d.parent >= 0) je["parent"] = d.parent;
    if (d.hasElement) je["ui"] = WriteElement(d.element);
    if (d.hasCanvas) je["uiCanvas"] = WriteCanvas(d.canvas);
    if (d.hasAnimator) je["uiAnimator"] = WriteAnimator(d.animator);
    if (d.hasPanel) je["uiPanel"] = WritePanel(d.panel);
    if (d.hasLayout) je["uiLayoutGroup"] = WriteLayout(d.layout);
    if (d.hasGroup) je["uiCanvasGroup"] = WriteGroup(d.group);
    // No "guid": a document is a template, not a world object (decision 3).
    return je;
}

void DocEntityFromJson(const json& je, DocEntity& d) {
    d.name = je.value("name", "");
    d.parent = je.value("parent", -1);
    if (auto it = je.find("transform"); it != je.end()) {
        d.hasTransform = true;
        d.transform.position = Vec3(it->value("p", json()));
        d.transform.rotation = Quat(it->value("r", json()));
        d.transform.scale = Vec3(it->value("s", json()), glm::vec3(1.0f));
    }
    if (auto it = je.find("ui"); it != je.end()) {
        d.hasElement = true;
        ReadElement(*it, d.element);
    }
    if (auto it = je.find("uiCanvas"); it != je.end()) {
        d.hasCanvas = true;
        ReadCanvas(*it, d.canvas);
    }
    if (auto it = je.find("uiAnimator"); it != je.end()) {
        d.hasAnimator = true;
        ReadAnimator(*it, d.animator);
    }
    if (auto it = je.find("uiPanel"); it != je.end()) {
        d.hasPanel = true;
        ReadPanel(*it, d.panel);
    }
    if (auto it = je.find("uiLayoutGroup"); it != je.end()) {
        d.hasLayout = true;
        ReadLayout(*it, d.layout);
    }
    if (auto it = je.find("uiCanvasGroup"); it != je.end()) {
        d.hasGroup = true;
        ReadGroup(*it, d.group);
    }
}

json DocToJson(const DocData& doc) {
    json root = DocHeader(doc);
    json& arr = root["entities"] = json::array();
    for (const DocEntity& d : doc.entities) arr.push_back(DocEntityToJson(d));
    return root;
}

// Shared by LoadDocument (VFS bytes) and LoadDocumentFromString (undo snapshots).
// `label` only names the source in diagnostics.
bool ParseDocRoot(const json& root, DocData& out, const std::string& label) {
    if (!root.is_object()) {
        HBE_ERROR("UI: '{}' is not a UI document (root is not an object).", label);
        return false;
    }
    // A wrong `kind` is a WARNING, not a refusal: the file extension already
    // routed us here, and refusing would turn a typo into "the game has no menu".
    if (const std::string kind = root.value("kind", std::string(kDocKind)); kind != kDocKind)
        HBE_WARN("UI: '{}' has kind '{}', expected '{}'.", label, kind, kDocKind);

    out = DocData{};
    ReadDocHeader(root, out);
    const auto ents = root.find("entities");
    if (ents == root.end() || !ents->is_array()) return true; // empty document is legal
    out.entities.reserve(ents->size());
    for (const json& je : *ents) {
        if (!je.is_object()) continue;
        DocEntity d;
        DocEntityFromJson(je, d);
        out.entities.push_back(std::move(d));
    }
    // Defensive: an out-of-range parent index would become an emplace on
    // entt::null in InstantiateDocument (blocker B1). Clamp it to a root here,
    // where we can name the file.
    const int n = static_cast<int>(out.entities.size());
    for (DocEntity& d : out.entities) {
        if (d.parent >= n || d.parent < -1) {
            HBE_WARN("UI: '{}' entity '{}' has out-of-range parent {}; loaded as a root.",
                     label, d.name, d.parent);
            d.parent = -1;
        }
    }
    return true;
}

} // namespace

bool LoadDocument(const fs::path& path, DocData& out) {
    // Pack-aware, like every other runtime asset read: a shipped build has no
    // loose .hbui on disk, only the mounted .uap packs. `.hbuianim` shipped
    // broken because it used a raw ifstream here, and `.hbworld` was never
    // packed AND used an ifstream - two independent failure modes, both silent.
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes || bytes->empty()) {
        HBE_ERROR("UI: cannot open document '{}'.", path.string());
        return false;
    }
    json root;
    try {
        root = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("UI: '{}' is not valid JSON ({}).", path.string(), e.what());
        return false;
    }
    return ParseDocRoot(root, out, path.string());
}

std::string SaveDocumentToString(const DocData& doc) { return DocToJson(doc).dump(); }

bool LoadDocumentFromString(const std::string& text, DocData& out) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        HBE_ERROR("UI: document snapshot is not valid JSON ({}).", e.what());
        return false;
    }
    return ParseDocRoot(root, out, "<snapshot>");
}

bool SaveDocument(const DocData& doc, const fs::path& path) {
    // Editor-only: std::ofstream is the sanctioned asymmetry (read via VFS,
    // write via the filesystem), exactly like SaveScene and ui::SaveClip.
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        HBE_ERROR("UI: cannot write document '{}'.", path.string());
        return false;
    }
    out << DocToJson(doc).dump(2);
    return static_cast<bool>(out);
}

// =============================================================================
// LIVE DOCUMENTS - instantiate / capture / DocumentSet (P3)
// =============================================================================

std::vector<entt::entity> InstantiateDocument(Scene& scene, Renderer* renderer,
                                              const DocData& doc, DocHandle handle,
                                              bool screenOwned, bool preload) {
    auto& reg = scene.Registry();
    std::vector<entt::entity> created;
    created.reserve(doc.entities.size());

    // PASS 1 - create every entity and emplace its components. Scene::CreateEntity
    // is the one guid mint site, so each document entity gets a fresh runtime guid
    // like anything else; it is never WRITTEN anywhere (documents are excluded from
    // every snapshot), so it stays a within-session id and nothing aliases.
    for (const DocEntity& d : doc.entities) {
        const entt::entity e = scene.CreateEntity(d.name);
        created.push_back(e);
        // The document tag goes on FIRST so that anything reacting to a UI
        // component's construct signal already sees the membership.
        reg.emplace<UIDocMember>(e, UIDocMember{handle, screenOwned});
        if (d.hasTransform) reg.emplace<Transform>(e, d.transform);
        if (d.hasElement) {
            UIElement el = d.element;
            el.hovered = false; // runtime interaction state is never authored
            el.clicked = false;
            reg.emplace<UIElement>(e, el);
        }
        if (d.hasCanvas) reg.emplace<UICanvas>(e, d.canvas);
        if (d.hasAnimator) reg.emplace<UIAnimator>(e, d.animator);
        if (d.hasPanel) reg.emplace<UIPanel>(e, d.panel);
        if (d.hasLayout) reg.emplace<UILayoutGroup>(e, d.layout);
        if (d.hasGroup) reg.emplace<UICanvasGroup>(e, d.group);
    }

    // PASS 2 - parent links, once every entity exists. LoadDocument already
    // clamped an out-of-range index to -1, but this is the site B1 named, so the
    // bound is re-checked here rather than trusted from a caller.
    const int n = static_cast<int>(created.size());
    for (int i = 0; i < n; ++i) {
        const int p = doc.entities[static_cast<usize>(i)].parent;
        if (p < 0 || p >= n || p == i) continue;
        reg.emplace<Parent>(created[static_cast<usize>(i)],
                            Parent{created[static_cast<usize>(p)]});
    }

    // The laid-out element set changed structurally.
    scene.BumpUIVersion();

    // Eager asset preload, exactly as scene::Instantiate does for a UI scene:
    // bake the font atlas and load every UI texture NOW instead of lazily on the
    // first draw, which is what kills the blank-text/white-quad first frame.
    // THIS is the half that needs a device, and the only reason a document open
    // is not fully headless.
    if (preload && renderer && !doc.entities.empty() && Project::HasActive()) {
        bool anyElement = false;
        for (const DocEntity& d : doc.entities)
            if (d.hasElement) { anyElement = true; break; }
        if (anyElement) PreloadUIAssets(scene, *renderer, Project::Active().AssetsDir());
    }
    return created;
}

void CaptureDocument(const Scene& scene, DocHandle doc, const DocData& header,
                     DocData& out, const std::vector<entt::entity>* order) {
    const auto& reg = scene.Registry();
    out = DocData{};
    out.canvas = header.canvas;
    out.post = header.post;
    out.ambientIntensity = header.ambientIntensity;
    out.exposure = header.exposure;
    if (doc == 0) return;

    // The registry, not a cached vector, is the source of truth: the editor
    // creates and deletes entities inside an open document and a stale list
    // would silently drop or resurrect them.
    std::vector<entt::entity> members;
    for (const entt::entity e : reg.view<const UIDocMember>()) {
        if (!reg.valid(e)) continue;
        if (reg.get<const UIDocMember>(e).doc != doc) continue;
        if (reg.all_of<UISurface>(e)) continue; // generated world-UI page quads
        members.push_back(e);
    }
    // DOCUMENT ORDER, because entity order is z-order and hit order for
    // canvas-less roots and it has to survive a save/reopen unchanged.
    //
    // `order` (the DocumentInstance's own list) is the truth: it is file order at
    // open and creation order after that. Sorting by the raw entt handle is NOT a
    // substitute - entt 3.13 keeps the entity index in the low 20 bits and a
    // 12-bit VERSION in the high bits, so a handle recycled after one delete
    // compares ABOVE every freshly created one and the document silently permutes
    // between what the editor drew and what the file says. Members created since
    // the open (or every member, when a caller passes no order) fall into a tail
    // sorted by entity INDEX with the version masked off, which is at least
    // stable across a save/save with no edits in between.
    {
        std::unordered_map<u32, usize> rank;
        if (order) {
            rank.reserve(order->size());
            for (usize i = 0; i < order->size(); ++i)
                rank.emplace(static_cast<u32>((*order)[i]), i);
        }
        const usize tail = rank.size();
        const auto keyOf = [&](entt::entity e) -> std::pair<usize, u32> {
            const auto it = rank.find(static_cast<u32>(e));
            // entt::to_entity() strips the version bits, leaving the index.
            return it != rank.end()
                       ? std::pair<usize, u32>{it->second, 0u}
                       : std::pair<usize, u32>{tail, static_cast<u32>(entt::to_entity(e))};
        };
        std::stable_sort(members.begin(), members.end(),
                         [&](entt::entity a, entt::entity b) { return keyOf(a) < keyOf(b); });
    }

    std::unordered_map<u32, int> indexOf;
    indexOf.reserve(members.size());
    for (usize i = 0; i < members.size(); ++i)
        indexOf[static_cast<u32>(members[i])] = static_cast<int>(i);

    out.entities.reserve(members.size());
    for (const entt::entity e : members) {
        DocEntity d;
        if (const Name* nm = reg.try_get<Name>(e)) d.name = nm->value;
        if (const Parent* p = reg.try_get<Parent>(e)) {
            // A parent OUTSIDE the document becomes a root: a document file
            // cannot reference a world entity, by construction.
            const auto it = indexOf.find(static_cast<u32>(p->entity));
            d.parent = it != indexOf.end() ? it->second : -1;
        }
        if (const Transform* t = reg.try_get<Transform>(e)) {
            d.hasTransform = true;
            d.transform = *t;
        }
        if (const UIElement* el = reg.try_get<UIElement>(e)) {
            d.hasElement = true;
            d.element = *el;
        }
        if (const UICanvas* c = reg.try_get<UICanvas>(e)) {
            d.hasCanvas = true;
            d.canvas = *c;
        }
        if (const UIAnimator* a = reg.try_get<UIAnimator>(e)) {
            d.hasAnimator = true;
            d.animator = *a;
        }
        if (const UIPanel* pl = reg.try_get<UIPanel>(e)) {
            d.hasPanel = true;
            d.panel = *pl;
        }
        if (const UILayoutGroup* lg = reg.try_get<UILayoutGroup>(e)) {
            d.hasLayout = true;
            d.layout = *lg;
        }
        if (const UICanvasGroup* cg = reg.try_get<UICanvasGroup>(e)) {
            d.hasGroup = true;
            d.group = *cg;
        }
        out.entities.push_back(std::move(d));
    }
}

DocHandle DocumentSet::Open(Scene& scene, Renderer* renderer, const fs::path& path,
                            bool screenOwned, bool preload) {
    DocData data;
    if (!LoadDocument(path, data)) return 0;
    return OpenFromData(scene, renderer, data, path, screenOwned, preload);
}

DocHandle DocumentSet::OpenFromData(Scene& scene, Renderer* renderer, const DocData& doc,
                                    const fs::path& path, bool screenOwned, bool preload) {
    const DocHandle h = next_++;
    DocumentInstance inst;
    inst.handle = h;
    inst.path = path;
    inst.rel = Project::HasActive() && !path.empty() ? Project::Active().RelativeAssetPath(path)
                                                     : path.string();
    inst.screenOwned = screenOwned;
    inst.header = doc;
    inst.header.entities.clear(); // the registry owns the entities from here on
    inst.entities = InstantiateDocument(scene, renderer, doc, h, screenOwned, preload);
    docs_.push_back(std::move(inst));
    return h;
}

DocHandle DocumentSet::AdoptLegacy(Scene& scene, const std::vector<entt::entity>& entities,
                                   const fs::path& path, const DocData& header,
                                   bool screenOwned) {
    auto& reg = scene.Registry();
    const DocHandle h = next_++;
    DocumentInstance inst;
    inst.handle = h;
    inst.path = path;
    inst.rel = Project::HasActive() && !path.empty() ? Project::Active().RelativeAssetPath(path)
                                                     : path.string();
    inst.screenOwned = screenOwned;
    inst.header = header;
    inst.header.entities.clear();
    inst.legacy = true;
    for (const entt::entity e : entities) {
        if (!reg.valid(e)) continue;
        reg.emplace_or_replace<UIDocMember>(e, UIDocMember{h, screenOwned});
        // The legacy branch ALSO keeps the Persistent tag it has always had.
        // Belt and braces on purpose: a `.hbscene`-sourced UI layer is the exact
        // configuration a half-migrated project boots with, and it must behave
        // identically to a document under both sweeps even if one of the two
        // predicates is ever changed in isolation.
        if (!reg.all_of<Persistent>(e)) reg.emplace<Persistent>(e);
        inst.entities.push_back(e);
    }
    docs_.push_back(std::move(inst));
    return h;
}

void DocumentSet::Track(Scene& scene, DocHandle doc, entt::entity e) {
    DocumentInstance* inst = Get(doc);
    if (!inst || !scene.Registry().valid(e)) return;
    scene.Registry().emplace_or_replace<UIDocMember>(e,
                                                     UIDocMember{doc, inst->screenOwned});
    // Idempotent: re-tracking an entity must not duplicate it in the order list.
    if (std::find(inst->entities.begin(), inst->entities.end(), e) == inst->entities.end())
        inst->entities.push_back(e);
    inst->dirty = true;
}

void DocumentSet::Close(Scene& scene, DocHandle doc) {
    if (doc == 0) return;
    auto& reg = scene.Registry();
    // Destroy by MEMBERSHIP, not by the recorded list: the editor creates and
    // deletes entities inside an open document, and a world-space canvas grows a
    // generated UISurface child that carries the tag too (UIWorld propagates it).
    std::vector<entt::entity> kill;
    for (const entt::entity e : reg.view<const UIDocMember>())
        if (reg.valid(e) && reg.get<const UIDocMember>(e).doc == doc) kill.push_back(e);
    for (const entt::entity e : kill)
        if (reg.valid(e)) reg.destroy(e);
    docs_.erase(std::remove_if(docs_.begin(), docs_.end(),
                               [doc](const DocumentInstance& d) { return d.handle == doc; }),
                docs_.end());
    scene.BumpUIVersion();
}

void DocumentSet::CloseAll(Scene& scene) {
    // Snapshot the handles: Close mutates docs_.
    std::vector<DocHandle> handles;
    handles.reserve(docs_.size());
    for (const DocumentInstance& d : docs_) handles.push_back(d.handle);
    for (const DocHandle h : handles) Close(scene, h);
    docs_.clear();
}

DocumentInstance* DocumentSet::Get(DocHandle doc) {
    if (doc == 0) return nullptr;
    for (DocumentInstance& d : docs_)
        if (d.handle == doc) return &d;
    return nullptr;
}

const DocumentInstance* DocumentSet::Get(DocHandle doc) const {
    if (doc == 0) return nullptr;
    for (const DocumentInstance& d : docs_)
        if (d.handle == doc) return &d;
    return nullptr;
}

const DocData* DocumentSet::Header(DocHandle doc) const {
    const DocumentInstance* d = Get(doc);
    return d ? &d->header : nullptr;
}

// =============================================================================
// Converter (.hbscene -> .hbui). PURE JSON: no registry, no Renderer, no GPU.
// =============================================================================

namespace {

bool HasAnyDocComponent(const json& je) {
    for (const std::string& k : DocumentComponentKeys())
        if (je.contains(k)) return true;
    return false;
}

// UI membership: an entity that carries a document component, or that is
// TRANSITIVELY parented under one. The transitive clause matters because a
// plain grouping node ("Buttons") holding UI children carries no UI key itself,
// and leaving it behind would orphan the whole subtree into roots.
//
// Fixed-point rather than one pass: `parent` is an index that may point FORWARD.
std::vector<bool> MarkUIEntities(const json& ents) {
    const int n = static_cast<int>(ents.size());
    std::vector<bool> isUI(static_cast<usize>(n), false);
    for (int i = 0; i < n; ++i)
        if (ents[static_cast<usize>(i)].is_object() && HasAnyDocComponent(ents[static_cast<usize>(i)]))
            isUI[static_cast<usize>(i)] = true;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < n; ++i) {
            if (isUI[static_cast<usize>(i)]) continue;
            const json& je = ents[static_cast<usize>(i)];
            if (!je.is_object()) continue;
            const int p = je.value("parent", -1);
            if (p >= 0 && p < n && isUI[static_cast<usize>(p)]) {
                isUI[static_cast<usize>(i)] = true;
                changed = true;
            }
        }
    }
    return isUI;
}

} // namespace

bool ConvertSceneToDocument(const fs::path& src, const fs::path& dstDoc,
                            const CanvasConfig& canvas, ConvertReport& report,
                            bool dryRun, bool force) {
    report = ConvertReport{};

    json root;
    {
        std::ifstream in(src, std::ios::binary);
        if (!in) {
            HBE_WARN("ConvertSceneToDocument: cannot read '{}'.", src.string());
            return false;
        }
        try {
            in >> root;
        } catch (const std::exception& e) {
            HBE_WARN("ConvertSceneToDocument: '{}' is not valid JSON ({}).", src.string(),
                     e.what());
            return false;
        }
    }
    const auto ents = root.find("entities");
    if (ents == root.end() || !ents->is_array()) {
        HBE_WARN("ConvertSceneToDocument: '{}' has no entities array.", src.string());
        return false;
    }
    report.sceneEntities = static_cast<u32>(ents->size());

    const std::vector<bool> isUI = MarkUIEntities(*ents);
    u32 uiCount = 0;
    for (const bool b : isUI)
        if (b) ++uiCount;
    report.uiEntities = uiCount;
    report.worldEntities = report.sceneEntities - uiCount;
    report.convertible = uiCount > 0;
    report.mixed = uiCount > 0 && report.worldEntities > 0;
    if (!report.convertible) return true; // nothing to do; not an error

    // SALVAGE 1 (Scene/StreamingSalvage.h): partition + renumber every surviving
    // "parent" index, cross-partition parent -> -1 (root). This is the loop that
    // SplitSceneFile used and the only surviving copy; do not re-derive it.
    // The predicate is index-exact rather than call-order-dependent: a json array
    // is a std::vector<json>, so the element's index is its offset from the
    // first. (A stateful counter would silently mis-partition if the salvaged
    // loop ever visited out of order.)
    const json* base = &(*ents)[0];
    const json uiArr =
        salvage::PartitionEntitiesRemappingParents(*ents, [&](const json& e) {
            const usize i = static_cast<usize>(&e - base);
            return i < isUI.size() && isUI[i];
        });

    // Whitelist-scrub. `guid` is dropped like every other non-document key
    // (decision 3) - it is not special-cased, it simply is not on the list.
    DocData doc;
    doc.canvas = canvas;
    doc.ambientIntensity = root.value("ambientIntensity", 1.0f);
    doc.exposure = root.value("exposure", 1.0f);
    if (const auto p = root.find("post"); p != root.end() && p->is_object())
        scene::PostFromJson(*p, doc.post);

    // A SEVERED MOUNT is the one lossy thing the lift does, and no key count can
    // see it: `parent` is a whitelisted document key, so a UI entity whose parent
    // stayed behind in the scene simply arrives with `parent: -1` and zero dropped
    // keys. Detected by comparing the SOURCE's parent (still a scene index) with
    // what the remap produced. MarkUIEntities marks descendants of UI entities,
    // never ancestors, so this is exactly "the mount point is world content".
    {
        usize k = 0;
        for (usize i = 0; i < ents->size(); ++i) {
            if (!isUI[i]) continue;
            const json& row = (*ents)[i]; // NOT `src`: that is the source PATH parameter
            const int srcParent = row.is_object() ? row.value("parent", -1) : -1;
            const int newParent =
                k < uiArr.size() && uiArr[k].is_object() ? uiArr[k].value("parent", -1) : -1;
            ++k;
            if (srcParent < 0 || newParent >= 0) continue;
            ++report.severedParents;
            if (report.severedParentNames.size() < 8) {
                std::string nm = row.is_object() ? row.value("name", std::string()) : std::string();
                if (nm.empty()) nm = "(unnamed #" + std::to_string(i) + ")";
                report.severedParentNames.push_back(nm);
            }
        }
    }

    std::unordered_set<std::string> dropped;
    for (const json& je : uiArr) {
        for (auto kv = je.begin(); kv != je.end(); ++kv) {
            if (IsDocumentEntityKey(kv.key())) continue;
            dropped.insert(kv.key());
            ++report.droppedKeys;
        }
        DocEntity d;
        DocEntityFromJson(je, d);
        doc.entities.push_back(std::move(d));
    }
    report.droppedKeyNames.assign(dropped.begin(), dropped.end());
    std::sort(report.droppedKeyNames.begin(), report.droppedKeyNames.end());

    if (dryRun) return true;
    std::error_code ec;
    if (fs::exists(dstDoc, ec) && !force) {
        HBE_WARN("ConvertSceneToDocument: '{}' already exists; refusing to overwrite "
                 "(use --force).",
                 dstDoc.string());
        return false;
    }
    if (!SaveDocument(doc, dstDoc)) return false;
    report.wrote = true;
    return true;
}

UIMigrationStats MigrateSceneUI(const fs::path& assetsDir, const CanvasConfig& canvas,
                                bool dryRun, bool force) {
    UIMigrationStats st;
    const fs::path uiDir = assetsDir / "UI";
    std::error_code ec;
    // Deterministic order so a dry run and the real run report identically.
    std::vector<fs::path> scenes;
    for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".hbscene") continue;
        scenes.push_back(it->path());
    }
    std::sort(scenes.begin(), scenes.end());

    // Stem -> the source that claimed it in THIS run. The destination is flat, so
    // two convertible scenes with the same stem in different folders would compete
    // for one `UI/<stem>.hbui`; without this, `--force` silently overwrote the
    // first with the second and still reported both as written.
    std::unordered_map<std::string, fs::path> claimed;

    for (const fs::path& p : scenes) {
        ++st.files;
        const fs::path dst = uiDir / (p.stem().string() + ".hbui");
        ConvertReport rep;
        if (const auto it = claimed.find(p.stem().string()); it != claimed.end()) {
            // Survey it (dry) so the report still counts its UI, then refuse.
            ConvertReport probe;
            ConvertSceneToDocument(p, dst, canvas, probe, /*dryRun*/ true, /*force*/ false);
            if (!probe.convertible) continue;
            ++st.convertible;
            ++st.collisions;
            st.uiEntities += probe.uiEntities;
            if (probe.mixed) ++st.mixed;
            HBE_WARN("migrate-ui: '{}' and '{}' both map to '{}'. REFUSED (nothing "
                     "written for the second) - rename one source or convert it by "
                     "hand; overwriting would destroy the first document.",
                     it->second.string(), p.string(), dst.filename().string());
            continue;
        }
        if (!ConvertSceneToDocument(p, dst, canvas, rep, dryRun, force)) {
            // A refusal to overwrite is a SKIP, not a failure: re-running the
            // migration on an already-migrated project must be a no-op, not an
            // error, and must never clobber hand-edited documents.
            if (rep.convertible && fs::exists(dst, ec) && !force) {
                ++st.convertible;
                ++st.skipped;
                st.uiEntities += rep.uiEntities; // the survey is still valid
                if (rep.mixed) ++st.mixed;
                HBE_INFO("migrate-ui: {} -> {} SKIPPED (destination exists).",
                         p.filename().string(), dst.filename().string());
                continue;
            }
            ++st.failed;
            continue;
        }
        if (!rep.convertible) continue;
        ++st.convertible;
        claimed.emplace(p.stem().string(), p);
        st.uiEntities += rep.uiEntities;
        st.droppedKeys += rep.droppedKeys;
        st.severedParents += rep.severedParents;
        if (rep.mixed) ++st.mixed;
        if (rep.wrote) ++st.written;
        // The severed mounts, NAMED. This is the line that tells the operator a
        // world-space page stopped following the door it hung on.
        if (rep.severedParents > 0) {
            std::string names;
            for (const std::string& n : rep.severedParentNames)
                names += (names.empty() ? "" : ", ") + n;
            HBE_WARN("migrate-ui: {} : {} UI entit(ies) were parented to WORLD content "
                     "and became roots in the document ({}). Their mount point stayed "
                     "in the scene - re-anchor them by hand if the parenting mattered.",
                     p.filename().string(), rep.severedParents, names);
        }
        std::string droppedList;
        for (const std::string& k : rep.droppedKeyNames)
            droppedList += (droppedList.empty() ? "" : ", ") + k;
        HBE_INFO("migrate-ui: {} -> {} : {} UI entit(ies), {} world entit(ies) LEFT IN "
                 "PLACE{}{}",
                 p.filename().string(), dst.filename().string(), rep.uiEntities,
                 rep.worldEntities, droppedList.empty() ? "" : "; dropped keys: ",
                 droppedList);
        // THE SOURCE SCENE IS NEVER MODIFIED OR DELETED. A file whose entities
        // were ALL lifted is a retirement CANDIDATE; retiring it is the
        // operator's decision, made after eyeballing the document.
        if (!rep.mixed)
            HBE_INFO("migrate-ui: '{}' is now fully represented by '{}' - candidate for "
                     "retirement once the project points at the document (P3). NOT "
                     "deleted.",
                     p.filename().string(), dst.filename().string());
    }
    return st;
}

int RepointProjectDocuments(const fs::path& hbproj, const fs::path& assetsDir,
                            bool dryRun) {
    std::ifstream in(hbproj, std::ios::binary);
    if (!in) {
        HBE_ERROR("repoint: cannot open '{}'.", hbproj.string());
        return -1;
    }
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        HBE_ERROR("repoint: '{}' is not valid JSON ({}).", hbproj.string(), e.what());
        return -1;
    }
    in.close();
    if (!j.is_object()) {
        HBE_ERROR("repoint: '{}' root is not an object.", hbproj.string());
        return -1;
    }

    int changed = 0;
    // (legacy key, new key). Order matches Project::ParseSettings' fallback.
    const std::pair<const char*, const char*> kSlots[] = {
        {"uiScene", "uiDocument"}, {"studioLoadingScene", "bootDocument"}};
    for (const auto& [oldKey, newKey] : kSlots) {
        // Already repointed by hand? Leave it alone; only drop the dead legacy key.
        // `is_string()` FIRST: get<std::string>() on a null/number/object throws
        // nlohmann::json::type_error, and the only try/catch in this function wraps
        // the parse - so one hand-edited `"uiDocument": null` aborted the whole
        // --migrate-ui process instead of reporting. (The oldKey read below was
        // already guarded this way; this is the same guard on the other side.)
        if (j.contains(newKey) && j[newKey].is_string() &&
            !j[newKey].get<std::string>().empty()) {
            if (j.contains(oldKey)) {
                if (!dryRun) j.erase(oldKey);
                HBE_INFO("repoint: '{}' already set; dropping stale '{}'.", newKey, oldKey);
                ++changed;
            }
            continue;
        }
        if (!j.contains(oldKey) || !j[oldKey].is_string()) continue;
        const std::string legacy = j[oldKey].get<std::string>();
        if (legacy.empty()) continue;
        const fs::path stem = fs::path(legacy).stem();
        const std::string rel = "UI/" + stem.string() + ".hbui";
        std::error_code ec;
        if (!fs::exists(assetsDir / rel, ec)) {
            // Refusing beats silently blanking the slot: a project whose UI slot
            // points at nothing boots with no menu at all.
            HBE_WARN("repoint: '{}' -> '{}' skipped; '{}' does not exist (run "
                     "--migrate-ui first).",
                     oldKey, newKey, rel);
            continue;
        }
        HBE_INFO("repoint: {} '{}' -> {} '{}'.", oldKey, legacy, newKey, rel);
        if (!dryRun) {
            j[newKey] = rel;
            j.erase(oldKey);
        }
        ++changed;
    }
    if (changed > 0 && !dryRun) {
        std::ofstream out(hbproj, std::ios::binary | std::ios::trunc);
        if (!out) {
            HBE_ERROR("repoint: cannot write '{}'.", hbproj.string());
            return -1;
        }
        out << j.dump(4); // Project::Save's indentation, so the diff stays small
        if (!out) return -1;
    }
    return changed;
}

// =============================================================================
// Splitter (one all-in-one `.hbui` -> one document per SCREEN). PURE JSON.
// =============================================================================

namespace {

// A filename that survives a UIPanel name. Panel names are authored free text and
// end up as paths; anything that could escape the destination directory or break
// on a case-insensitive filesystem is folded to '_'. Empty -> "Screen".
std::string SanitizeScreenFileName(const std::string& panel) {
    std::string s;
    s.reserve(panel.size());
    for (const char c : panel) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        s.push_back(ok ? c : '_');
    }
    // Leading dots/dashes make argument-looking or hidden files; trim them.
    usize b = 0;
    while (b < s.size() && (s[b] == '_' || s[b] == '-')) ++b;
    s.erase(0, b);
    while (!s.empty() && (s.back() == '_' || s.back() == '-')) s.pop_back();
    return s.empty() ? std::string("Screen") : s;
}

} // namespace

bool SplitDocumentByPanel(const fs::path& srcDoc, const fs::path& outDir,
                          const std::string& relPrefix, ScreenSplitReport& report,
                          bool dryRun, bool force) {
    report = ScreenSplitReport{};

    json root;
    {
        // The migrator runs from the editor CLI against loose project files, but
        // reading through the VFS costs nothing and keeps the one asset-read rule
        // intact (a `.hbui` inside a mounted pack is still splittable).
        const std::optional<std::vector<u8>> bytes = vfs::ReadFile(srcDoc);
        if (!bytes || bytes->empty()) {
            std::ifstream in(srcDoc, std::ios::binary);
            if (!in) {
                HBE_ERROR("migrate-screens: cannot read '{}'.", srcDoc.string());
                return false;
            }
            try {
                in >> root;
            } catch (const std::exception& e) {
                HBE_ERROR("migrate-screens: '{}' is not valid JSON ({}).",
                          srcDoc.string(), e.what());
                return false;
            }
        } else {
            try {
                root = json::parse(bytes->begin(), bytes->end());
            } catch (const std::exception& e) {
                HBE_ERROR("migrate-screens: '{}' is not valid JSON ({}).",
                          srcDoc.string(), e.what());
                return false;
            }
        }
    }
    if (!root.is_object()) {
        HBE_ERROR("migrate-screens: '{}' root is not an object.", srcDoc.string());
        return false;
    }
    const auto entsIt = root.find("entities");
    if (entsIt == root.end() || !entsIt->is_array()) {
        HBE_ERROR("migrate-screens: '{}' has no entities array.", srcDoc.string());
        return false;
    }
    const json& ents = *entsIt;
    const int n = static_cast<int>(ents.size());
    report.sourceEntities = static_cast<u32>(n);

    // 1) ROOT PANELS, in document order. A UIPanel that is NOT a root (a panel
    //    nested inside another screen) stays with its ancestor rather than being
    //    lifted out - the split is by SCREEN, and a screen is a root subtree.
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        const json& e = ents[static_cast<usize>(i)];
        if (!e.is_object() || !e.contains("uiPanel")) continue;
        if (e.value("parent", -1) != -1) continue;
        roots.push_back(i);
    }
    if (roots.empty()) {
        HBE_ERROR("migrate-screens: '{}' has no ROOT UIPanel entities - there is "
                  "nothing to split into screens.",
                  srcDoc.string());
        return false;
    }

    // 2) OWNERSHIP. Each entity belongs to the root panel it descends from.
    //    Fixed-point rather than one pass: `parent` may point FORWARD (the
    //    reference document's MainMenu children sit at indices 9-11, its root at
    //    12), so a single forward sweep would leave them unassigned.
    std::vector<int> owner(static_cast<usize>(n), -1); // index of the owning root
    for (const int r : roots) owner[static_cast<usize>(r)] = r;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < n; ++i) {
            if (owner[static_cast<usize>(i)] >= 0) continue;
            const json& e = ents[static_cast<usize>(i)];
            if (!e.is_object()) continue;
            const int p = e.value("parent", -1);
            if (p < 0 || p >= n) continue;
            if (owner[static_cast<usize>(p)] < 0) continue;
            owner[static_cast<usize>(i)] = owner[static_cast<usize>(p)];
            changed = true;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (owner[static_cast<usize>(i)] >= 0) continue;
        ++report.orphans;
        if (report.orphanNames.size() < 16) {
            const json& e = ents[static_cast<usize>(i)];
            std::string nm = e.is_object() ? e.value("name", std::string()) : std::string();
            if (nm.empty()) nm = "(unnamed #" + std::to_string(i) + ")";
            report.orphanNames.push_back(nm);
        }
    }

    // 3) HEADER. Copied VERBATIM into every screen - same canvas config, same
    //    post block, same ambient/exposure - so each screen lays out and looks
    //    exactly as it did inside the combined document. Only entry [0] of the
    //    project's screen list actually gets its `post` replayed, but carrying it
    //    everywhere keeps each file self-describing and makes any of them
    //    promotable to the menu document without an edit.
    json header = root;
    header.erase("entities");

    // 4) Plan the outputs (names, paths, membership). Nothing is written yet:
    //    a refusal must leave the disk untouched, not half-split.
    std::unordered_set<std::string> panelNames;
    std::unordered_map<std::string, u32> globalActions; // action -> screens using it
    struct Planned {
        ScreenSplitReport::Screen info;
        json doc;
    };
    std::vector<Planned> planned;
    planned.reserve(roots.size());

    for (const int r : roots) {
        const json& rootEnt = ents[static_cast<usize>(r)];
        const json& panelObj = rootEnt["uiPanel"];
        std::string panelName =
            panelObj.is_object() ? panelObj.value("name", std::string()) : std::string();
        if (panelName.empty())
            panelName = rootEnt.is_object() ? rootEnt.value("name", std::string())
                                            : std::string();

        ScreenSplitReport::Screen sc;
        sc.panel = panelName;
        sc.startVisible = panelObj.is_object() && panelObj.value("startVisible", false);
        const std::string file = SanitizeScreenFileName(panelName) + ".hbui";
        sc.rel = relPrefix + file;
        sc.file = (outDir / file).string();

        if (!panelNames.insert(panelName).second) {
            report.duplicatePanels.push_back(panelName);
            continue;
        }

        // SALVAGE 1 (Scene/StreamingSalvage.h): partition + renumber every
        // surviving `parent` index. Every member of this screen has its parent in
        // the same partition by construction (ownership is transitive from the
        // root), so the cross-partition -> root fallback should never fire here -
        // it is the defensive path, exactly as the salvaged comment says.
        const json* base = &ents[0];
        json arr = salvage::PartitionEntitiesRemappingParents(ents, [&](const json& e) {
            const usize i = static_cast<usize>(&e - base);
            return i < owner.size() && owner[i] == r;
        });
        sc.entities = static_cast<u32>(arr.size());

        std::unordered_set<std::string> here;
        for (const json& e : arr) {
            if (!e.is_object()) continue;
            const auto uiIt = e.find("ui");
            if (uiIt == e.end() || !uiIt->is_object()) continue;
            const std::string act = uiIt->value("action", std::string());
            if (!act.empty() && ui::IsGlobalAction(act)) here.insert(act);
        }
        for (const std::string& a : here) ++globalActions[a];

        json out = header;
        out["entities"] = std::move(arr);

        Planned p;
        p.info = std::move(sc);
        p.doc = std::move(out);
        planned.push_back(std::move(p));
    }

    for (const auto& [action, count] : globalActions)
        if (count > 1) report.actionCollisions.push_back(action);
    std::sort(report.actionCollisions.begin(), report.actionCollisions.end());

    std::error_code ec;
    for (Planned& p : planned) {
        p.info.existed = fs::exists(fs::path(p.info.file), ec);
        report.screens.push_back(p.info);
    }

    // ALREADY ONE SCREEN PER DOCUMENT. Nothing to split, so nothing to refuse
    // either - and in particular the destination-exists check below must not
    // fire, because the destination a single-screen document maps to is USUALLY
    // ITSELF. Re-running the migration on a migrated project has to be a clean
    // no-op, not an error.
    if (planned.size() <= 1) {
        report.ok = true;
        return true;
    }

    // 5) REFUSE, as a whole, before writing anything.
    if (!report.duplicatePanels.empty()) {
        HBE_ERROR("migrate-screens: '{}' has {} DUPLICATE root panel name(s); two "
                  "screens would compete for one file. Rename them and re-run.",
                  srcDoc.string(), report.duplicatePanels.size());
        return false;
    }
    if (report.orphans > 0 && !force) {
        HBE_ERROR("migrate-screens: '{}' has {} entit(ies) under NO root UIPanel; a "
                  "panel-wise split would LOSE them. Parent them under a screen (or "
                  "pass --force to split anyway and leave them behind).",
                  srcDoc.string(), report.orphans);
        return false;
    }
    if (!force) {
        u32 clashes = 0;
        for (const auto& s : report.screens)
            if (s.existed) ++clashes;
        if (clashes > 0) {
            HBE_ERROR("migrate-screens: {} destination document(s) already exist; "
                      "refusing to overwrite (use --force).",
                      clashes);
            return false;
        }
    }

    report.ok = true;
    if (dryRun) return true;

    fs::create_directories(outDir, ec);
    for (usize i = 0; i < planned.size(); ++i) {
        const fs::path dst(planned[i].info.file);
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out) {
            HBE_ERROR("migrate-screens: cannot write '{}'.", dst.string());
            report.ok = false;
            return false;
        }
        out << planned[i].doc.dump(2); // SaveDocument's indentation
        if (!out) {
            HBE_ERROR("migrate-screens: write failed for '{}'.", dst.string());
            report.ok = false;
            return false;
        }
        report.screens[i].wrote = true;
        HBE_INFO("migrate-screens: {} -> {} ({} entities{}).", report.screens[i].panel,
                 report.screens[i].rel, report.screens[i].entities,
                 report.screens[i].startVisible ? ", startVisible" : "");
    }
    HBE_INFO("migrate-screens: '{}' is LEFT ON DISK, unmodified.", srcDoc.string());
    return true;
}

bool RepointProjectScreens(const fs::path& hbproj, const std::vector<std::string>& rels,
                           bool dryRun) {
    std::ifstream in(hbproj, std::ios::binary);
    if (!in) {
        HBE_ERROR("repoint-screens: cannot open '{}'.", hbproj.string());
        return false;
    }
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        HBE_ERROR("repoint-screens: '{}' is not valid JSON ({}).", hbproj.string(),
                  e.what());
        return false;
    }
    in.close();
    if (!j.is_object()) {
        HBE_ERROR("repoint-screens: '{}' root is not an object.", hbproj.string());
        return false;
    }
    json arr = json::array();
    for (const std::string& r : rels) arr.push_back(r);
    HBE_INFO("repoint-screens: uiDocuments <- [{}]{}",
             [&] {
                 std::string s;
                 for (const std::string& r : rels) {
                     if (!s.empty()) s += ", ";
                     s += r;
                 }
                 return s;
             }(),
             dryRun ? " (dry run)" : "");
    if (dryRun) return true;
    j["uiDocuments"] = std::move(arr);
    // The legacy single slot keeps naming the MENU document, so an older reader
    // (and Project::ParseSettings' own fallback) lands on the right file rather
    // than a blank slot. `uiScene` is the pre-document key; drop it if present.
    j["uiDocument"] = rels.empty() ? std::string() : rels.front();
    j.erase("uiScene");
    std::ofstream out(hbproj, std::ios::binary | std::ios::trunc);
    if (!out) {
        HBE_ERROR("repoint-screens: cannot write '{}'.", hbproj.string());
        return false;
    }
    out << j.dump(4); // Project::Save's indentation, so the diff stays small
    return static_cast<bool>(out);
}

// =============================================================================
// --test-uidoc - THE P2 GATE
// =============================================================================

namespace {

// --- FROZEN pre-extraction writers -------------------------------------------
// Verbatim copies of the seven inline blocks as they stood in
// Scene/SceneSerializer.cpp::EntityToJson BEFORE the extraction (writers at
// :753-845 of that revision). This is the "before" side of the byte-identity
// gate: same precedent as --test-vfxcompat, which diffs the live particle sim
// against a frozen copy of the pre-module-stack loop. If anybody ever "tidies"
// the live writers, this fails loudly instead of silently rewriting every
// .hbscene on the next Ctrl+S.
json FrozenElement(const UIElement* el) {
    json je;
    je["ui"] = {{"type", static_cast<int>(el->type)},
                {"text", el->text},
                {"anchorMin", json::array({el->anchorMin.x, el->anchorMin.y})},
                {"anchorMax", json::array({el->anchorMax.x, el->anchorMax.y})},
                {"pivot", json::array({el->pivot.x, el->pivot.y})},
                {"offset", json::array({el->offset.x, el->offset.y})},
                {"size", json::array({el->size.x, el->size.y})},
                {"color", ToJson(el->color)},
                {"textSize", el->textSize},
                {"hAlign", static_cast<int>(el->hAlign)},
                {"vAlign", static_cast<int>(el->vAlign)},
                {"visible", el->visible},
                {"texture", el->texture},
                {"fill", el->fill},
                {"fillColor", ToJson(el->fillColor)},
                {"radial", el->radial},
                {"fullscreen", el->fullscreen},
                {"action", el->action},
                {"font", el->font},
                {"rotation", el->rotation},
                {"scale", json::array({el->scale.x, el->scale.y})},
                {"value", el->value},
                {"toggled", el->toggled},
                {"selected", el->selected},
                {"options", el->options},
                {"frames", el->frames},
                {"contentSize", json::array({el->contentSize.x, el->contentSize.y})},
                {"scrollPos", json::array({el->scrollPos.x, el->scrollPos.y})},
                {"scrollSpeed", el->scrollSpeed},
                {"scrollVertical", el->scrollVertical},
                {"scrollHorizontal", el->scrollHorizontal},
                {"autoScroll", el->autoScroll},
                {"autoScrollLoop", el->autoScrollLoop},
                {"placeholder", el->placeholder},
                {"maxLength", el->maxLength},
                {"hoverColor", ToJson(el->hoverColor)},
                {"pressedColor", ToJson(el->pressedColor)},
                {"disabledColor", ToJson(el->disabledColor)},
                {"enabled", el->enabled},
                {"hoverSound", el->hoverSound},
                {"clickSound", el->clickSound},
                {"trackTexture", el->trackTexture},
                {"fillTexture", el->fillTexture},
                {"handleTexture", el->handleTexture},
                {"handleSize", el->handleSize},
                {"onTexture", el->onTexture},
                {"offTexture", el->offTexture},
                {"hoverTexture", el->hoverTexture},
                {"pressedTexture", el->pressedTexture},
                {"disabledTexture", el->disabledTexture},
                {"cellTexture", el->cellTexture},
                {"slice", ToJson(el->slice)},
                {"wrap", el->wrap}};
    return je["ui"];
}
json FrozenCanvas(const UICanvas* canvas) {
    json je;
    je["uiCanvas"] = {{"scaleMode", canvas->scaleMode},
                      {"refWidth", canvas->refWidth},
                      {"refHeight", canvas->refHeight},
                      {"sortOrder", canvas->sortOrder},
                      {"visible", canvas->visible},
                      {"worldSpace", canvas->worldSpace},
                      {"worldWidth", canvas->worldWidth},
                      {"emissive", canvas->emissive},
                      {"rtWidth", canvas->rtWidth},
                      {"rtHeight", canvas->rtHeight},
                      {"occlude", canvas->occlude},
                      {"interactRange", canvas->interactRange}};
    return je["uiCanvas"];
}
json FrozenAnimator(const UIAnimator* an) {
    json je;
    je["uiAnimator"] = {{"clip", an->clip}, {"trigger", static_cast<int>(an->trigger)}};
    return je["uiAnimator"];
}
json FrozenPanel(const UIPanel* p) {
    json je;
    je["uiPanel"] = {{"name", p->name}, {"startVisible", p->startVisible}};
    return je["uiPanel"];
}
json FrozenLayout(const UILayoutGroup* lg) {
    json je;
    je["uiLayoutGroup"] = {{"kind", static_cast<int>(lg->kind)},
                           {"spacing", lg->spacing},
                           {"cellSize", json::array({lg->cellSize.x, lg->cellSize.y})},
                           {"padding", ToJson(lg->padding)},
                           {"columns", lg->columns},
                           {"fitContent", lg->fitContent}};
    return je["uiLayoutGroup"];
}
json FrozenGroup(const UICanvasGroup* cg) {
    json je;
    je["uiCanvasGroup"] = {{"opacity", cg->opacity}, {"interactable", cg->interactable}};
    return je["uiCanvasGroup"];
}
json FrozenWorldText(const WorldText* wt) {
    json je;
    je["worldText"] = {{"text", wt->text},
                       {"size", wt->size},
                       {"color", json::array({wt->color.r, wt->color.g, wt->color.b,
                                              wt->color.a})},
                       {"font", wt->font},
                       {"billboard", wt->billboard}};
    return je["worldText"];
}

// Deterministic LCG so a failure is reproducible from the iteration index alone.
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed ? seed : 1) {}
    u32 Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(s >> 33);
    }
    f32 F(f32 lo, f32 hi) {
        return lo + (hi - lo) * (static_cast<f32>(Next() % 100001u) / 100000.0f);
    }
    bool B() { return (Next() & 1u) != 0; }
    int I(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<u32>(hi - lo + 1)); }
    std::string S() {
        static const char* kWords[] = {"", "Play", "quit:now", "  spaced  ", "\"quoted\"",
                                       "line\nbreak", "utf8-\xc3\xa9", "__dlgchoice"};
        return kWords[Next() % 8];
    }
};

void FuzzElement(Rng& r, UIElement& e) {
    e.type = static_cast<UIElement::Type>(r.I(0, 9));
    e.text = r.S();
    e.anchorMin = {r.F(-2, 2), r.F(-2, 2)};
    e.anchorMax = {r.F(-2, 2), r.F(-2, 2)};
    e.pivot = {r.F(0, 1), r.F(0, 1)};
    e.offset = {r.F(-999, 999), r.F(-999, 999)};
    e.size = {r.F(0, 4000), r.F(0, 4000)};
    e.color = {r.F(0, 1), r.F(0, 1), r.F(0, 1), r.F(0, 1)};
    e.textSize = r.F(1, 200);
    e.hAlign = static_cast<UIElement::HAlign>(r.I(0, 2));
    e.vAlign = static_cast<UIElement::VAlign>(r.I(0, 2));
    e.visible = r.B();
    e.texture = r.S();
    e.fill = r.F(0, 1);
    e.fillColor = {r.F(0, 1), r.F(0, 1), r.F(0, 1), r.F(0, 1)};
    e.radial = r.B();
    e.fullscreen = r.B();
    e.action = r.S();
    e.font = r.S();
    e.rotation = r.F(-360, 360);
    e.scale = {r.F(0.01f, 4), r.F(0.01f, 4)};
    e.value = r.F(0, 1);
    e.toggled = r.B();
    e.selected = r.I(0, 8);
    e.options = {r.S(), r.S()};
    e.frames = {r.S()};
    e.contentSize = {r.F(0, 5000), r.F(0, 5000)};
    e.scrollPos = {r.F(-500, 500), r.F(-500, 500)};
    e.scrollSpeed = r.F(1, 4000);
    e.scrollVertical = r.B();
    e.scrollHorizontal = r.B();
    e.autoScroll = r.F(-4000, 4000);
    e.autoScrollLoop = r.B();
    e.placeholder = r.S();
    e.maxLength = r.I(1, 4096);
    e.hoverColor = {r.F(0, 1), r.F(0, 1), r.F(0, 1), r.F(0, 1)};
    e.pressedColor = {r.F(0, 1), r.F(0, 1), r.F(0, 1), r.F(0, 1)};
    e.disabledColor = {r.F(0, 1), r.F(0, 1), r.F(0, 1), r.F(0, 1)};
    e.enabled = r.B();
    e.hoverSound = r.S();
    e.clickSound = r.S();
    e.trackTexture = r.S();
    e.fillTexture = r.S();
    e.handleTexture = r.S();
    e.handleSize = r.F(0, 200);
    e.onTexture = r.S();
    e.offTexture = r.S();
    e.hoverTexture = r.S();
    e.pressedTexture = r.S();
    e.disabledTexture = r.S();
    e.cellTexture = r.S();
    e.slice = {r.F(0, 64), r.F(0, 64), r.F(0, 64), r.F(0, 64)};
    e.wrap = r.B();
}

// Field-by-field equality. Deliberately NOT memcmp: UIElement carries runtime
// members (hovered/clicked/glyph cache) that never serialize.
bool ElementEq(const UIElement& a, const UIElement& b) {
    return WriteElement(a).dump() == WriteElement(b).dump();
}

} // namespace

bool DocumentSelfTest(const std::vector<fs::path>& scenes) {
    bool ok = true;
    const auto expect = [&ok](bool cond, const std::string& what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("uidoc: FAILED - {}", what);
        }
    };

    // --- 1) THE GATE: extracted writers == frozen pre-extraction writers ------
    {
        const UIElement de{};
        const UICanvas dc{};
        const UIAnimator da{};
        const UIPanel dp{};
        const UILayoutGroup dl{};
        const UICanvasGroup dg{};
        const WorldText dw{};
        expect(WriteElement(de).dump() == FrozenElement(&de).dump(), "ui: defaults differ");
        expect(WriteCanvas(dc).dump() == FrozenCanvas(&dc).dump(), "uiCanvas: defaults differ");
        expect(WriteAnimator(da).dump() == FrozenAnimator(&da).dump(),
               "uiAnimator: defaults differ");
        expect(WritePanel(dp).dump() == FrozenPanel(&dp).dump(), "uiPanel: defaults differ");
        expect(WriteLayout(dl).dump() == FrozenLayout(&dl).dump(),
               "uiLayoutGroup: defaults differ");
        expect(WriteGroup(dg).dump() == FrozenGroup(&dg).dump(),
               "uiCanvasGroup: defaults differ");
        expect(WriteWorldText(dw).dump() == FrozenWorldText(&dw).dump(),
               "worldText: defaults differ");
        // The key count is part of the contract: an added-but-unread key is a
        // silent format fork. 53 for UIElement, counted from the block itself.
        expect(WriteElement(de).size() == 53,
               "ui block must emit exactly 53 keys, got " +
                   std::to_string(WriteElement(de).size()));
        // 12 since the pick pass: `occlude` + `interactRange` (world-canvas
        // interaction, Interaction/Pick.cpp). Both defaults are duplicated in
        // Components.h and in ReadCanvas's .value() fallbacks - keep them equal.
        expect(WriteCanvas(dc).size() == 12, "uiCanvas block must emit 12 keys");
        expect(WriteAnimator(da).size() == 2, "uiAnimator block must emit 2 keys");
        expect(WritePanel(dp).size() == 2, "uiPanel block must emit 2 keys");
        expect(WriteLayout(dl).size() == 6, "uiLayoutGroup block must emit 6 keys");
        expect(WriteGroup(dg).size() == 2, "uiCanvasGroup block must emit 2 keys");
        expect(WriteWorldText(dw).size() == 5, "worldText block must emit 5 keys");

        Rng r(0xB0A7ull);
        for (int i = 0; i < 512; ++i) {
            UIElement e;
            FuzzElement(r, e);
            if (WriteElement(e).dump() != FrozenElement(&e).dump()) {
                expect(false, "ui: fuzz case " + std::to_string(i) + " differs from frozen");
                break;
            }
            UICanvas c;
            c.scaleMode = static_cast<u32>(r.I(0, 2));
            c.refWidth = r.F(64, 4000);
            c.refHeight = r.F(64, 4000);
            c.sortOrder = r.I(-99, 99);
            c.visible = r.B();
            c.worldSpace = r.B();
            c.worldWidth = r.F(0.01f, 1000);
            c.emissive = r.F(0, 10);
            c.rtWidth = static_cast<u32>(r.I(0, 4096));
            c.rtHeight = static_cast<u32>(r.I(0, 4096));
            c.occlude = r.B();
            c.interactRange = r.F(0, 500);
            UIAnimator an;
            an.clip = r.S();
            an.trigger = static_cast<UIAnimator::Trigger>(r.I(0, 5));
            UIPanel p;
            p.name = r.S();
            p.startVisible = r.B();
            UILayoutGroup lg;
            lg.kind = static_cast<UILayoutGroup::Kind>(r.I(0, 2));
            lg.spacing = r.F(-50, 200);
            lg.cellSize = {r.F(0, 900), r.F(0, 900)};
            lg.padding = {r.F(0, 60), r.F(0, 60), r.F(0, 60), r.F(0, 60)};
            lg.columns = r.I(1, 32);
            lg.fitContent = r.B();
            UICanvasGroup cg;
            cg.opacity = r.F(0, 1);
            cg.interactable = r.B();
            WorldText wt;
            wt.text = r.S();
            wt.size = r.F(0.001f, 100);
            wt.color = {r.F(0, 1), r.F(0, 1), r.F(0, 1), r.F(0, 1)};
            wt.font = r.S();
            wt.billboard = r.B();
            const bool same = WriteCanvas(c).dump() == FrozenCanvas(&c).dump() &&
                              WriteAnimator(an).dump() == FrozenAnimator(&an).dump() &&
                              WritePanel(p).dump() == FrozenPanel(&p).dump() &&
                              WriteLayout(lg).dump() == FrozenLayout(&lg).dump() &&
                              WriteGroup(cg).dump() == FrozenGroup(&cg).dump() &&
                              WriteWorldText(wt).dump() == FrozenWorldText(&wt).dump();
            if (!same) {
                expect(false, "sibling block fuzz case " + std::to_string(i) + " differs");
                break;
            }
        }
    }

    // --- 2) Reader round-trip: write -> read -> write is a fixed point --------
    {
        Rng r(0x5EEDull);
        for (int i = 0; i < 256; ++i) {
            UIElement a;
            FuzzElement(r, a);
            UIElement b;
            ReadElement(WriteElement(a), b);
            if (!ElementEq(a, b)) {
                expect(false, "ui: round trip lost data at fuzz case " + std::to_string(i));
                break;
            }
        }
        // The two documented back-compat rules must survive the lift.
        {
            UIElement e;
            json j = json::object();
            j["anchor"] = json::array({0.25f, 0.75f});
            j["textScale"] = 2.0f;
            ReadElement(j, e);
            expect(e.anchorMin == glm::vec2(0.25f, 0.75f) &&
                       e.anchorMax == glm::vec2(0.25f, 0.75f),
                   "v2 collapsed \"anchor\" must expand to both anchors");
            expect(e.textSize == 56.0f, "v1 \"textScale\" must scale by 28 into textSize");
        }
        // And the clamps.
        {
            UIElement e;
            json j = json::object();
            j["maxLength"] = 999999;
            j["scrollSpeed"] = -5.0f;
            j["hAlign"] = 77;
            j["slice"] = json::array({-4.0f, -4.0f, -4.0f, -4.0f});
            ReadElement(j, e);
            expect(e.maxLength == 4096 && e.scrollSpeed == 1.0f &&
                       e.hAlign == UIElement::HAlign::Right && e.slice == glm::vec4(0.0f),
                   "reader clamps must survive the lift");
        }
    }

    // --- 3) Document round-trip through a real file ---------------------------
    {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / "hbe_uidoc";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        DocData src;
        src.canvas.mode = ScaleMode::PixelPerfect;
        src.canvas.refWidth = 1280.0f;
        src.canvas.refHeight = 720.0f;
        src.ambientIntensity = 0.8395f;
        src.exposure = 1.25f;
        src.post.fogEnabled = false;
        src.post.autoExposureEnabled = false;
        src.post.contrast = 1.37f;
        Rng r(0xD0Cull);
        for (int i = 0; i < 12; ++i) {
            DocEntity d;
            d.name = "E" + std::to_string(i);
            d.parent = i == 0 ? -1 : i - 1;
            d.hasElement = true;
            FuzzElement(r, d.element);
            if (i == 0) {
                d.hasCanvas = true;
                d.canvas.worldSpace = true;
                d.hasTransform = true;
                d.transform.position = {1.5f, -2.25f, 3.0f};
                d.transform.scale = {2.0f, 2.0f, 2.0f};
            }
            if (i == 1) {
                d.hasPanel = true;
                d.panel.name = "MainMenu";
                d.panel.startVisible = true;
            }
            if (i == 2) {
                d.hasLayout = true;
                d.hasGroup = true;
                d.hasAnimator = true;
                d.animator.clip = "UI/fade.hbuianim";
            }
            src.entities.push_back(std::move(d));
        }
        const fs::path p = dir / "Doc.hbui";
        expect(SaveDocument(src, p), "SaveDocument writes");
        DocData back;
        expect(LoadDocument(p, back), "LoadDocument reads it back");
        expect(back.entities.size() == src.entities.size(), "document entity count survives");
        bool same = back.canvas.mode == src.canvas.mode &&
                    back.canvas.refWidth == src.canvas.refWidth &&
                    back.canvas.refHeight == src.canvas.refHeight &&
                    back.ambientIntensity == src.ambientIntensity &&
                    back.exposure == src.exposure &&
                    scene::PostToJson(back.post).dump() == scene::PostToJson(src.post).dump();
        expect(same, "document header (canvas + post + ambient/exposure) survives");
        for (usize i = 0; i < back.entities.size() && i < src.entities.size(); ++i) {
            const DocEntity& a = src.entities[i];
            const DocEntity& b = back.entities[i];
            if (a.name != b.name || a.parent != b.parent || a.hasElement != b.hasElement ||
                a.hasCanvas != b.hasCanvas || a.hasPanel != b.hasPanel ||
                a.hasLayout != b.hasLayout || a.hasGroup != b.hasGroup ||
                a.hasAnimator != b.hasAnimator || a.hasTransform != b.hasTransform ||
                (a.hasElement && !ElementEq(a.element, b.element))) {
                expect(false, "document entity " + std::to_string(i) + " lost data");
                break;
            }
        }
        // The header MUST carry post even when it is all defaults - dropping it
        // makes the menu render with gameplay's look.
        {
            DocData plain;
            DocEntity pe;
            pe.name = "Root";
            pe.hasPanel = true;
            plain.entities.push_back(pe);
            const fs::path q = dir / "Plain.hbui";
            expect(SaveDocument(plain, q), "SaveDocument writes a defaults-only document");
            std::ifstream in(q, std::ios::binary);
            json j;
            in >> j;
            expect(j.contains("post") && j["post"].is_object() && j["post"].size() > 40,
                   "the .hbui header must ALWAYS carry the post block");
            expect(j.value("kind", std::string()) == "uidoc", "header kind must be uidoc");
            expect(j.contains("canvas") && j.contains("ambientIntensity") &&
                       j.contains("exposure"),
                   "header must carry canvas + ambientIntensity + exposure");
            expect(!j["entities"][0].contains("guid"),
                   "a .hbui entity must never carry a guid");
        }

        // --- 4) Converter: partition, parent remap, key strip -----------------
        {
            json sc;
            sc["version"] = 1;
            sc["ambientIntensity"] = 0.5f;
            sc["exposure"] = 2.5f;
            sc["post"] = scene::PostToJson(src.post);
            json arr = json::array();
            // 0: world root (mesh)          -> stays
            // 1: UI root (uiPanel)          -> lifted, becomes doc index 0
            // 2: plain child of 1           -> lifted transitively, doc index 1
            // 3: world child of 0           -> stays
            // 4: UI child of 2 (forward ptr already resolved by fixed point)
            // 5: worldText entity           -> NOT UI (decision 4)
            arr.push_back({{"name", "Ground"}, {"guid", "aaaa"}, {"mesh", {{"source", "x"}}}});
            arr.push_back({{"name", "Menu"},
                           {"guid", "bbbb"},
                           {"sceneLayer", "static"},
                           {"uiPanel", WritePanel(UIPanel{})}});
            arr.push_back({{"name", "Group"}, {"guid", "cccc"}, {"parent", 1}});
            arr.push_back({{"name", "Rock"}, {"guid", "dddd"}, {"parent", 0}});
            arr.push_back({{"name", "Label"},
                           {"guid", "eeee"},
                           {"parent", 2},
                           {"ui", WriteElement(UIElement{})}});
            arr.push_back({{"name", "Sign"},
                           {"guid", "ffff"},
                           {"worldText", WriteWorldText(WorldText{})}});
            sc["entities"] = arr;
            const fs::path scp = dir / "Mixed.hbscene";
            {
                std::ofstream o(scp, std::ios::binary | std::ios::trunc);
                o << sc.dump(2);
            }
            const fs::path outDoc = dir / "Mixed.hbui";
            ConvertReport rep;
            CanvasConfig cc;
            cc.mode = ScaleMode::MatchHeight;
            cc.refWidth = 1920.0f;
            cc.refHeight = 1080.0f;
            expect(ConvertSceneToDocument(scp, outDoc, cc, rep, /*dryRun*/ false,
                                          /*force*/ false),
                   "converter runs on a mixed file");
            expect(rep.sceneEntities == 6 && rep.uiEntities == 3 && rep.worldEntities == 3,
                   "converter partitions 3 UI / 3 world (worldText is NOT UI)");
            expect(rep.mixed, "a mixed file reports mixed");
            expect(rep.droppedKeys == 4,
                   "converter drops guid x3 + sceneLayer x1, got " +
                       std::to_string(rep.droppedKeys));
            DocData conv;
            expect(LoadDocument(outDoc, conv), "the converted document loads");
            expect(conv.entities.size() == 3, "3 entities lifted");
            if (conv.entities.size() == 3) {
                expect(conv.entities[0].name == "Menu" && conv.entities[0].parent == -1,
                       "UI root stays a root");
                expect(conv.entities[1].name == "Group" && conv.entities[1].parent == 0,
                       "parent index 1 remaps to 0");
                expect(conv.entities[2].name == "Label" && conv.entities[2].parent == 1,
                       "parent index 2 remaps to 1");
            }
            // The source scene is UNTOUCHED - this is the hard rule.
            json after;
            {
                std::ifstream in(scp, std::ios::binary);
                in >> after;
            }
            expect(after.dump(2) == sc.dump(2),
                   "the converter must NEVER modify the source .hbscene");
            // Re-running refuses to overwrite.
            ConvertReport rep2;
            expect(!ConvertSceneToDocument(scp, outDoc, cc, rep2, false, false),
                   "converter refuses to overwrite an existing .hbui without --force");
            expect(ConvertSceneToDocument(scp, outDoc, cc, rep2, false, true),
                   "--force overwrites");
            // A world-only file converts to nothing and is not an error.
            json wo;
            wo["version"] = 1;
            wo["entities"] = json::array({{{"name", "Cube"}, {"mesh", {{"source", "y"}}}}});
            const fs::path wop = dir / "World.hbscene";
            {
                std::ofstream o(wop, std::ios::binary | std::ios::trunc);
                o << wo.dump(2);
            }
            ConvertReport rep3;
            expect(ConvertSceneToDocument(wop, dir / "World.hbui", cc, rep3, false, false) &&
                       !rep3.convertible && !fs::exists(dir / "World.hbui", ec),
                   "a world-only scene yields no document and no error");

            // A SEVERED MOUNT must be COUNTED AND NAMED. A world-space page hung
            // off a door lifts into the document as a root; the door stays in the
            // scene. `droppedKeys` cannot see it (`parent` is a document key), so
            // without this the only lossy thing the lift does was silent.
            json mnt;
            mnt["version"] = 1;
            mnt["entities"] = json::array(
                {{{"name", "Door"}, {"mesh", {{"source", "d"}}}},
                 {{"name", "Page"}, {"parent", 0}, {"uiCanvas", WriteCanvas(UICanvas{})}}});
            const fs::path mntp = dir / "Mount.hbscene";
            {
                std::ofstream o(mntp, std::ios::binary | std::ios::trunc);
                o << mnt.dump(2);
            }
            ConvertReport rep4;
            expect(ConvertSceneToDocument(mntp, dir / "Mount.hbui", cc, rep4, false, false),
                   "a scene with a world-mounted page converts");
            expect(rep4.severedParents == 1,
                   "a cross-partition parent is REPORTED as severed, got " +
                       std::to_string(rep4.severedParents));
            expect(rep4.severedParentNames.size() == 1 &&
                       rep4.severedParentNames[0] == "Page",
                   "the severed entity is NAMED");
            expect(rep.severedParents == 0,
                   "a wholly-UI subtree severs nothing (no false positives)");
        }

        // --- 5) Real content, END TO END through SaveScene ---------------------
        // For each supplied .hbscene: parse it, rebuild a registry carrying every
        // UI component it authored, SaveScene it, then diff the UI sub-objects
        // the file actually contains against the FROZEN pre-extraction writers.
        // This is the byte-identity proof on authored data rather than on fuzz.
        for (const fs::path& scenePath : scenes) {
            scene::SceneData data;
            if (!scene::ParseSceneFile(scenePath, data)) {
                expect(false, "cannot parse '" + scenePath.string() + "'");
                continue;
            }
            Scene s;
            u32 uiBlocks = 0;
            for (const scene::EntityData& d : data.entities) {
                const entt::entity e = s.CreateEntity(d.name);
                auto& reg = s.Registry();
                if (d.guid != 0) reg.emplace_or_replace<Guid>(e, Guid{d.guid});
                if (d.hasTransform) reg.emplace<Transform>(e, d.transform);
                if (d.hasSceneLayerTag)
                    reg.emplace<SceneLayer>(e, SceneLayer{d.sceneLayerKind});
                if (d.hasUI) {
                    reg.emplace<UIElement>(e, d.uiElement);
                    ++uiBlocks;
                }
                if (d.hasUICanvas) {
                    reg.emplace<UICanvas>(e, d.uiCanvas);
                    ++uiBlocks;
                }
                if (d.hasUIAnimator) {
                    reg.emplace<UIAnimator>(e, d.uiAnimator);
                    ++uiBlocks;
                }
                if (d.hasUIPanel) {
                    reg.emplace<UIPanel>(e, d.uiPanel);
                    ++uiBlocks;
                }
                if (d.hasUILayoutGroup) {
                    reg.emplace<UILayoutGroup>(e, d.uiLayoutGroup);
                    ++uiBlocks;
                }
                if (d.hasUICanvasGroup) {
                    reg.emplace<UICanvasGroup>(e, d.uiCanvasGroup);
                    ++uiBlocks;
                }
                if (d.hasWorldText) {
                    reg.emplace<WorldText>(e, d.worldText);
                    ++uiBlocks;
                }
            }
            const fs::path saved = dir / (scenePath.stem().string() + ".resaved.hbscene");
            expect(scene::SaveScene(s, saved), "re-save '" + scenePath.stem().string() + "'");
            std::string liveText;
            {
                // TEXT mode on purpose: SaveScene writes through a text-mode
                // std::ofstream, so every '\n' in dump(2) hits the disk as CRLF.
                // Reading back in text mode reverses that translation and yields
                // exactly the bytes dump(2) produced - which is what we are
                // diffing. Reading binary here compares dump() against CRLF and
                // fails at byte 1 for reasons that have nothing to do with UI.
                std::ifstream in(saved);
                liveText.assign((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            }
            json live = json::parse(liveText);
            // Idempotence sanity: a re-dump must reproduce the file exactly, so
            // that a substitution diff below is meaningful.
            expect(live.dump(2) == liveText, "SaveScene output must re-dump identically");
            // Substitute every UI sub-object with the FROZEN writer's output,
            // keyed by guid so entity ORDER (which BuildSceneJson reverses) is
            // irrelevant. If the extraction changed a single byte, the rebuilt
            // text differs and we report the offset.
            std::unordered_map<std::string, const scene::EntityData*> byGuid;
            for (const scene::EntityData& d : data.entities)
                if (d.guid != 0) byGuid[guid::ToHex(d.guid)] = &d;
            u32 substituted = 0;
            for (json& je : live["entities"]) {
                const auto g = je.find("guid");
                if (g == je.end()) continue;
                const auto found = byGuid.find(g->get<std::string>());
                if (found == byGuid.end()) continue;
                const scene::EntityData& d = *found->second;
                if (d.hasUI) { je["ui"] = FrozenElement(&d.uiElement); ++substituted; }
                if (d.hasUICanvas) { je["uiCanvas"] = FrozenCanvas(&d.uiCanvas); ++substituted; }
                if (d.hasUIAnimator) { je["uiAnimator"] = FrozenAnimator(&d.uiAnimator); ++substituted; }
                if (d.hasUIPanel) { je["uiPanel"] = FrozenPanel(&d.uiPanel); ++substituted; }
                if (d.hasUILayoutGroup) { je["uiLayoutGroup"] = FrozenLayout(&d.uiLayoutGroup); ++substituted; }
                if (d.hasUICanvasGroup) { je["uiCanvasGroup"] = FrozenGroup(&d.uiCanvasGroup); ++substituted; }
                if (d.hasWorldText) { je["worldText"] = FrozenWorldText(&d.worldText); ++substituted; }
            }
            expect(substituted == uiBlocks,
                   "every authored UI block must be reachable by guid in the re-saved "
                   "file (" + std::to_string(substituted) + " of " +
                       std::to_string(uiBlocks) + ")");
            const std::string frozenText = live.dump(2);
            if (frozenText != liveText) {
                usize at = 0;
                while (at < frozenText.size() && at < liveText.size() &&
                       frozenText[at] == liveText[at])
                    ++at;
                HBE_ERROR("uidoc: '{}' differs from the frozen writers at byte {}",
                          scenePath.filename().string(), at);
            }
            expect(frozenText == liveText,
                   "SaveScene output for '" + scenePath.filename().string() +
                       "' is byte-identical to the pre-extraction writers");
            HBE_INFO("uidoc: {} - {} entities, {} UI blocks, {} bytes, byte-identical.",
                     scenePath.filename().string(), data.entities.size(), uiBlocks,
                     liveText.size());
        }
        fs::remove_all(dir, ec);
    }
    return ok;
}

// =============================================================================
// --test-uidoc-invariants: the P3 STRUCTURAL contract, checked against a real
// document opened into a real Scene. Everything here is a property of
// InstantiateDocument / the two sweep predicates / the two JSON writers, which
// is why it is asserted rather than described.
// =============================================================================

bool DocumentInvariantsSelfTest(Scene& scene, Renderer* renderer, const fs::path& path,
                                bool preload) {
    bool ok = true;
    const auto expect = [&ok](bool cond, const std::string& what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("uidoc-invariants: FAILED - {}", what);
        }
    };

    DocData data;
    if (!LoadDocument(path, data)) {
        HBE_ERROR("uidoc-invariants: cannot load '{}'", path.string());
        return false;
    }
    expect(!data.entities.empty(), "the document under test has entities");

    auto& reg = scene.Registry();
    // A world entity that must be swept, so "spares documents" is proven against
    // a control rather than against an empty registry.
    const entt::entity world = scene.CreateEntity("__invariants_world");
    reg.emplace<Transform>(world);

    DocumentSet docs;
    const DocHandle h = docs.Open(scene, renderer, path, /*screenOwned*/ true, preload);
    expect(h != 0, "the document opens");
    if (h == 0) return false;
    // Non-const: the ordering half of check 4 edits the document the way the
    // editor does (destroy an element, create a replacement) and needs to keep the
    // instance's order list honest while doing it.
    DocumentInstance* inst = docs.Get(h);
    expect(inst != nullptr, "the open document is retrievable by handle");
    if (!inst) return false;
    expect(inst->entities.size() == data.entities.size(),
           "one live entity per document entity (" +
               std::to_string(inst->entities.size()) + " of " +
               std::to_string(data.entities.size()) + ")");

    // 1) Every created entity carries UIDocMember with THIS handle, and carries
    //    none of the world-content components. This is G3: the guarantee is a
    //    property of what InstantiateDocument can emplace, and the list below is
    //    the assertion that it did not grow.
    u32 members = 0;
    for (const entt::entity e : inst->entities) {
        expect(reg.valid(e), "a created document entity is valid");
        if (!reg.valid(e)) continue;
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        expect(m != nullptr && m->doc == h, "every created entity carries UIDocMember{doc}");
        expect(m != nullptr && m->screenOwned, "screenOwned rides in the component");
        if (m && m->doc == h) ++members;
        expect(!reg.all_of<MeshInstance>(e), "no document entity carries a MeshInstance");
        expect(!reg.all_of<RigidBody>(e), "no document entity carries a RigidBody");
        expect(!reg.all_of<SceneLayer>(e), "no document entity carries a SceneLayer");
        expect(!reg.all_of<SceneSource>(e), "no document entity carries a SceneSource");
        expect(!reg.all_of<Persistent>(e),
               "a document entity does NOT need the Persistent decoration - "
               "UIDocMember::screenOwned replaced it");
        expect(!reg.all_of<WorldText>(e),
               "worldText is level signage and is not a document component");
    }
    expect(members == inst->entities.size(), "the whole set is tagged with one handle");

    // 2) Neither scene writer emits a document entity (B11). Checked on the TEXT
    //    because that is what lands in a .hbscene / .hbprefab / .hbsave.
    {
        const std::string sceneText = scene::SaveSceneToString(scene);
        for (const std::string& key : DocumentComponentKeys())
            expect(sceneText.find("\"" + key + "\"") == std::string::npos,
                   "BuildSceneJson writes no '" + key + "' while a document is open");
        // The control world entity IS written, so this is not vacuous.
        expect(sceneText.find("__invariants_world") != std::string::npos,
               "BuildSceneJson still writes ordinary world entities");
        // A subtree copy of a document element is a legitimate INTRA-document
        // copy (that is how a widget gets duplicated), so it must produce real
        // entities - it used to produce `{"version":1,"entities":[]}`, which is
        // non-empty as a string, so every caller's emptiness guard missed it and
        // Ctrl+C/Ctrl+D on a menu button silently did nothing. It must still never
        // carry the membership tag out of the document (the paste side re-stamps
        // it, and the paste guard refuses the fragment into a scene).
        const std::string sub = scene::SaveSubtreeToString(scene, inst->entities.front());
        expect(sub.find("uiDoc") == std::string::npos,
               "BuildSubtreeJson never writes the document membership tag");
        {
            scene::SceneData subData;
            expect(scene::ParseSceneString(sub, subData) && !subData.entities.empty(),
                   "an intra-document subtree copy is NOT empty (Ctrl+C/Ctrl+D work)");
        }
        // ...and a WORLD root never drags document content along with it. Parent a
        // document element to the control world entity and copy the world root:
        // the fragment must contain the world entity and nothing from the document.
        {
            const entt::entity strayChild = inst->entities.front();
            const Parent had = reg.all_of<Parent>(strayChild) ? reg.get<Parent>(strayChild)
                                                             : Parent{entt::null};
            const bool hadParent = reg.all_of<Parent>(strayChild);
            reg.emplace_or_replace<Parent>(strayChild, Parent{world});
            const std::string worldSub = scene::SaveSubtreeToString(scene, world);
            scene::SceneData wd;
            expect(scene::ParseSceneString(worldSub, wd) && wd.entities.size() == 1,
                   "a WORLD subtree copy carries no document content across the boundary");
            if (hadParent) reg.emplace_or_replace<Parent>(strayChild, had);
            else reg.remove<Parent>(strayChild);
        }
    }

    // 3) A Replace sweep spares the document and destroys the world.
    {
        scene::SceneData empty;
        scene::StagedAssets staged;
        if (renderer)
            scene::Instantiate(scene, *renderer, empty, staged, scene::LoadMode::Replace);
        else {
            // No device: run the predicate itself rather than the loader.
            std::vector<entt::entity> kill;
            for (const entt::entity e : reg.storage<entt::entity>()) {
                const UIDocMember* m = reg.try_get<UIDocMember>(e);
                if (!reg.all_of<Persistent>(e) && !(m && m->screenOwned)) kill.push_back(e);
            }
            for (const entt::entity e : kill)
                if (reg.valid(e)) reg.destroy(e);
        }
        expect(!reg.valid(world) || !reg.all_of<Transform>(world),
               "a Replace sweep destroys ordinary world entities");
        u32 survivors = 0;
        for (const entt::entity e : inst->entities)
            if (reg.valid(e)) ++survivors;
        expect(survivors == inst->entities.size(),
               "a Replace sweep spares EVERY document entity (" +
                   std::to_string(survivors) + " of " +
                   std::to_string(inst->entities.size()) + ")");
    }

    // 4) Capture round-trips: what the editor would save equals what was loaded -
    //    ORDER INCLUDED, because entity order is z-order for canvas-less roots.
    {
        DocData captured;
        CaptureDocument(scene, h, inst->header, captured, &inst->entities);
        expect(captured.entities.size() == data.entities.size(),
               "capture recovers every entity");
        bool sameOrder = captured.entities.size() == data.entities.size();
        for (usize i = 0; sameOrder && i < captured.entities.size(); ++i)
            sameOrder = captured.entities[i].name == data.entities[i].name &&
                        captured.entities[i].parent == data.entities[i].parent &&
                        captured.entities[i].hasElement == data.entities[i].hasElement &&
                        captured.entities[i].hasPanel == data.entities[i].hasPanel;
        expect(sameOrder, "capture preserves DOCUMENT ORDER (name/parent/blocks, in place)");
        DocData reloaded;
        expect(LoadDocumentFromString(SaveDocumentToString(captured), reloaded),
               "a captured document re-parses");
        expect(SaveDocumentToString(reloaded) == SaveDocumentToString(captured),
               "capture -> save -> load -> save is stable");
        // THE RECYCLED-HANDLE CASE, which the old raw-handle sort got backwards.
        // Delete an element, then create TWO: the first (`__recycled`) gets the
        // freed INDEX back with a bumped VERSION, so its 32-bit integral value is
        // ~0x100000 or higher, while the second (`__brandnew`) takes a fresh low
        // index. Sorting by the raw handle therefore emits them in the WRONG order
        // - brandnew before recycled - silently inverting their draw/hit order
        // between the editing session and the reloaded file. Document order must
        // win over both.
        if (inst->entities.size() >= 2) {
            const usize before = inst->entities.size();
            const entt::entity victim = inst->entities.back();
            const std::string firstName = captured.entities.front().name;
            reg.destroy(victim);
            inst->entities.pop_back();
            const entt::entity recycled = scene.CreateEntity("__recycled");
            reg.emplace<UIElement>(recycled);
            docs.Track(scene, h, recycled);
            const entt::entity brandnew = scene.CreateEntity("__brandnew");
            reg.emplace<UIElement>(brandnew);
            docs.Track(scene, h, brandnew);
            // The premise of the check: the recycled handle really is numerically
            // above the fresh one (otherwise the assertion below proves nothing).
            expect(static_cast<u32>(recycled) > static_cast<u32>(brandnew),
                   "entt recycled a handle whose integral value is HIGHER (version bits)");
            DocData after;
            CaptureDocument(scene, h, inst->header, after, &inst->entities);
            expect(after.entities.size() == before + 1,
                   "capture after delete+create+create sees the whole document");
            const usize n = after.entities.size();
            expect(n >= 2 && after.entities[n - 2].name == "__recycled" &&
                       after.entities[n - 1].name == "__brandnew",
                   "capture keeps CREATION order across a handle recycle");
            expect(!after.entities.empty() && after.entities.front().name == firstName,
                   "the rest of the document keeps its order across a recycle");
        }
    }

    // 5) Close destroys the whole set and nothing else.
    {
        const usize n = inst->entities.size();
        const std::vector<entt::entity> was = inst->entities;
        docs.Close(scene, h);
        expect(docs.Get(h) == nullptr, "a closed document is forgotten");
        u32 alive = 0;
        for (const entt::entity e : was)
            if (reg.valid(e)) ++alive;
        expect(alive == 0, "Close destroys every document entity");
        expect(n > 0, "the test document was not empty");
    }
    return ok;
}

bool DocumentCanvasSelfTest(Scene& scene, Renderer& renderer, const fs::path& path) {
    bool ok = true;
    const auto expect = [&ok](bool cond, const std::string& what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("uicanvas: FAILED - {}", what);
        }
    };

    DocData data;
    if (!LoadDocument(path, data)) {
        HBE_ERROR("uicanvas: cannot load '{}'", path.string());
        return false;
    }
    const fs::path assetsDir =
        Project::HasActive() ? Project::Active().AssetsDir() : fs::path();

    auto& reg = scene.Registry();
    DocumentSet docs;
    const DocHandle h = docs.Open(scene, &renderer, path, /*screenOwned*/ true);
    expect(h != 0, "the document opens");
    if (h == 0) return false;

    const CanvasConfig cfg = data.canvas;
    const glm::vec2 targetSize(glm::max(cfg.refWidth, 64.0f), glm::max(cfg.refHeight, 64.0f));

    // A real document is a set of named SCREENS whose UIPanel::active is runtime
    // state - false on load, set only by UIManager as the game flow runs. So an
    // as-loaded document lays out NOTHING and the parity check below would be
    // comparing two empty streams and proving nothing. Force the first screen
    // visible exactly the way the editor does (the session-only EditorUIShow tag,
    // honoured only under EditorView), so the test runs over real geometry.
    scene.SetEditorView(true);
    scene.SetUIAuthoringView(true); // EditorUIShow is gated on BOTH (see Scene.h)
    {
        u32 panels = 0;
        for (const entt::entity e : reg.view<UIPanel>()) {
            const UIDocMember* m = reg.try_get<UIDocMember>(e);
            if (!m || m->doc != h) continue;
            if (panels++ == 0) reg.emplace_or_replace<EditorUIShow>(e);
        }
        if (panels > 0)
            HBE_INFO("uicanvas: {} screen(s) in the document; authoring the first.", panels);
    }

    // Does the document route anything away from the screen list? A world-space
    // canvas is the one shape where the runtime and the authoring build MUST
    // differ (that is the whole reason BuildDocumentVertices exists), so parity is
    // reported as inapplicable rather than silently weakened.
    u32 worldCanvases = 0;
    for (const entt::entity e : reg.view<UICanvas>())
        if (reg.get<UICanvas>(e).worldSpace) ++worldCanvases;

    // 1) VERTEX PARITY. The runtime build runs TWICE first: an auto-sized
    //    ScrollView measures its content from the PREVIOUS layout
    //    (contentExtent, "stable by the second frame"), so a single pass would be
    //    comparing a converged stream against an unconverged one and the mismatch
    //    would be an artifact of the harness, not a drift.
    std::vector<rhi::UIVertex> runtimeVerts;
    std::vector<WorldUIBatch> batches;
    BuildVertices(scene, renderer, assetsDir, targetSize, cfg, runtimeVerts, &batches);
    BuildVertices(scene, renderer, assetsDir, targetSize, cfg, runtimeVerts, &batches);

    std::vector<rhi::UIVertex> docVerts;
    std::vector<LayoutItem> docLayout;
    BuildDocumentVertices(scene, renderer, assetsDir, targetSize, cfg, h, docVerts, docLayout);

    expect(!docLayout.empty(), "the authoring build lays out at least one element");
    if (worldCanvases == 0) {
        expect(docVerts.size() == runtimeVerts.size(),
               "vertex COUNT parity with the runtime build (" +
                   std::to_string(docVerts.size()) + " vs " +
                   std::to_string(runtimeVerts.size()) + ")");
        if (docVerts.size() == runtimeVerts.size()) {
            const bool same =
                docVerts.empty() ||
                std::memcmp(docVerts.data(), runtimeVerts.data(),
                            docVerts.size() * sizeof(rhi::UIVertex)) == 0;
            expect(same, "the authoring canvas is BYTE-IDENTICAL to the shipped UI pass");
        }
    } else {
        HBE_INFO("uicanvas: {} world-space canvas(es) present - byte parity is "
                 "inapplicable by design (the runtime routes those items into a "
                 "texture batch; the authoring build shows them on the canvas).",
                 worldCanvases);
    }

    // 2) LAYOUT PARITY: `docFilter` is exact, and inert at 0.
    {
        std::vector<LayoutItem> plain;
        LayoutUI(scene, targetSize, cfg, plain);
        expect(plain.size() == docLayout.size(),
               "the document-filtered layout matches the unfiltered one item for item (" +
                   std::to_string(docLayout.size()) + " vs " + std::to_string(plain.size()) +
                   ")");
        if (plain.size() == docLayout.size()) {
            bool same = true;
            for (usize i = 0; i < plain.size(); ++i) {
                const LayoutItem& a = plain[i];
                const LayoutItem& b = docLayout[i];
                if (a.entity != b.entity || a.rect.x0 != b.rect.x0 || a.rect.y0 != b.rect.y0 ||
                    a.rect.x1 != b.rect.x1 || a.rect.y1 != b.rect.y1 ||
                    a.canvas != b.canvas || a.parentEntity != b.parentEntity) {
                    same = false;
                    break;
                }
            }
            expect(same, "every filtered layout item has the same entity, rect, canvas "
                         "and parent as the unfiltered walk");
        }
    }

    // 3) DOCUMENT SCOPING. A second open document must be invisible to the first.
    {
        DocData other;
        DocEntity de;
        de.name = "__uicanvas_other";
        de.hasElement = true;
        de.element.type = UIElement::Type::Panel;
        de.element.size = {120.0f, 60.0f};
        de.element.color = {1.0f, 0.0f, 1.0f, 1.0f};
        other.entities.push_back(de);
        other.canvas = cfg;
        const DocHandle h2 = docs.OpenFromData(scene, &renderer, other,
                                               fs::path("__uicanvas_other.hbui"),
                                               /*screenOwned*/ true, /*preload*/ false);
        expect(h2 != 0 && h2 != h, "a second independent document opens");
        if (h2 != 0) {
            std::vector<rhi::UIVertex> againVerts;
            std::vector<LayoutItem> againLayout;
            BuildDocumentVertices(scene, renderer, assetsDir, targetSize, cfg, h,
                                  againVerts, againLayout);
            expect(againVerts.size() == docVerts.size() &&
                       (docVerts.empty() ||
                        std::memcmp(againVerts.data(), docVerts.data(),
                                    docVerts.size() * sizeof(rhi::UIVertex)) == 0),
                   "opening a SECOND document does not change the first document's "
                   "authoring canvas");
            bool leaked = false;
            for (const LayoutItem& it : againLayout) {
                const UIDocMember* m = reg.try_get<UIDocMember>(it.entity);
                if (!m || m->doc != h) leaked = true;
            }
            expect(!leaked, "the authoring layout contains ONLY the target document");

            std::vector<rhi::UIVertex> otherVerts;
            std::vector<LayoutItem> otherLayout;
            BuildDocumentVertices(scene, renderer, assetsDir, targetSize, cfg, h2,
                                  otherVerts, otherLayout);
            expect(otherLayout.size() == 1,
                   "the second document lays out exactly its one element (" +
                       std::to_string(otherLayout.size()) + ")");
            expect(!otherVerts.empty(), "the second document emits geometry");
            docs.Close(scene, h2);
        }
    }

    // 4) The authoring render target is real and presentable to ImGui.
    {
        const u32 w = static_cast<u32>(glm::clamp(targetSize.x, 64.0f, 4096.0f));
        const u32 hh = static_cast<u32>(glm::clamp(targetSize.y, 64.0f, 4096.0f));
        const std::string key =
            "uiedit@" + std::to_string(w) + "x" + std::to_string(hh);
        const rhi::TextureHandle rt = AcquireUITarget(renderer, key, w, hh);
        const rhi::TextureHandle again = AcquireUITarget(renderer, key, w, hh);
        const bool needTarget = renderer.API() == rhi::GraphicsAPI::D3D12 ||
                                renderer.API() == rhi::GraphicsAPI::Vulkan;
        if (needTarget) {
            expect(rt.IsValid(), "AcquireUITarget mints an authoring render target");
            expect(again.index == rt.index,
                   "the same key RE-ADOPTS the target instead of burning a second "
                   "slot (the RHI can never free one)");
            expect(renderer.TextureUIId(rt) != 0,
                   "the authoring target can be handed to ImGui as a texture id");
        } else {
            HBE_INFO("uicanvas: backend {} has no UI render target - the panel degrades "
                     "to a frame + a message by design.",
                     rhi::ToString(renderer.API()));
        }
    }

    docs.Close(scene, h);
    return ok;
}

} // namespace hbe::ui
