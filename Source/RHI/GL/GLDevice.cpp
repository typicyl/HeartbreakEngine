// RHI/GL/GLDevice.cpp - OpenGL 4.6 RHI backend.
//
// Scene rendering at growing parity with D3D12/Vulkan. A GL 4.6 core context
// drives a forward metallic-roughness mesh pass (material maps + image-based
// lighting) into an RGBA16F HDR target, an equirect sky background, then an ACES
// tonemap blit to the back buffer. Implemented here: the GL entry-point loader,
// VBO/IBO/VAO meshes, per-draw material textures, a std140 frame UBO, sampler
// objects (material REPEAT / equirect wrap-clamp / LUT clamp), and DrawScene.
// GLSL shaders are hand-authored (Windows has no SPIR-V->GLSL path vendored and
// GL has no runtime descriptor arrays to consume the bindless Vulkan SPIR-V).
//
// Still TODO toward full parity (later increments): punctual/area lights, local
// probes + the SH-GI volume, cascaded shadows, the rest of the HDR post stack
// (bloom/SSAO/TAA/DoF/SSR/fog/SSGI/painterly), particles, the UI overlay,
// skinning, and ARB_bindless_texture so post passes index a texture array.
#include "RHI/GL/GLDevice.h"
#include "RHI/GL/GLApi.h"
#include "Assets/Mesh.h"
#include "Core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace hbe::rhi {
namespace {

using namespace hbe::rhi::gl; // loaded GL 2.0+ entry points

// WGL ARB context creation (modern core profile); not in <GL/gl.h>.
using PFNWGLCREATECONTEXTATTRIBSARBPROC = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB    = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB    = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB     = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

// GL 1.1 constants used below that some <GL/gl.h> still omit.
#ifndef GL_DEPTH_COMPONENT
#  define GL_DEPTH_COMPONENT 0x1902
#endif
#ifndef GL_LEQUAL
#  define GL_LEQUAL 0x0203
#endif

// std140 per-frame constants (matches the Frame block in the GLSL below).
struct FrameUBO {
    glm::mat4 viewProj{1.0f};
    glm::mat4 invViewProj{1.0f};
    glm::vec4 camPos{0.0f};     // xyz
    glm::vec4 lightDir{0.0f};   // xyz dir (normalized), w intensity
    glm::vec4 lightColor{0.0f}; // xyz color, w ambient intensity
    glm::vec4 ibl{0.0f};        // x = prefilteredMaxLod, y = hasIBL, z = hasSky
};

struct GlMesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLsizei indexCount = 0;
    bool alive = false;
};

// ---- GLSL 4.60 sources ----------------------------------------------------

// Shared frame block + equirect mapping, prepended to every program.
const char* kCommon = R"GLSL(#version 460 core
layout(std140, binding=0) uniform Frame {
    mat4 uViewProj;
    mat4 uInvViewProj;
    vec4 uCamPos;
    vec4 uLightDir;   // xyz dir, w intensity
    vec4 uLightColor; // xyz color, w ambient
    vec4 uIbl;        // x=prefilteredMaxLod, y=hasIBL, z=hasSky
};
const float PI = 3.14159265359;
const float EPS = 1e-4;
vec2 EquirectUV(vec3 d){
    float u = atan(d.z, d.x) * 0.15915494 + 0.5;
    float v = acos(clamp(d.y, -1.0, 1.0)) * 0.31830989;
    return vec2(u, v);
}
)GLSL";

const char* kMeshVS = R"GLSL(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aTangent;
layout(location=3) in vec2 aUV;
uniform mat4 uModel;
uniform mat3 uNormalMat;
out vec3 vWorldPos;
out vec3 vNormal;
out vec4 vTangent;
out vec2 vUV;
void main(){
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(uNormalMat * aNormal);
    vTangent = vec4(normalize(mat3(uModel) * aTangent.xyz), aTangent.w);
    vUV = aUV;
    gl_Position = uViewProj * wp;
}
)GLSL";

