// HumanApp/main_humanapp.cpp - HeartbreakHuman. A procedural human authoring application.
//
// This is a TOOL, not a runtime system. It links no engine renderer, no scene, no editor:
// only the human-generation library plus D3D11 and ImGui. The reason is the same one that
// keeps the Hub standalone - a tool that can only run when the whole engine links and boots
// is a tool that cannot be used to diagnose the engine, and a tool that pulls in the engine's
// renderer inherits every one of its constraints for no benefit.
//
// The generator is deliberately allowed to be EXPENSIVE. Extraction runs on a worker thread
// while the viewport keeps drawing the previous body, because a tool that freezes for a
// second on every slider drag is not an authoring tool - it is a batch process with a GUI.
//
// D3D11 rather than the engine's D3D12 backend: this window needs a depth buffer, a lit
// triangle pass and a line pass. D3D11 does that in a few hundred lines with no device
// lifetime to manage, and nothing here is performance-bound - the frame cost is dominated by
// generation, which happens off this thread entirely.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include "Human/HumanGenerator.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace hbe;
using namespace hbe::human;

namespace {

// ---------------------------------------------------------------------------
// D3D11 plumbing
// ---------------------------------------------------------------------------

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11DepthStencilView* g_dsv = nullptr;
ID3D11Texture2D* g_depth = nullptr;
u32 g_width = 1600, g_height = 950;

ID3D11VertexShader* g_meshVS = nullptr;
ID3D11PixelShader* g_meshPS = nullptr;
ID3D11InputLayout* g_meshIL = nullptr;
ID3D11VertexShader* g_lineVS = nullptr;
ID3D11PixelShader* g_linePS = nullptr;
ID3D11InputLayout* g_lineIL = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11RasterizerState* g_rsSolid = nullptr;
ID3D11RasterizerState* g_rsWire = nullptr;
ID3D11DepthStencilState* g_dss = nullptr;

// One matrix plus the shading knobs. Kept to a single 16-byte-aligned block so there is
// exactly one constant buffer in the whole program.
struct alignas(16) Constants {
    glm::mat4 viewProj{1.0f};
    glm::vec4 lightDir{0.4f, 0.8f, 0.45f, 0.0f};
    glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 mode{0.0f, 0.0f, 0.0f, 0.0f}; // x: 1 = colour by region
};

const char* kShaderSrc = R"HLSL(
cbuffer C : register(b0) {
    float4x4 gViewProj;
    float4   gLightDir;
    float4   gTint;
    float4   gMode;
};
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float3 nrm : TEXCOORD0; float4 col : TEXCOORD1; };

VSOut VSMesh(VSIn i) {
    VSOut o;
    o.pos = mul(gViewProj, float4(i.pos, 1.0));
    o.nrm = i.nrm;
    o.col = i.col;
    return o;
}
float4 PSMesh(VSOut i) : SV_TARGET {
    float3 n = normalize(i.nrm);
    float3 l = normalize(gLightDir.xyz);
    // A key light plus a hemisphere fill. Enough to read anatomical form honestly - flat
    // lighting hides exactly the muscle relief this tool exists to show.
    float  key  = saturate(dot(n, l));
    float  fill = 0.5 + 0.5 * n.y;
    float3 base = lerp(float3(0.62, 0.50, 0.44), i.col.rgb, gMode.x);
    float3 c = base * (0.25 * fill + 0.85 * key);
    // A rim term so the silhouette stays readable against the background.
    c += base * 0.18 * pow(1.0 - saturate(n.z * 0.5 + 0.5), 3.0);
    return float4(pow(saturate(c * gTint.rgb), 1.0 / 2.2), 1.0);
}
struct LVSOut { float4 pos : SV_POSITION; float4 col : TEXCOORD0; };
LVSOut VSLine(float3 p : POSITION, float4 c : COLOR) {
    LVSOut o;
    o.pos = mul(gViewProj, float4(p, 1.0));
    o.col = c;
    return o;
}
float4 PSLine(LVSOut i) : SV_TARGET { return i.col; }
)HLSL";

