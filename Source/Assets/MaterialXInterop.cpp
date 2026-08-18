// Assets/MaterialXInterop.cpp - .mtlx <-> .hbmat via MaterialXCore + MaterialXFormat (editor-only).
#include "Assets/MaterialXInterop.h"

#include "Core/Log.h"
#include "RHI/MaterialCompiler.h" // material::ComputeShaderVariant (--test-openpbr routing checks)
#include "RHI/RHI.h"              // rhi::MaterialFlag_Transparent

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

#ifdef HBE_HAS_MATERIALX
#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
namespace mx = MaterialX;
#endif

namespace hbe::assets {

bool MaterialXAvailable() {
#ifdef HBE_HAS_MATERIALX
    return true;
#else
    return false;
#endif
}

#ifdef HBE_HAS_MATERIALX
namespace {

glm::vec3 ToVec3(const mx::Color3& c) { return glm::vec3(c[0], c[1], c[2]); }

// Read a value input by name. Returns `def` when the input is absent, connected-only (no constant
// value), or of a different type - so a partial .mtlx never corrupts a field, it just keeps the
// OpenPBR default already sitting in SurfaceParams.
float GetFloat(const mx::NodePtr& n, const std::string& name, float def) {
    if (auto in = n->getInput(name))
        if (auto v = in->getValue())
            if (v->isA<float>()) return v->asA<float>();
    return def;
}
glm::vec3 GetColor(const mx::NodePtr& n, const std::string& name, glm::vec3 def) {
    if (auto in = n->getInput(name))
        if (auto v = in->getValue())
            if (v->isA<mx::Color3>()) return ToVec3(v->asA<mx::Color3>());
    return def;
}
bool GetBool(const mx::NodePtr& n, const std::string& name, bool def) {
    if (auto in = n->getInput(name))
        if (auto v = in->getValue())
            if (v->isA<bool>()) return v->asA<bool>();
    return def;
}
// If `name` is connected to an <image>/<tiledimage>, return its "file" (as authored). getValueString
// is type-agnostic (filename values serialise straight to the path string).
std::string GetTexture(const mx::NodePtr& n, const std::string& name) {
    auto in = n->getInput(name);
    if (!in) return {};
    mx::NodePtr src = in->getConnectedNode();
    if (!src) return {};
    const std::string cat = src->getCategory();
    if (cat != "image" && cat != "tiledimage") return {};
    if (auto f = src->getInput("file")) return f->getValueString();
    return {};
}

} // namespace
#endif // HBE_HAS_MATERIALX

std::optional<MaterialAsset> ImportMaterialX(const std::filesystem::path& mtlx, std::string& error) {
#ifndef HBE_HAS_MATERIALX
    (void)mtlx;
    error = "MaterialX support was not built into this editor (HBE_ENABLE_MATERIALX was off).";
    return std::nullopt;
#else
    mx::DocumentPtr doc = mx::createDocument();
    try {
        mx::readFromXmlFile(doc, mtlx.string());
    } catch (const std::exception& e) {
        error = std::string("failed to read .mtlx: ") + e.what();
        return std::nullopt;
    }
    // First surface-shader node in the document (a DCC writes the node's type="surfaceshader" attr,
    // so this resolves without loading the MaterialX standard libraries).
    mx::NodePtr surf;
    for (const mx::NodePtr& n : doc->getNodes())
        if (n->getType() == "surfaceshader") { surf = n; break; }
    if (!surf) {
        error = "no surface-shader node (open_pbr_surface / standard_surface) found in the document.";
        return std::nullopt;
    }

    MaterialAsset mat;
    mat.name = surf->getName();
    SurfaceParams& s = mat.surface;
    const std::string cat = surf->getCategory();
    if (cat == "standard_surface") {
        // Autodesk standard_surface -> OpenPBR (approximate; the input NAMES differ).
        s.base_weight        = GetFloat(surf, "base", s.base_weight);
        s.base_color         = glm::vec4(GetColor(surf, "base_color", glm::vec3(s.base_color)), s.base_color.a);
        s.base_metalness     = GetFloat(surf, "metalness", s.base_metalness);
        s.specular_weight    = GetFloat(surf, "specular", s.specular_weight);
        s.specular_color     = GetColor(surf, "specular_color", s.specular_color);
        s.specular_roughness = GetFloat(surf, "specular_roughness", s.specular_roughness);
        s.specular_ior       = GetFloat(surf, "specular_IOR", s.specular_ior);
        s.specular_roughness_anisotropy = GetFloat(surf, "specular_anisotropy", s.specular_roughness_anisotropy);
        s.transmission_weight = GetFloat(surf, "transmission", s.transmission_weight);
        s.transmission_color  = GetColor(surf, "transmission_color", s.transmission_color);
        s.coat_weight        = GetFloat(surf, "coat", s.coat_weight);
        s.coat_color         = GetColor(surf, "coat_color", s.coat_color);
        s.coat_roughness     = GetFloat(surf, "coat_roughness", s.coat_roughness);
        s.coat_ior           = GetFloat(surf, "coat_IOR", s.coat_ior);
        s.fuzz_weight        = GetFloat(surf, "sheen", s.fuzz_weight);
        s.fuzz_color         = GetColor(surf, "sheen_color", s.fuzz_color);
        s.fuzz_roughness     = GetFloat(surf, "sheen_roughness", s.fuzz_roughness);
        s.subsurface_weight  = GetFloat(surf, "subsurface", s.subsurface_weight);
        s.subsurface_color   = GetColor(surf, "subsurface_color", s.subsurface_color);
        s.thin_film_thickness = GetFloat(surf, "thin_film_thickness", s.thin_film_thickness);
        s.thin_film_ior      = GetFloat(surf, "thin_film_IOR", s.thin_film_ior);
        s.emission_luminance = GetFloat(surf, "emission", s.emission_luminance);
        s.emission_color     = GetColor(surf, "emission_color", s.emission_color);
        s.thin_walled        = GetBool(surf, "thin_walled", s.thin_walled);
    } else {
        // open_pbr_surface (or any node using OpenPBR input names) -> SurfaceParams (1:1: both use
        // the OpenPBR Surface parameter names).
        s.base_weight        = GetFloat(surf, "base_weight", s.base_weight);
        s.base_color         = glm::vec4(GetColor(surf, "base_color", glm::vec3(s.base_color)), s.base_color.a);
        s.base_metalness     = GetFloat(surf, "base_metalness", s.base_metalness);
        s.base_diffuse_roughness = GetFloat(surf, "base_diffuse_roughness", s.base_diffuse_roughness);
        s.specular_weight    = GetFloat(surf, "specular_weight", s.specular_weight);
        s.specular_color     = GetColor(surf, "specular_color", s.specular_color);
        s.specular_roughness = GetFloat(surf, "specular_roughness", s.specular_roughness);
        s.specular_ior       = GetFloat(surf, "specular_ior", s.specular_ior);
        s.specular_roughness_anisotropy = GetFloat(surf, "specular_roughness_anisotropy", s.specular_roughness_anisotropy);
        s.transmission_weight = GetFloat(surf, "transmission_weight", s.transmission_weight);
        s.transmission_color  = GetColor(surf, "transmission_color", s.transmission_color);
        s.transmission_depth  = GetFloat(surf, "transmission_depth", s.transmission_depth);
        s.coat_weight        = GetFloat(surf, "coat_weight", s.coat_weight);
        s.coat_color         = GetColor(surf, "coat_color", s.coat_color);
        s.coat_roughness     = GetFloat(surf, "coat_roughness", s.coat_roughness);
        s.coat_ior           = GetFloat(surf, "coat_ior", s.coat_ior);
        s.fuzz_weight        = GetFloat(surf, "fuzz_weight", s.fuzz_weight);
        s.fuzz_color         = GetColor(surf, "fuzz_color", s.fuzz_color);
        s.fuzz_roughness     = GetFloat(surf, "fuzz_roughness", s.fuzz_roughness);
        s.subsurface_weight  = GetFloat(surf, "subsurface_weight", s.subsurface_weight);
        s.subsurface_color   = GetColor(surf, "subsurface_color", s.subsurface_color);
        s.subsurface_radius  = GetFloat(surf, "subsurface_radius_scale", s.subsurface_radius); // scalar approx
        s.thin_film_weight   = GetFloat(surf, "thin_film_weight", s.thin_film_weight);
        s.thin_film_thickness = GetFloat(surf, "thin_film_thickness", s.thin_film_thickness);
        s.thin_film_ior      = GetFloat(surf, "thin_film_ior", s.thin_film_ior);
        s.emission_luminance = GetFloat(surf, "emission_luminance", s.emission_luminance);
        s.emission_color     = GetColor(surf, "emission_color", s.emission_color);
        s.thin_walled        = GetBool(surf, "geometry_thin_walled", s.thin_walled);
    }
    // Texture inputs on the two colour channels we can map unambiguously (base_color is the same
    // input name in both node types). Normal / metal-rough wiring in MaterialX goes through
    // intermediate nodes and is left to the .hbmat authoring pass.
    mat.albedoTex = GetTexture(surf, "base_color");
    mat.emissiveTex = GetTexture(surf, "emission_color");
    // A transmissive material must draw in the alpha-blended pass (where the OpenPBR refraction runs).
    if (s.transmission_weight > 0.0f) mat.flags |= rhi::MaterialFlag_Transparent;
    HBE_INFO("MaterialX: imported '{}' ({}) from '{}'.", mat.name, cat, mtlx.string());
    return mat;
#endif
}

bool ExportMaterialX(const MaterialAsset& mat, const std::filesystem::path& mtlx, std::string& error) {
#ifndef HBE_HAS_MATERIALX
    (void)mat;
    (void)mtlx;
    error = "MaterialX support was not built into this editor (HBE_ENABLE_MATERIALX was off).";
    return false;
#else
    const SurfaceParams& s = mat.surface;
    mx::DocumentPtr doc = mx::createDocument();

    // MaterialX identifiers can't contain spaces / arbitrary punctuation.
    std::string base = mat.name.empty() ? std::string("Material") : mat.name;
    for (char& c : base)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';

    mx::NodePtr surf = doc->addNode("open_pbr_surface", base + "_surface", "surfaceshader");
    auto setF = [&](const char* n, float v) { surf->setInputValue(n, v); };
    auto setC = [&](const char* n, glm::vec3 v) { surf->setInputValue(n, mx::Color3(v.x, v.y, v.z)); };
    setF("base_weight", s.base_weight);
    setC("base_color", glm::vec3(s.base_color));
    setF("base_metalness", s.base_metalness);
    setF("base_diffuse_roughness", s.base_diffuse_roughness);
    setF("specular_weight", s.specular_weight);
    setC("specular_color", s.specular_color);
    setF("specular_roughness", s.specular_roughness);
    setF("specular_ior", s.specular_ior);
    setF("specular_roughness_anisotropy", s.specular_roughness_anisotropy);
    setF("transmission_weight", s.transmission_weight);
    setC("transmission_color", s.transmission_color);
    setF("transmission_depth", s.transmission_depth);
    setF("coat_weight", s.coat_weight);
    setC("coat_color", s.coat_color);
    setF("coat_roughness", s.coat_roughness);
    setF("coat_ior", s.coat_ior);
    setF("fuzz_weight", s.fuzz_weight);
    setC("fuzz_color", s.fuzz_color);
    setF("fuzz_roughness", s.fuzz_roughness);
    setF("subsurface_weight", s.subsurface_weight);
    setC("subsurface_color", s.subsurface_color);
    setF("thin_film_weight", s.thin_film_weight);
    setF("thin_film_thickness", s.thin_film_thickness);
    setF("thin_film_ior", s.thin_film_ior);
    setF("emission_luminance", s.emission_luminance);
    setC("emission_color", s.emission_color);
    surf->setInputValue("geometry_thin_walled", s.thin_walled);

    mx::NodePtr material = doc->addNode("surfacematerial", base, "material");
    mx::InputPtr shaderIn = material->addInput("surfaceshader", "surfaceshader");
    shaderIn->setNodeName(surf->getName());

    try {
        mx::writeToXmlFile(doc, mtlx.string());
    } catch (const std::exception& e) {
        error = std::string("failed to write .mtlx: ") + e.what();
        return false;
    }
    HBE_INFO("MaterialX: exported '{}' -> '{}'.", mat.name, mtlx.string());
    return true;
#endif
}

bool MaterialInteropSelfTest() {
    namespace fs = std::filesystem;
    bool ok = true;
    const auto check = [&](bool cond, const char* what) {
        if (!cond) {
            HBE_ERROR("MaterialSelfTest: FAIL - {}", what);
            ok = false;
        }
    };
    const auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };

    // A distinctive material that exercises every OpenPBR lobe (so a dropped/renamed field shows up).
    MaterialAsset m;
    m.name = "SelfTestGlass";
    SurfaceParams& s = m.surface;
    s.base_color = {0.8f, 0.2f, 0.1f, 1.0f};
    s.base_metalness = 0.3f;
    s.specular_roughness = 0.22f;
    s.specular_ior = 1.45f;
    s.specular_roughness_anisotropy = 0.4f;
    s.transmission_weight = 0.9f;
    s.transmission_color = {0.7f, 0.9f, 1.0f};
    s.coat_weight = 0.5f;
    s.coat_roughness = 0.1f;
    s.coat_ior = 1.6f;
    s.fuzz_weight = 0.25f;
    s.fuzz_color = {0.5f, 0.5f, 0.6f};
    s.fuzz_roughness = 0.4f;
    s.subsurface_weight = 0.15f;
    s.thin_film_weight = 0.6f;
    s.thin_film_thickness = 0.45f;
    s.thin_film_ior = 1.4f;
    s.emission_color = {0.1f, 0.0f, 0.0f};
    s.emission_luminance = 2.0f;
    s.thin_walled = false;
    m.flags = rhi::MaterialFlag_Transparent;

    const fs::path tmp = fs::temp_directory_path();
    const fs::path hb = tmp / "hbe_selftest.hbmat";