const char* kMeshFS = R"GLSL(
in vec3 vWorldPos;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUV;
out vec4 oColor;
uniform vec4  uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3  uEmissive;
uniform int   uHasAlbedo, uHasNormal, uHasMR, uHasAO, uHasEmissiveTex;
uniform sampler2D uAlbedo, uNormalTex, uMR, uAO, uEmissiveTex; // units 0..4
uniform sampler2D uIrr, uPre, uBrdf;                          // units 5..7
float D_GGX(float NoH, float a){ float a2=a*a; float d=NoH*NoH*(a2-1.0)+1.0; return a2/(PI*d*d); }
float G_Smith(float NoV,float NoL,float r){ float k=(r+1.0); k=k*k/8.0;
    float gv=NoV/(NoV*(1.0-k)+k); float gl=NoL/(NoL*(1.0-k)+k); return gv*gl; }
vec3 F_Schlick(float c, vec3 f0){ return f0 + (1.0-f0)*pow(1.0-c,5.0); }
vec3 F_SchlickRough(float c, vec3 f0, float r){ vec3 fr=max(vec3(1.0-r),f0); return f0+(fr-f0)*pow(clamp(1.0-c,0.0,1.0),5.0); }
vec3 applyNormal(vec3 N, vec4 T, vec2 uv){
    vec3 tn = texture(uNormalTex, uv).xyz * 2.0 - 1.0;
    vec3 n = normalize(N);
    vec3 t = normalize(T.xyz - n*dot(n,T.xyz));
    vec3 b = cross(n,t) * T.w;
    return normalize(mat3(t,b,n) * tn);
}
void main(){
    vec3 albedo = uBaseColor.rgb * (uHasAlbedo==1 ? texture(uAlbedo,vUV).rgb : vec3(1.0));
    float metallic = uMetallic, roughness = uRoughness;
    if(uHasMR==1){ vec3 mr = texture(uMR,vUV).rgb; metallic *= mr.b; roughness *= mr.g; }
    metallic = clamp(metallic,0.0,1.0);
    roughness = clamp(roughness,0.04,1.0);
    float ao = uHasAO==1 ? texture(uAO,vUV).r : 1.0;
    vec3 gN = normalize(vNormal);
    vec3 N = uHasNormal==1 ? applyNormal(vNormal,vTangent,vUV) : gN;
    vec3 V = normalize(uCamPos.xyz - vWorldPos);
    float NoV = max(dot(N,V), EPS);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Direct sun (one shadow-less directional light for now).
    vec3 L = normalize(-uLightDir.xyz);
    vec3 H = normalize(V+L);
    float NoL=max(dot(N,L),0.0), NoH=max(dot(N,H),0.0), VoH=max(dot(V,H),0.0);
    vec3 Fd = F_Schlick(VoH, F0);
    vec3 spec = (D_GGX(NoH,roughness*roughness)*G_Smith(NoV,NoL,roughness)) * Fd / max(4.0*NoV*NoL,EPS);
    vec3 kd = (1.0-Fd)*(1.0-metallic);
    vec3 radiance = uLightColor.rgb * uLightDir.w;
    vec3 Lo = (kd*albedo/PI + spec) * radiance * NoL;

    // Ambient: image-based lighting (equirect irradiance + GGX-prefiltered specular).
    vec3 F = F_SchlickRough(NoV, F0, roughness);
    vec3 kD = (1.0-F)*(1.0-metallic);
    vec3 R = reflect(-V, N);
    float ambI = uLightColor.w;
    vec3 ambient;
    if(uIbl.y > 0.5){
        vec3 irradiance = texture(uIrr, EquirectUV(N)).rgb;
        vec3 diffuseIBL = irradiance * albedo;
        vec3 prefiltered = textureLod(uPre, EquirectUV(R), roughness*uIbl.x).rgb;
        vec2 brdf = texture(uBrdf, vec2(NoV, roughness)).rg;
        vec3 specIBL = prefiltered * (F*brdf.x + brdf.y);
        float specOcc = clamp(pow(NoV+ao, exp2(-16.0*roughness-1.0)) - 1.0 + ao, 0.0, 1.0);
        float horizon = clamp(1.0 + dot(R, gN), 0.0, 1.0);
        specIBL *= specOcc * horizon * horizon;
        ambient = (kD*diffuseIBL + specIBL) * ambI * ao;
    } else {
        ambient = kD * albedo * ambI * ao;
    }

    vec3 emissive = uEmissive * (uHasEmissiveTex==1 ? texture(uEmissiveTex,vUV).rgb : vec3(1.0));
    oColor = vec4(ambient + Lo + emissive, uBaseColor.a);
}
)GLSL";