bool CompileShader(const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = ::D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr,
                                    nullptr, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) {
        if (err) {
            std::printf("shader %s: %.*s\n", entry, static_cast<int>(err->GetBufferSize()),
                        static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        return false;
    }
    if (err) err->Release();
    return true;
}

void CreateTargets() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        D3D11_TEXTURE2D_DESC bd{};
        back->GetDesc(&bd);
        back->Release();
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = bd.Width;
        dd.Height = bd.Height;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (SUCCEEDED(g_dev->CreateTexture2D(&dd, nullptr, &g_depth)) && g_depth)
            g_dev->CreateDepthStencilView(g_depth, nullptr, &g_dsv);
        g_width = bd.Width;
        g_height = bd.Height;
    }
}

void ReleaseTargets() {
    if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
    if (g_depth) { g_depth->Release(); g_depth = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    // Hardware first, then WARP. A machine with a broken driver should still be able to
    // author a human, slowly, rather than not start.
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        if (SUCCEEDED(::D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, want, 1,
                                                      D3D11_SDK_VERSION, &sd, &g_swap, &g_dev,
                                                      &got, &g_ctx)))
            break;
    }
    if (!g_dev) return false;
    CreateTargets();

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    if (!CompileShader("VSMesh", "vs_5_0", &vs) || !CompileShader("PSMesh", "ps_5_0", &ps))
        return false;
    g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_meshVS);
    g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_meshPS);
    // Interleaved position/normal/colour. Deliberately NOT hbe::Vertex: the viewport wants a
    // per-vertex region colour, and copying into a 28-byte draw vertex is cheaper and
    // clearer than smuggling a colour through an unused field of the engine's layout.
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    g_dev->CreateInputLayout(il, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &g_meshIL);
    vs->Release();
    ps->Release();

    if (!CompileShader("VSLine", "vs_5_0", &vs) || !CompileShader("PSLine", "ps_5_0", &ps))
        return false;
    g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_lineVS);
    g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_linePS);
    const D3D11_INPUT_ELEMENT_DESC ll[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    g_dev->CreateInputLayout(ll, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &g_lineIL);
    vs->Release();
    ps->Release();

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Constants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cbd, nullptr, &g_cb);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = TRUE;
    rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rsSolid);
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    g_dev->CreateRasterizerState(&rd, &g_rsWire);

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    g_dev->CreateDepthStencilState(&dsd, &g_dss);
    return true;
}

// ---------------------------------------------------------------------------
// GPU-side view of a generated human
// ---------------------------------------------------------------------------

struct DrawVertex {
    glm::vec3 pos;
    glm::vec3 nrm;
    u32 col;
};
struct LineVertex {
    glm::vec3 pos;
    u32 col;
};