    // (1) .hbmat round-trip.
    check(SaveMaterial(hb, m), "SaveMaterial");
    if (auto loaded = LoadMaterial(hb)) {
        const SurfaceParams& t = loaded->surface;
        check(approx(t.transmission_weight, 0.9f) && approx(t.specular_ior, 1.45f) &&
                  approx(t.thin_film_thickness, 0.45f) && approx(t.coat_ior, 1.6f) &&
                  approx(t.fuzz_weight, 0.25f) && t.thin_walled == false,
              ".hbmat round-trip values");
        check(loaded->flags == m.flags, ".hbmat flags round-trip");
    } else {
        check(false, "LoadMaterial");
    }

    // (1b) A MALFORMED .hbmat must not crash LoadMaterial - nulls / wrong types fall back to defaults
    // (regression guard for the +inf-attenuation -> JSON-null crash and hand-edited files).
    {
        const fs::path bad = tmp / "hbe_selftest_bad.hbmat";
        {
            std::ofstream os(bad);
            os << R"({"name":"Bad","metallic":null,"transmission_depth":null,"roughness":"oops",)"
                  R"("baseColor":[1,null,0.5,1],"flags":null,"textures":{"albedo":null}})";
        }
        if (auto badLoaded = LoadMaterial(bad)) {
            check(approx(badLoaded->surface.base_metalness, 0.0f) &&
                      approx(badLoaded->surface.specular_roughness, 0.5f) &&
                      approx(badLoaded->surface.base_color.x, 1.0f),
                  "malformed .hbmat fields fall back to defaults");
        } else {
            check(false, "malformed .hbmat failed to load (should fall back, not fail)");
        }
        std::error_code bec;
        fs::remove(bad, bec);
    }