const char* kSkyVS = R"GLSL(
out vec2 vNdc;
void main(){
    vec2 uv = vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2));
    vec2 ndc = uv*2.0 - 1.0;
    vNdc = ndc;
    gl_Position = vec4(ndc, 1.0, 1.0); // far plane
}
)GLSL";

const char* kSkyFS = R"GLSL(
in vec2 vNdc;
out vec4 oColor;
uniform sampler2D uSky;
void main(){
    vec4 pN = uInvViewProj * vec4(vNdc, 0.0, 1.0);
    vec4 pF = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(pF.xyz/pF.w - pN.xyz/pN.w);
    oColor = vec4(textureLod(uSky, EquirectUV(dir), 0.0).rgb, 1.0);
}
)GLSL";

// Present blit (no shared Frame block; standalone).
const char* kPresentVS = R"GLSL(#version 460 core
out vec2 vUV;
void main(){
    vec2 p = vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2));
    vUV = p;
    gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);
}
)GLSL";

const char* kPresentFS = R"GLSL(#version 460 core
in vec2 vUV;
out vec4 oColor;
uniform sampler2D uHDR;
uniform float uExposure;
vec3 aces(vec3 x){ return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14), 0.0, 1.0); }
void main(){
    vec3 hdr = texture(uHDR, vUV).rgb * uExposure;
    vec3 ldr = pow(aces(hdr), vec3(1.0/2.2));
    oColor = vec4(ldr, 1.0);
}
)GLSL";

// In-game UI overlay: NDC textured triangles, straight-alpha. texIndex is bindless
// in HLSL; here it is batched on the CPU (consecutive same-index triangles -> one draw).
const char* kUiVS = R"GLSL(#version 460 core
layout(location=0) in vec2 aPos; // NDC
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
out vec2 vUV;
out vec4 vColor;
void main(){ vUV = aUV; vColor = aColor; gl_Position = vec4(aPos, 0.0, 1.0); }
)GLSL";

const char* kUiFS = R"GLSL(#version 460 core
in vec2 vUV;
in vec4 vColor;
out vec4 oColor;
uniform sampler2D uTex;
void main(){ oColor = vColor * texture(uTex, vUV); }
)GLSL";