u32 RegionColour(Region r) {
    // Distinct hues per region so the anatomical partition is legible at a glance - this is
    // an inspection tool before it is a beauty tool.
    static const u32 kPalette[] = {
        0xFF6E7BE8, 0xFF5FB3E8, 0xFF54C9A8, 0xFF6BD46B, 0xFFB6D34F, 0xFFE8C64F,
        0xFFE89A4F, 0xFFE86B5A, 0xFFE85A9A, 0xFFC46BE8, 0xFF8F6BE8, 0xFF6B8FE8,
        0xFF4FD4C4, 0xFF4FE888, 0xFF9AE84F, 0xFFE8D14F, 0xFFE8804F, 0xFFE85A70,
        0xFFD45AE8, 0xFF7A5AE8,
    };
    return kPalette[static_cast<usize>(r) % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

struct GpuHuman {
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* lines = nullptr;
    u32 indexCount = 0;
    u32 lineCount = 0;

    void Release() {
        if (vb) { vb->Release(); vb = nullptr; }
        if (ib) { ib->Release(); ib = nullptr; }
        if (lines) { lines->Release(); lines = nullptr; }
        indexCount = lineCount = 0;
    }

    void Upload(const GeneratedHuman& h) {
        Release();
        const GeneratedSurface& s = h.surface;
        if (s.mesh.vertices.empty() || s.mesh.indices.empty()) return;

        std::vector<DrawVertex> verts(s.mesh.vertices.size());
        for (usize i = 0; i < verts.size(); ++i) {
            verts[i].pos = s.mesh.vertices[i].position;
            verts[i].nrm = s.mesh.vertices[i].normal;
            verts[i].col = i < s.vertexRegion.size() ? RegionColour(s.vertexRegion[i]) : 0xFFFFFFFF;
        }
        D3D11_BUFFER_DESC bd{};
        D3D11_SUBRESOURCE_DATA sr{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(DrawVertex));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sr.pSysMem = verts.data();
        g_dev->CreateBuffer(&bd, &sr, &vb);

        bd.ByteWidth = static_cast<UINT>(s.mesh.indices.size() * sizeof(u32));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sr.pSysMem = s.mesh.indices.data();
        g_dev->CreateBuffer(&bd, &sr, &ib);
        indexCount = static_cast<u32>(s.mesh.indices.size());

        // Skeleton and muscle overlays, built as one line list. Seeing the structures under
        // the skin is most of what makes this an anatomy tool rather than a mesh viewer.
        std::vector<LineVertex> lv;
        for (const BoneSolid& b : h.anatomy.bones) {
            lv.push_back({b.head, 0xFFFFFFFF});
            lv.push_back({b.tail, 0xFFFFFFFF});
        }
        for (const Muscle& m : h.anatomy.muscles) {
            const glm::vec3 o = h.anatomy.ToModel(m.originJoint, m.originLocal);
            const glm::vec3 i2 = h.anatomy.ToModel(m.insertJoint, m.insertLocal);
            const u32 c = 0xFF4444EE; // muscles in red; bone in white
            lv.push_back({o, c});
            lv.push_back({i2, c});
        }
        if (!lv.empty()) {
            bd.ByteWidth = static_cast<UINT>(lv.size() * sizeof(LineVertex));
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            sr.pSysMem = lv.data();
            g_dev->CreateBuffer(&bd, &sr, &lines);
            lineCount = static_cast<u32>(lv.size());
        }
    }
};

// ---------------------------------------------------------------------------
// Background generation
// ---------------------------------------------------------------------------

// Generation takes ~0.5-3 s at useful resolutions. Running it on the UI thread would make
// every slider feel broken, so it runs on a worker while the viewport keeps drawing the
// PREVIOUS human. The tool stays responsive and the cost stays honest - it is not hidden,
// it is shown as a progress state.
struct Generator {
    std::thread worker;
    std::atomic<bool> busy{false};
    std::atomic<bool> ready{false};
    std::mutex mutex;
    std::unique_ptr<GeneratedHuman> result;
    HumanParameters pending;
    SurfaceSettings settings;
    bool queued = false;

    void Request(const HumanParameters& p, const SurfaceSettings& s) {
        pending = p;
        settings = s;
        queued = true;
    }

    void Pump() {
        if (busy.load()) return;
        if (!queued) return;
        queued = false;
        const HumanParameters p = pending;
        const SurfaceSettings s = settings;
        if (worker.joinable()) worker.join();
        busy.store(true);
        worker = std::thread([this, p, s] {
            auto h = std::make_unique<GeneratedHuman>(HumanGenerator::Generate(p, s));
            {
                std::lock_guard<std::mutex> lock(mutex);
                result = std::move(h);
            }
            ready.store(true);
            busy.store(false);
        });
    }

    std::unique_ptr<GeneratedHuman> Take() {
        if (!ready.exchange(false)) return nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(result);
    }

    void Shutdown() {
        if (worker.joinable()) worker.join();
    }
};

// ---------------------------------------------------------------------------

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_dev && wp != SIZE_MINIMIZED) {
            ReleaseTargets();
            g_swap->ResizeBuffers(0, static_cast<UINT>(LOWORD(lp)), static_cast<UINT>(HIWORD(lp)),
                                  DXGI_FORMAT_UNKNOWN, 0);
            CreateTargets();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 5.0f;
    s.FrameRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.97f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.20f, 0.26f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.24f, 0.32f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.34f, 0.46f, 1.0f);
}