    // (2) Shader-variant routing (the specialization contract).
    check(material::ComputeShaderVariant(SurfaceParams{}, 0u) == rhi::ShaderVariant::Std,
          "default -> Std");
    check(material::ComputeShaderVariant(s, m.flags) == rhi::ShaderVariant::Full,
          "transmission+multi-lobe -> Full");
    {
        SurfaceParams coatOnly;
        coatOnly.coat_weight = 0.5f;
        check(material::ComputeShaderVariant(coatOnly, 0u) == rhi::ShaderVariant::Coat,
              "coat-only -> Coat");
    }

    // (3) .mtlx round-trip (only when MaterialX is linked; also proves the DLLs load + execute).
    if (MaterialXAvailable()) {
        const fs::path mx = tmp / "hbe_selftest.mtlx";
        std::string err;
        check(ExportMaterialX(m, mx, err), err.empty() ? "ExportMaterialX" : err.c_str());
        if (auto reimported = ImportMaterialX(mx, err)) {
            const SurfaceParams& t = reimported->surface;
            check(approx(t.transmission_weight, 0.9f) && approx(t.specular_ior, 1.45f) &&
                      approx(t.thin_film_weight, 0.6f) && approx(t.coat_ior, 1.6f) &&
                      approx(t.base_metalness, 0.3f) && approx(t.emission_luminance, 2.0f),
                  ".mtlx round-trip values");
            check((reimported->flags & rhi::MaterialFlag_Transparent) != 0u,
                  ".mtlx transmissive -> Transparent flag");
        } else {
            check(false, err.empty() ? "ImportMaterialX" : err.c_str());
        }
        std::error_code ec;
        fs::remove(mx, ec);
    } else {
        HBE_INFO("MaterialSelfTest: MaterialX not built - skipping .mtlx round-trip.");
    }

    std::error_code ec;
    fs::remove(hb, ec);
    return ok;
}

} // namespace hbe::assets