GLuint CompileShader(GLenum type, const char* const* parts, GLsizei n, const char* tag) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, n, parts, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        HBE_ERROR("[GL] {} compile failed: {}", tag, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Links a program from VS+FS, each optionally prefixed with the shared kCommon.
GLuint LinkProgram(const char* vsSrc, const char* fsSrc, bool shared, const char* tag) {
    const char* vsParts[2] = {kCommon, vsSrc};
    const char* fsParts[2] = {kCommon, fsSrc};
    GLuint vs = shared ? CompileShader(GL_VERTEX_SHADER, vsParts, 2, tag)
                       : CompileShader(GL_VERTEX_SHADER, &vsSrc, 1, tag);
    GLuint fs = shared ? CompileShader(GL_FRAGMENT_SHADER, fsParts, 2, tag)
                       : CompileShader(GL_FRAGMENT_SHADER, &fsSrc, 1, tag);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        HBE_ERROR("[GL] {} link failed: {}", tag, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void APIENTRY GLDebugCb(GLenum, GLenum, GLuint, GLenum severity, GLsizei,
                        const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    HBE_WARN("[GL] {}", message);
}

class GLDevice final : public IRenderDevice {
public:
    bool Init(const RenderDeviceDesc& desc) {
        hwnd_ = static_cast<HWND>(desc.windowHandle);
        if (!hwnd_) { HBE_ERROR("[GL] No native window handle provided."); return false; }
        hdc_ = GetDC(hwnd_);
        width_ = desc.width;
        height_ = desc.height;

        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        const int pf = ChoosePixelFormat(hdc_, &pfd);
        if (pf == 0 || !SetPixelFormat(hdc_, pf, &pfd)) {
            HBE_ERROR("[GL] Failed to set a pixel format."); return false;
        }

        HGLRC legacy = wglCreateContext(hdc_);
        if (!legacy) { HBE_ERROR("[GL] wglCreateContext failed."); return false; }
        wglMakeCurrent(hdc_, legacy);

        auto wglCreateContextAttribsARB = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
            wglGetProcAddress("wglCreateContextAttribsARB"));
        if (wglCreateContextAttribsARB) {
            const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
                                   WGL_CONTEXT_MINOR_VERSION_ARB, 6,
                                   WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
            if (HGLRC core = wglCreateContextAttribsARB(hdc_, nullptr, attribs)) {
                wglMakeCurrent(hdc_, core);
                wglDeleteContext(legacy);
                ctx_ = core;
            } else { ctx_ = legacy; }
        } else { ctx_ = legacy; }

        if (const GLubyte* r = glGetString(GL_RENDERER))
            adapter_ = reinterpret_cast<const char*>(r);
        const GLubyte* ver = glGetString(GL_VERSION);

        if (!LoadGLFunctions()) {
            // FAIL, do not "stay clear-only". Returning true here reported success to
            // Renderer::Initialize, which reports success to the boot loop, which
            // `break`s on the first backend that works - so a machine with a stale GL
            // ICD stopped the fallback chain HERE and the player stared at a blank
            // window forever, with no attempt at the backend that would have worked.
            // A device that cannot draw is not a device.
            HBE_ERROR("[GL] Required GL entry points unavailable - failing so the boot "
                      "chain can fall through to another backend.");
            return false;
        }

        if (desc.enableValidation && glDebugMessageCallback) {
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(&GLDebugCb, nullptr);
        }

        if (glClipControl) glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glViewport(0, 0, GLsizei(width_), GLsizei(height_));

        meshProg_    = LinkProgram(kMeshVS, kMeshFS, /*shared*/ true, "MeshPBR");
        skyProg_     = LinkProgram(kSkyVS, kSkyFS, /*shared*/ true, "Sky");
        presentProg_ = LinkProgram(kPresentVS, kPresentFS, /*shared*/ false, "Present");
        uiProg_      = LinkProgram(kUiVS, kUiFS, /*shared*/ false, "UI");
        if (!meshProg_ || !skyProg_ || !presentProg_ || !uiProg_) {
            // Same reasoning as the entry-point failure above: without these programs
            // this device can clear the screen and nothing else, which is worse than
            // no device at all because it ends the fallback chain.
            HBE_ERROR("[GL] Core programs failed to link - failing so the boot chain can "
                      "fall through to another backend.");
            return false;
        } else {
            CacheUniforms();
            CreateSamplers();
            glGenVertexArrays(1, &emptyVao_);
            CreateUIBuffers();
            CreateDefaultTexture();
            sceneOk_ = true;
        }

        HBE_INFO("[GL] OpenGL backend ready: {} ({}) [{}]", adapter_,
                 ver ? reinterpret_cast<const char*>(ver) : "?",
                 sceneOk_ ? "scene rendering" : "clear-only");
        return true;
    }

    ~GLDevice() override {
        if (ctx_) {
            for (auto& m : meshes_) if (m.alive) DestroyMesh(m);
            if (emptyVao_) glDeleteVertexArrays(1, &emptyVao_);
            if (uiVao_) glDeleteVertexArrays(1, &uiVao_);
            if (uiVbo_) glDeleteBuffers(1, &uiVbo_);
            if (frameUbo_) glDeleteBuffers(1, &frameUbo_);
            if (hdrFbo_) glDeleteFramebuffers(1, &hdrFbo_);
            GLuint samplers[3] = {sampMaterial_, sampEquirect_, sampLut_};
            glDeleteSamplers(3, samplers);
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(ctx_);
        }
        if (hdc_ && hwnd_) ReleaseDC(hwnd_, hdc_);
    }

    void BeginFrame() override { wglMakeCurrent(hdc_, ctx_); }

    void ClearBackBuffer(f32 r, f32 g, f32 b, f32 a) override {
        clear_ = {r, g, b, a};
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, GLsizei(width_), GLsizei(height_));
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void EndFrame() override { SwapBuffers(hdc_); }

    void Resize(u32 width, u32 height) override {
        if (width && height) { width_ = width; height_ = height; }
    }

    void WaitForGpuIdle() override { glFinish(); }

    bool SupportsSceneRendering() const override { return sceneOk_; }

    MeshHandle CreateMesh(const hbe::MeshData& data) override {
        if (!sceneOk_ || data.Empty()) return {};
        GlMesh m{};
        glGenVertexArrays(1, &m.vao);
        glGenBuffers(1, &m.vbo);
        glGenBuffers(1, &m.ibo);
        glBindVertexArray(m.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.vertices.size() * sizeof(hbe::Vertex)),
                     data.vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(data.indices.size() * sizeof(u32)),
                     data.indices.data(), GL_STATIC_DRAW);
        SetupVertexLayout();
        glBindVertexArray(0);
        m.indexCount = GLsizei(data.indices.size());
        m.alive = true;

        u32 id = 0;
        for (u32 i = 1; i < meshes_.size(); ++i) if (!meshes_[i].alive) { id = i; break; }
        if (id == 0) { id = u32(meshes_.size()); meshes_.resize(id + 1); }
        meshes_[id] = m;
        return MeshHandle{id};
    }

    void UpdateMesh(MeshHandle h, const hbe::MeshData& data) override {
        if (!sceneOk_ || !h.IsValid() || h.id >= meshes_.size() || data.Empty()) return;
        GlMesh& m = meshes_[h.id];
        if (!m.alive) return;
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.vertices.size() * sizeof(hbe::Vertex)),
                     data.vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(data.indices.size() * sizeof(u32)),
                     data.indices.data(), GL_STATIC_DRAW);
        m.indexCount = GLsizei(data.indices.size());
    }

    TextureHandle CreateTexture(const TextureDesc& desc) override {
        if (!sceneOk_) return {};
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        UploadTexture(desc);
        u32 index = u32(textures_.size());
        textures_.push_back(tex);
        return TextureHandle{index};
    }

    void UpdateTexture(TextureHandle h, const TextureDesc& desc) override {
        if (!sceneOk_ || !h.IsValid() || h.index >= textures_.size()) return;
        glBindTexture(GL_TEXTURE_2D, textures_[h.index]);
        UploadTexture(desc);
    }

    void DrawScene(const SceneView& view, const DrawItem* items, u32 count) override {
        if (!sceneOk_) return;
        EnsureHDR(width_, height_);

        glBindFramebuffer(GL_FRAMEBUFFER, hdrFbo_);
        glViewport(0, 0, GLsizei(width_), GLsizei(height_));
        glClearColor(clear_.x, clear_.y, clear_.z, clear_.w);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CW); // GL's lower-left origin inverts the engine's CCW front

        const bool hasIBL = view.irradianceIndex != 0 && view.irradianceIndex < textures_.size();
        const bool hasSky = view.skyIndex != 0 && view.skyIndex < textures_.size();

        FrameUBO fb{};
        fb.viewProj = view.viewProj;
        fb.invViewProj = view.invViewProj;
        fb.camPos = glm::vec4(view.cameraPos, 1.0f);
        fb.lightDir = glm::vec4(glm::normalize(view.light.direction), view.light.intensity);
        fb.lightColor = glm::vec4(view.light.color, view.ambientIntensity);
        fb.ibl = glm::vec4(view.prefilteredMaxLod, hasIBL ? 1.0f : 0.0f, hasSky ? 1.0f : 0.0f, 0.0f);
        if (!frameUbo_) glGenBuffers(1, &frameUbo_);
        glBindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(FrameUBO), &fb, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUbo_);

        // ---- Opaque mesh pass ----
        glUseProgram(meshProg_);
        // Material units (0..4) use the tiling sampler; IBL (5,6) equirect; LUT (7) clamp.
        for (GLuint u = 0; u <= 4; ++u) glBindSampler(u, sampMaterial_);
        glBindSampler(5, sampEquirect_);
        glBindSampler(6, sampEquirect_);
        glBindSampler(7, sampLut_);
        if (hasIBL) {
            BindUnit(5, view.irradianceIndex);
            BindUnit(6, view.prefilteredIndex);
            BindUnit(7, view.brdfLUTIndex);
        }

        for (u32 i = 0; i < count; ++i) {
            const DrawItem& it = items[i];
            if (!it.mesh.IsValid() || it.mesh.id >= meshes_.size()) continue;
            const GlMesh& m = meshes_[it.mesh.id];
            if (!m.alive) continue;
            glUniformMatrix4fv(uModel_, 1, GL_FALSE, glm::value_ptr(it.transform));
            glm::mat3 nrm = glm::inverseTranspose(glm::mat3(it.transform));
            glUniformMatrix3fv(uNormalMat_, 1, GL_FALSE, glm::value_ptr(nrm));
            glUniform4fv(uBaseColor_, 1, glm::value_ptr(it.baseColor));
            glUniform1f(uMetallic_, it.metallic);
            glUniform1f(uRoughness_, it.roughness);
            glm::vec3 emis = it.emissiveColor * it.emissiveIntensity;
            glUniform3fv(uEmissive_, 1, glm::value_ptr(emis));
            glUniform1i(uHasAlbedo_, BindMaterial(0, it.albedoTexture) ? 1 : 0);
            glUniform1i(uHasNormal_, BindMaterial(1, it.normalTexture) ? 1 : 0);
            glUniform1i(uHasMR_, BindMaterial(2, it.mrTexture) ? 1 : 0);
            glUniform1i(uHasAO_, BindMaterial(3, it.aoTexture) ? 1 : 0);
            glUniform1i(uHasEmissiveTex_, BindMaterial(4, it.emissiveTexture) ? 1 : 0);
            glBindVertexArray(m.vao);
            glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
        }
        glBindVertexArray(0);

        // ---- Sky background (only the pixels with no geometry) ----
        if (hasSky) {
            glUseProgram(skyProg_);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glBindSampler(0, sampEquirect_);
            BindUnit(0, view.skyIndex);
            glUniform1i(uSky_, 0);
            glBindVertexArray(emptyVao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
        }

        // ---- Tonemap + present ----
        for (GLuint u = 0; u <= 7; ++u) glBindSampler(u, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, GLsizei(width_), GLsizei(height_));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(presentProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdrColor_);
        glUniform1i(uHDR_, 0);
        glUniform1f(uExposure_, view.exposure);
        glBindVertexArray(emptyVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }

    // In-game UI overlay: alpha-blended NDC triangles drawn over the presented
    // scene (the back buffer), no depth. Batched by texIndex since GL has no
    // bindless array here yet.
    void DrawUIOverlay(const UIVertex* verts, u32 count) override {
        if (!sceneOk_ || !verts || count < 3) return;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, GLsizei(width_), GLsizei(height_));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(uiProg_);
        glBindVertexArray(uiVao_);
        glBindBuffer(GL_ARRAY_BUFFER, uiVbo_);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(count * sizeof(UIVertex)), verts, GL_DYNAMIC_DRAW);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, sampLut_); // clamp + linear, no mip
        glUniform1i(uUiTex_, 0);

        const u32 nTris = count / 3;
        for (u32 t = 0; t < nTris;) {
            const u32 idx = verts[t * 3].texIndex; // a triangle shares one texIndex
            u32 e = t;
            while (e < nTris && verts[e * 3].texIndex == idx) ++e;
            const bool ok = idx != 0 && idx < textures_.size();
            glBindTexture(GL_TEXTURE_2D, ok ? textures_[idx] : textures_[0]);
            glDrawArrays(GL_TRIANGLES, GLint(t * 3), GLsizei((e - t) * 3));
            t = e;
        }
        glBindVertexArray(0);
        glBindSampler(0, 0);
        glDisable(GL_BLEND);
    }

    GraphicsAPI GetAPI() const override { return GraphicsAPI::OpenGL; }
    const char* GetAdapterName() const override { return adapter_.c_str(); }