// A slider that reports whether it changed, so the caller can mark exactly one dirty flag.
bool Slider(const char* label, f32* v, f32 lo, f32 hi, const char* fmt = "%.2f",
            const char* tip = nullptr) {
    const bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return changed;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int) {
    // --test-human runs the whole generator headlessly. A GUI-subsystem process has no
    // console, so attach to the parent's when there is one - that is what makes the flag
    // usable from a shell and from CI.
    if (cmdLine && std::strstr(cmdLine, "--test-human")) {
        // Only claim a console when we do not already HAVE a usable stdout. Reopening
        // CONOUT$ unconditionally writes to the console DEVICE, which throws the output away
        // whenever the caller redirected to a pipe or a file - so the flag appeared to
        // produce nothing at all when run from a shell or from CI.
        const HANDLE existing = ::GetStdHandle(STD_OUTPUT_HANDLE);
        const bool inherited = existing && existing != INVALID_HANDLE_VALUE;
        if (!inherited) {
            if (!::AttachConsole(ATTACH_PARENT_PROCESS)) ::AllocConsole();
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
        }
        const bool ok = GeneratorSelfTest();
        std::printf("human %s\n", ok ? "PASS" : "FAIL");
        std::fflush(stdout);
        return ok ? 0 : 1;
    }

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, ::GetModuleHandleW(nullptr), nullptr,
                   ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), nullptr, nullptr,
                   L"HeartbreakHumanWnd", nullptr};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"HeartbreakHuman", WS_OVERLAPPEDWINDOW, 80, 60,
                                static_cast<int>(g_width), static_cast<int>(g_height), nullptr,
                                nullptr, wc.hInstance, nullptr);
    if (!CreateDevice(hwnd)) {
        ::MessageBoxW(nullptr, L"HeartbreakHuman could not create a Direct3D 11 device.",
                      L"HeartbreakHuman", MB_ICONERROR | MB_OK);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyTheme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // --- state ---
    HumanParameters params;
    SurfaceSettings preview;
    preview.resolution = 96;
    Generator gen;
    GpuHuman gpu;
    std::unique_ptr<GeneratedHuman> human;
    bool autoRegen = true;
    bool dirty = true;
    bool showSkeleton = false, showMuscles = false, wireframe = false, colourByRegion = false;
    f32 camYaw = 0.35f, camPitch = 0.05f, camDist = 3.2f, camHeight = 0.95f;
    int selectedMuscle = -1;

    bool running = true;
    while (running) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (dirty && autoRegen && !gen.busy.load()) {
            gen.Request(params, preview);
            dirty = false;
        }
        gen.Pump();
        if (auto fresh = gen.Take()) {
            human = std::move(fresh);
            gpu.Upload(*human);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        // Orbit camera. Dragging anywhere the UI is not using rotates; the wheel dollies.
        if (!io.WantCaptureMouse) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                camYaw += io.MouseDelta.x * 0.008f;
                camPitch = std::clamp(camPitch + io.MouseDelta.y * 0.008f, -1.4f, 1.4f);
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
                camHeight = std::clamp(camHeight - io.MouseDelta.y * 0.004f, -0.5f, 2.5f);
            camDist = std::clamp(camDist - io.MouseWheel * 0.25f, 0.35f, 12.0f);
        }

        // ---- the panel ----
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, static_cast<f32>(g_height)), ImGuiCond_Always);
        ImGui::Begin("Human", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.95f, 1.0f), "HeartbreakHuman");
        ImGui::TextDisabled("Procedural anatomy. The surface is derived from it.");
        ImGui::Separator();

        if (gen.busy.load()) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Generating...");
        else if (human) {
            ImGui::Text("%d verts  %d tris  %.2fs", static_cast<int>(human->surface.mesh.vertices.size()),
                        static_cast<int>(human->surface.mesh.indices.size() / 3),
                        human->TotalSeconds());
            ImGui::TextDisabled("id %016llX", static_cast<unsigned long long>(human->contentHash));
            if (human->surface.boundaryEdges > 0)
                ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%u HOLES in the surface",
                                   human->surface.boundaryEdges);
            if (human->surface.nonManifoldEdges > 0)
                ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "%u pinched edges",
                                   human->surface.nonManifoldEdges);
        }
        ImGui::Checkbox("Auto-regenerate", &autoRegen);
        ImGui::SameLine();
        if (ImGui::Button("Generate")) { gen.Request(params, preview); }
        int res = static_cast<int>(preview.resolution);
        if (ImGui::SliderInt("Detail", &res, 32, 220)) {
            preview.resolution = static_cast<u32>(res);
            dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Grid cells along the body's longest axis. Higher is slower and "
                              "finer; generation cost grows with the cube of this.");

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Body", ImGuiTreeNodeFlags_DefaultOpen)) {
            int seed = static_cast<int>(params.seed);
            if (ImGui::InputInt("Seed", &seed)) { params.seed = static_cast<u64>(std::max(1, seed)); dirty = true; }
            dirty |= Slider("Height (m)", &params.height, 1.2f, 2.2f, "%.3f");
            dirty |= Slider("Weight (kg)", &params.weight, 35.0f, 180.0f, "%.1f");
            dirty |= Slider("Muscle mass", &params.muscleMass, 0.0f, 1.0f, "%.2f",
                            "Scales every muscle's physiological cross-section.");
            dirty |= Slider("Body fat", &params.bodyFat, 0.0f, 1.0f, "%.2f");
            dirty |= Slider("Age", &params.age, 8.0f, 90.0f, "%.0f");
            dirty |= Slider("Dimorphism", &params.dimorphism, 0.0f, 1.0f, "%.2f",
                            "Continuous feminine..masculine weighting of lean mass and fat.");
            dirty |= Slider("Asymmetry", &params.asymmetry, 0.0f, 1.0f, "%.2f",
                            "Seeded left/right divergence. 0 is perfectly symmetric.");
        }
        if (ImGui::CollapsingHeader("Proportions")) {
            dirty |= Slider("Leg length", &params.body.legLength, 0.7f, 1.4f);
            dirty |= Slider("Torso length", &params.body.torsoLength, 0.7f, 1.4f);
            dirty |= Slider("Arm length", &params.body.armLength, 0.7f, 1.4f);
            dirty |= Slider("Neck length", &params.body.neckLength, 0.6f, 1.6f);
            dirty |= Slider("Shoulder width", &params.body.shoulderWidth, 0.7f, 1.4f);
            dirty |= Slider("Hip width", &params.body.hipWidth, 0.7f, 1.5f);
            dirty |= Slider("Ribcage depth", &params.body.ribcageDepth, 0.7f, 1.4f);
            dirty |= Slider("Head size", &params.body.headSize, 0.8f, 1.25f);
            dirty |= Slider("Hand size", &params.body.handSize, 0.7f, 1.4f);
            dirty |= Slider("Foot size", &params.body.footSize, 0.7f, 1.4f);
        }
        if (ImGui::CollapsingHeader("Muscle")) {
            ImGui::TextDisabled("Per region, multiplied by Muscle mass.");
            dirty |= Slider("Shoulders", &params.muscle.shoulders, 0.0f, 2.5f);
            dirty |= Slider("Chest", &params.muscle.chest, 0.0f, 2.5f);
            dirty |= Slider("Back", &params.muscle.back, 0.0f, 2.5f);
            dirty |= Slider("Arms", &params.muscle.arms, 0.0f, 2.5f);
            dirty |= Slider("Forearms", &params.muscle.forearms, 0.0f, 2.5f);
            dirty |= Slider("Abdomen", &params.muscle.abdomen, 0.0f, 2.5f);
            dirty |= Slider("Glutes", &params.muscle.glutes, 0.0f, 2.5f);
            dirty |= Slider("Thighs", &params.muscle.thighs, 0.0f, 2.5f);
            dirty |= Slider("Calves", &params.muscle.calves, 0.0f, 2.5f);
            dirty |= Slider("Neck", &params.muscle.neck, 0.0f, 2.5f);
        }
        if (ImGui::CollapsingHeader("Fat distribution")) {
            ImGui::TextDisabled("Where the weight sits, independently of how much there is.");
            dirty |= Slider("Abdomen##f", &params.fat.abdomen, 0.0f, 2.5f);
            dirty |= Slider("Hips##f", &params.fat.hips, 0.0f, 2.5f);
            dirty |= Slider("Glutes##f", &params.fat.glutes, 0.0f, 2.5f);
            dirty |= Slider("Chest##f", &params.fat.chest, 0.0f, 2.5f);
            dirty |= Slider("Back##f", &params.fat.back, 0.0f, 2.5f);
            dirty |= Slider("Arms##f", &params.fat.arms, 0.0f, 2.5f);
            dirty |= Slider("Thighs##f", &params.fat.thighs, 0.0f, 2.5f);
            dirty |= Slider("Calves##f", &params.fat.calves, 0.0f, 2.5f);
            dirty |= Slider("Under jaw##f", &params.fat.submental, 0.0f, 2.5f);
            dirty |= Slider("Face##f", &params.fat.face, 0.0f, 2.5f);
        }
        if (ImGui::CollapsingHeader("Head")) {
            dirty |= Slider("Skull length", &params.face.skullLength, 0.75f, 1.3f);
            dirty |= Slider("Skull width", &params.face.skullWidth, 0.75f, 1.3f);
            dirty |= Slider("Jaw width", &params.face.jawWidth, 0.7f, 1.4f);
            ImGui::TextDisabled("Cheekbones, brow, eyes, nose, lips and ears are declared\n"
                                "in the parameters and saved, but the facial anatomy that\n"
                                "would drive them is not built yet.");
        }
        if (human && ImGui::CollapsingHeader("Muscles")) {
            ImGui::TextDisabled("Activation contracts a muscle. It shortens and thickens by\n"
                                "volume preservation - no blendshape involved.");
            for (usize i = 0; i < human->anatomy.muscles.size(); ++i) {
                Muscle& m = human->anatomy.muscles[i];
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SliderFloat(m.name.c_str(), &m.activation, 0.0f, 1.0f, "%.2f")) {
                    selectedMuscle = static_cast<int>(i);
                    // Only the field and the surface depend on activation - the anatomy is
                    // already resolved, so re-resolving it would be wasted work.
                    HumanGenerator::Regenerate(*human, Stage::Field, preview);
                    gpu.Upload(*human);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("PCSA %.1f cm2   rest %.0f mm", m.pcsa * 10000.0f,
                                      m.restLength * 1000.0f);
                ImGui::PopID();
            }
        }
        if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Wireframe", &wireframe);
            ImGui::SameLine();
            ImGui::Checkbox("Regions", &colourByRegion);
            ImGui::Checkbox("Skeleton", &showSkeleton);
            ImGui::SameLine();
            ImGui::Checkbox("Muscles", &showMuscles);
            ImGui::TextDisabled("Drag: orbit   Right-drag: raise   Wheel: zoom");
        }
        ImGui::End();

        // ---- draw ----
        ImGui::Render();
        const f32 clear[4] = {0.055f, 0.058f, 0.070f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

        if (gpu.indexCount > 0) {
            const f32 aspect = static_cast<f32>(g_width) / std::max(1.0f, static_cast<f32>(g_height));
            const glm::vec3 target(0.0f, camHeight, 0.0f);
            const glm::vec3 eye = target + glm::vec3(std::sin(camYaw) * std::cos(camPitch),
                                                     std::sin(camPitch),
                                                     std::cos(camYaw) * std::cos(camPitch)) * camDist;
            Constants c;
            c.viewProj = glm::perspectiveLH_ZO(glm::radians(38.0f), aspect, 0.02f, 60.0f) *
                         glm::lookAtLH(eye, target, glm::vec3(0, 1, 0));
            c.mode.x = colourByRegion ? 1.0f : 0.0f;
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                std::memcpy(map.pData, &c, sizeof(c));
                g_ctx->Unmap(g_cb, 0);
            }
            D3D11_VIEWPORT vp{0, 0, static_cast<f32>(g_width), static_cast<f32>(g_height), 0, 1};
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->OMSetDepthStencilState(g_dss, 0);
            g_ctx->RSSetState(wireframe ? g_rsWire : g_rsSolid);
            g_ctx->IASetInputLayout(g_meshIL);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT stride = sizeof(DrawVertex), offset = 0;
            g_ctx->IASetVertexBuffers(0, 1, &gpu.vb, &stride, &offset);
            g_ctx->IASetIndexBuffer(gpu.ib, DXGI_FORMAT_R32_UINT, 0);
            g_ctx->VSSetShader(g_meshVS, nullptr, 0);
            g_ctx->PSSetShader(g_meshPS, nullptr, 0);
            g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
            g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
            g_ctx->DrawIndexed(gpu.indexCount, 0, 0);

            if (gpu.lineCount > 0 && (showSkeleton || showMuscles)) {
                // Overlays draw with depth testing OFF so structures under the skin are
                // visible - the whole point of inspecting them.
                g_ctx->OMSetDepthStencilState(nullptr, 0);
                g_ctx->IASetInputLayout(g_lineIL);
                g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                const UINT ls = sizeof(LineVertex);
                g_ctx->IASetVertexBuffers(0, 1, &gpu.lines, &ls, &offset);
                g_ctx->VSSetShader(g_lineVS, nullptr, 0);
                g_ctx->PSSetShader(g_linePS, nullptr, 0);
                const u32 boneVerts = static_cast<u32>(human ? human->anatomy.bones.size() * 2 : 0);
                if (showSkeleton && boneVerts > 0) g_ctx->Draw(boneVerts, 0);
                if (showMuscles && gpu.lineCount > boneVerts)
                    g_ctx->Draw(gpu.lineCount - boneVerts, boneVerts);
            }
        }

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    gen.Shutdown();
    gpu.Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseTargets();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