private:
    void CreateUIBuffers() {
        glGenVertexArrays(1, &uiVao_);
        glGenBuffers(1, &uiVbo_);
        glBindVertexArray(uiVao_);
        glBindBuffer(GL_ARRAY_BUFFER, uiVbo_);
        const GLsizei stride = sizeof(UIVertex); // 52 (12 floats + u32; the
        // trailing NDC clip rect is unused here - GL's UI shader doesn't clip
        // yet, so ScrollView content renders unclipped on this backend)
        glEnableVertexAttribArray(0); // NDC position
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1); // uv
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(8));
        glEnableVertexAttribArray(2); // color
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(16));
        glBindVertexArray(0);
    }

    void CacheUniforms() {
        uModel_       = glGetUniformLocation(meshProg_, "uModel");
        uNormalMat_   = glGetUniformLocation(meshProg_, "uNormalMat");
        uBaseColor_   = glGetUniformLocation(meshProg_, "uBaseColor");
        uMetallic_    = glGetUniformLocation(meshProg_, "uMetallic");
        uRoughness_   = glGetUniformLocation(meshProg_, "uRoughness");
        uEmissive_    = glGetUniformLocation(meshProg_, "uEmissive");
        uHasAlbedo_   = glGetUniformLocation(meshProg_, "uHasAlbedo");
        uHasNormal_   = glGetUniformLocation(meshProg_, "uHasNormal");
        uHasMR_       = glGetUniformLocation(meshProg_, "uHasMR");
        uHasAO_       = glGetUniformLocation(meshProg_, "uHasAO");
        uHasEmissiveTex_ = glGetUniformLocation(meshProg_, "uHasEmissiveTex");
        // Bind the mesh sampler uniforms to fixed texture units, once.
        glUseProgram(meshProg_);
        SetSampler(meshProg_, "uAlbedo", 0);
        SetSampler(meshProg_, "uNormalTex", 1);
        SetSampler(meshProg_, "uMR", 2);
        SetSampler(meshProg_, "uAO", 3);
        SetSampler(meshProg_, "uEmissiveTex", 4);
        SetSampler(meshProg_, "uIrr", 5);
        SetSampler(meshProg_, "uPre", 6);
        SetSampler(meshProg_, "uBrdf", 7);
        BindBlock(meshProg_);
        BindBlock(skyProg_);
        uSky_ = glGetUniformLocation(skyProg_, "uSky");
        uHDR_ = glGetUniformLocation(presentProg_, "uHDR");
        uExposure_ = glGetUniformLocation(presentProg_, "uExposure");
        uUiTex_ = glGetUniformLocation(uiProg_, "uTex");
    }

    static void SetSampler(GLuint prog, const char* name, GLint unit) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform1i(loc, unit);
    }
    static void BindBlock(GLuint prog) {
        GLuint blk = glGetUniformBlockIndex(prog, "Frame");
        if (blk != GLuint(-1)) glUniformBlockBinding(prog, blk, 0);
    }

    void CreateSamplers() {
        glGenSamplers(1, &sampMaterial_);
        glSamplerParameteri(sampMaterial_, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri(sampMaterial_, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri(sampMaterial_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(sampMaterial_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenSamplers(1, &sampEquirect_); // equirect: U wraps, V clamps at the poles
        glSamplerParameteri(sampEquirect_, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri(sampEquirect_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampEquirect_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(sampEquirect_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenSamplers(1, &sampLut_);
        glSamplerParameteri(sampLut_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampLut_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampLut_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(sampLut_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // Binds textures_[index] to a unit. Returns false (binds white) if invalid.
    bool BindUnit(GLuint unit, u32 index) {
        glActiveTexture(GL_TEXTURE0 + unit);
        const bool ok = index != 0 && index < textures_.size();
        glBindTexture(GL_TEXTURE_2D, ok ? textures_[index] : textures_[0]);
        return ok;
    }
    bool BindMaterial(GLuint unit, TextureHandle h) { return BindUnit(unit, h.index); }

    static void SetupVertexLayout() {
        const GLsizei stride = sizeof(hbe::Vertex); // 72
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(12));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(24));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(40));
        glEnableVertexAttribArray(4);
        glVertexAttribIPointer(4, 4, GL_UNSIGNED_SHORT, stride, reinterpret_cast<void*>(48));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(56));
    }

    void DestroyMesh(GlMesh& m) {
        if (m.ibo) glDeleteBuffers(1, &m.ibo);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        m = {};
    }

    static u32 GLFormat(Format f, GLint& internal, GLenum& fmt, GLenum& type) {
        switch (f) {
            case Format::R8G8B8A8_SRGB:  internal = GL_SRGB8_ALPHA8; fmt = GL_RGBA; type = GL_UNSIGNED_BYTE; return 4;
            case Format::B8G8R8A8_UNORM: internal = GL_RGBA8;        fmt = GL_BGRA; type = GL_UNSIGNED_BYTE; return 4;
            case Format::B8G8R8A8_SRGB:  internal = GL_SRGB8_ALPHA8; fmt = GL_BGRA; type = GL_UNSIGNED_BYTE; return 4;
            case Format::R16G16B16A16_FLOAT: internal = GL_RGBA16F;  fmt = GL_RGBA; type = GL_HALF_FLOAT;    return 8;
            case Format::R32G32B32A32_FLOAT: internal = GL_RGBA32F;  fmt = GL_RGBA; type = GL_FLOAT;         return 16;
            case Format::R8G8B8A8_UNORM:
            default:                     internal = GL_RGBA8;        fmt = GL_RGBA; type = GL_UNSIGNED_BYTE; return 4;
        }
    }

    // Uploads into the currently-bound 2D texture. When mipCount > 1 the provided
    // (tightly packed) levels are uploaded verbatim - critical for the IBL
    // prefiltered map whose mips are GGX-baked, not naive downsamples. With a
    // single level, a mip chain is generated.
    void UploadTexture(const TextureDesc& desc) {
        GLint internal; GLenum fmt, type;
        const u32 bpp = GLFormat(desc.format, internal, fmt, type);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const u32 mips = desc.mipCount < 1 ? 1 : desc.mipCount;
        const u8* p = static_cast<const u8*>(desc.pixels);
        for (u32 m = 0; m < mips; ++m) {
            const u32 w = std::max(1u, desc.width >> m);
            const u32 h = std::max(1u, desc.height >> m);
            glTexImage2D(GL_TEXTURE_2D, GLint(m), internal, GLsizei(w), GLsizei(h),
                         0, fmt, type, p);
            if (p) p += size_t(w) * h * bpp;
        }
        if (mips > 1) {
            // Provided (GGX-baked) chain: cap at exactly what was uploaded.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(mips - 1));
        } else if (desc.pixels) {
            // Single level: generate the full chain. MAX_LEVEL must stay high BEFORE
            // glGenerateMipmap or the chain is constrained away and the texture ends
            // up mipmap-incomplete (samples black under a mipmap min filter).
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        // Sampler objects drive filtering at draw; set sane defaults regardless.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    void CreateDefaultTexture() {
        const u32 white = 0xFFFFFFFFu;
        TextureDesc d{};
        d.width = d.height = 1;
        d.format = Format::R8G8B8A8_UNORM;
        d.pixels = &white;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        UploadTexture(d);
        textures_.clear();
        textures_.push_back(tex); // index 0 = white default
    }

    void EnsureHDR(u32 w, u32 h) {
        if (hdrFbo_ && fboW_ == w && fboH_ == h) return;
        if (!hdrFbo_) glGenFramebuffers(1, &hdrFbo_);
        if (!hdrColor_) glGenTextures(1, &hdrColor_);
        if (!hdrDepth_) glGenTextures(1, &hdrDepth_);
        glBindTexture(GL_TEXTURE_2D, hdrColor_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindTexture(GL_TEXTURE_2D, hdrDepth_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, GLsizei(w), GLsizei(h), 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, hdrFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrColor_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, hdrDepth_, 0);
        const GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, bufs);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            HBE_ERROR("[GL] HDR framebuffer incomplete.");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fboW_ = w; fboH_ = h;
    }

    HWND  hwnd_ = nullptr;
    HDC   hdc_ = nullptr;
    HGLRC ctx_ = nullptr;
    u32   width_ = 0, height_ = 0;
    std::string adapter_ = "OpenGL";
    bool  sceneOk_ = false;
    glm::vec4 clear_{0.018f, 0.018f, 0.022f, 1.0f};

    GLuint meshProg_ = 0, skyProg_ = 0, presentProg_ = 0, uiProg_ = 0, emptyVao_ = 0, frameUbo_ = 0;
    GLuint uiVao_ = 0, uiVbo_ = 0;
    GLuint sampMaterial_ = 0, sampEquirect_ = 0, sampLut_ = 0;
    GLint uModel_ = -1, uNormalMat_ = -1, uBaseColor_ = -1, uMetallic_ = -1, uRoughness_ = -1,
          uEmissive_ = -1, uHasAlbedo_ = -1, uHasNormal_ = -1, uHasMR_ = -1, uHasAO_ = -1,
          uHasEmissiveTex_ = -1, uSky_ = -1, uHDR_ = -1, uExposure_ = -1, uUiTex_ = -1;

    GLuint hdrFbo_ = 0, hdrColor_ = 0, hdrDepth_ = 0;
    u32    fboW_ = 0, fboH_ = 0;

    std::vector<GlMesh> meshes_{GlMesh{}}; // index 0 reserved (invalid handle)
    std::vector<GLuint> textures_;         // index 0 = white default
};

} // namespace

std::unique_ptr<IRenderDevice> CreateOpenGLDevice(const RenderDeviceDesc& desc) {
    auto dev = std::make_unique<GLDevice>();
    if (!dev->Init(desc)) return nullptr;
    return dev;
}

} // namespace hbe::rhi
