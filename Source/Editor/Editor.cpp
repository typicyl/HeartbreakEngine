// Editor/Editor.cpp
#include "Editor/Editor.h"

#include "Assets/AssetLoader.h"
#include "Assets/MeshGenerator.h"
#include "Assets/UAF.h"
#include "Assets/UAP.h"
#include "Audio/AudioSystem.h"
#include "Core/Input.h"
#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Editor/Importer.h"
#include "Editor/MeshThumbnail.h"
#include "Engine/Engine.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/AnimationSystem.h"
#include "Scene/PaintSystem.h"
#include "Scene/SceneSerializer.h"
#include "Scene/StreamingWorld.h"
#include "Scene/TerrainSystem.h"
#include "Schematic/Schematic.h"
#include "Schematic/SchematicSystem.h"
#include "UI/FontAtlas.h"
#include "UI/UISystem.h"
#include "Project/Project.h"
#include "Renderer/Camera.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "RHI/RHI.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder
#include <ImGuizmo.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h> // IFileDialog (folder picker)

#include <nlohmann/json.hpp>
#include <stb_image.h> // brush-tip image import (implementation in stb_image_impl.cpp)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace hbe {

namespace {
// Splits a TRS matrix back into a Transform (no shear support).
void DecomposeTRS(const glm::mat4& m, Transform& out) {
    out.position = glm::vec3(m[3]);
    const glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    out.scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    const glm::vec3 s = glm::max(out.scale, glm::vec3(1e-6f));
    out.rotation = glm::normalize(glm::quat_cast(glm::mat3(c0 / s.x, c1 / s.y, c2 / s.z)));
}

// --- Asset-browser tile icons (vector art via the ImGui draw list) ----------

constexpr ImU32 kMeshAccent    = IM_COL32(120, 170, 255, 255);
constexpr ImU32 kTextureAccent = IM_COL32(140, 215, 150, 255);
constexpr ImU32 kAudioAccent   = IM_COL32(255, 200, 95, 255);
constexpr ImU32 kOtherAccent   = IM_COL32(150, 150, 158, 255);

// Shaded isometric cube.
void DrawMeshIcon(ImDrawList* dl, ImVec2 c, f32 s) {
    const ImVec2 A(c.x, c.y - s);                    // top
    const ImVec2 B(c.x + s * 0.9f, c.y - s * 0.45f); // upper right
    const ImVec2 C(c.x + s * 0.9f, c.y + s * 0.45f); // lower right
    const ImVec2 D(c.x, c.y + s);                    // bottom
    const ImVec2 E(c.x - s * 0.9f, c.y + s * 0.45f); // lower left
    const ImVec2 F(c.x - s * 0.9f, c.y - s * 0.45f); // upper left
    const ImVec2 M(c.x, c.y + s * 0.1f);             // shared front vertex
    dl->AddQuadFilled(A, B, M, F, IM_COL32(150, 190, 255, 255)); // top face
    dl->AddQuadFilled(B, C, D, M, IM_COL32(95, 140, 215, 255));  // right face
    dl->AddQuadFilled(F, M, D, E, IM_COL32(70, 105, 170, 255));  // left face
    const ImVec2 hex[6] = {A, B, C, D, E, F};
    dl->AddPolyline(hex, 6, IM_COL32(35, 50, 80, 255), ImDrawFlags_Closed, 1.5f);
}

// Speaker with sound arcs.
void DrawAudioIcon(ImDrawList* dl, ImVec2 c, f32 s) {
    const ImU32 body = kAudioAccent;
    dl->AddRectFilled(ImVec2(c.x - s * 0.95f, c.y - s * 0.32f),
                      ImVec2(c.x - s * 0.45f, c.y + s * 0.32f), body, s * 0.08f);
    const ImVec2 cone[4] = {
        ImVec2(c.x - s * 0.5f, c.y - s * 0.32f),
        ImVec2(c.x + s * 0.05f, c.y - s * 0.8f),
        ImVec2(c.x + s * 0.05f, c.y + s * 0.8f),
        ImVec2(c.x - s * 0.5f, c.y + s * 0.32f),
    };
    dl->AddConvexPolyFilled(cone, 4, body);
    for (int i = 1; i <= 2; ++i) {
        dl->PathArcTo(ImVec2(c.x + s * 0.1f, c.y), s * (0.3f + 0.3f * i), -0.9f, 0.9f, 12);
        dl->PathStroke(body, ImDrawFlags_None, s * 0.1f);
    }
}

// Modern folder-picker dialog (IFileDialog with FOS_PICKFOLDERS).
std::optional<std::filesystem::path> BrowseForFolderDialog() {
    // S_FALSE (already initialized) is fine.
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&dlg)))) {
        return std::nullopt;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);

    std::optional<std::filesystem::path> out;
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                out = std::filesystem::path(psz);
                ::CoTaskMemFree(psz);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// Where the recent-projects list persists (per user).
std::filesystem::path RecentProjectsFile() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    const std::filesystem::path base =
        (n > 0 && n < MAX_PATH) ? std::filesystem::path(buf)
                                : std::filesystem::temp_directory_path();
    return base / "HeartbreakEngine" / "recent_projects.json";
}

// Classic folder with a tab.
void DrawFolderIcon(ImDrawList* dl, ImVec2 c, f32 s) {
    const ImU32 tab = IM_COL32(205, 160, 70, 255);
    const ImU32 body = IM_COL32(235, 190, 95, 255);
    dl->AddRectFilled(ImVec2(c.x - s * 0.95f, c.y - s * 0.75f),
                      ImVec2(c.x - s * 0.15f, c.y - s * 0.25f), tab, s * 0.12f);
    dl->AddRectFilled(ImVec2(c.x - s * 0.95f, c.y - s * 0.5f),
                      ImVec2(c.x + s * 0.95f, c.y + s * 0.7f), body, s * 0.12f);
    dl->AddRectFilled(ImVec2(c.x - s * 0.95f, c.y - s * 0.5f),
                      ImVec2(c.x + s * 0.95f, c.y - s * 0.32f),
                      IM_COL32(255, 215, 130, 255), s * 0.12f);
}

// Film clapperboard (scene assets).
void DrawSceneIcon(ImDrawList* dl, ImVec2 c, f32 s) {
    const ImU32 body = IM_COL32(170, 130, 230, 255);
    const ImU32 dark = IM_COL32(110, 80, 160, 255);
    // Angled top bar with stripes.
    const ImVec2 bar[4] = {
        ImVec2(c.x - s * 0.95f, c.y - s * 0.45f),
        ImVec2(c.x + s * 0.95f, c.y - s * 0.75f),
        ImVec2(c.x + s * 0.95f, c.y - s * 0.35f),
        ImVec2(c.x - s * 0.95f, c.y - s * 0.05f),
    };
    dl->AddConvexPolyFilled(bar, 4, dark);
    for (int i = 0; i < 3; ++i) {
        const f32 t0 = 0.12f + i * 0.3f;
        const ImVec2 a(c.x - s * 0.95f + t0 * 1.9f * s,
                       c.y - s * 0.45f - t0 * 0.3f * s);
        dl->AddQuadFilled(a, ImVec2(a.x + s * 0.18f, a.y - s * 0.03f),
                          ImVec2(a.x + s * 0.18f, a.y + s * 0.32f),
                          ImVec2(a.x, a.y + s * 0.35f), body);
    }
    // Board body.
    dl->AddRectFilled(ImVec2(c.x - s * 0.95f, c.y - s * 0.05f),
                      ImVec2(c.x + s * 0.95f, c.y + s * 0.75f), body, s * 0.1f);
}

// Shaded material ball (PBR sphere with a specular highlight).
void DrawMaterialIcon(ImDrawList* dl, ImVec2 c, f32 s) {
    dl->AddCircleFilled(c, s, IM_COL32(70, 120, 110, 255), 32);
    dl->AddCircleFilled(ImVec2(c.x - s * 0.18f, c.y - s * 0.18f), s * 0.82f,
                        IM_COL32(95, 175, 155, 255), 32);
    dl->AddCircleFilled(ImVec2(c.x - s * 0.35f, c.y - s * 0.38f), s * 0.22f,
                        IM_COL32(225, 245, 240, 235), 24);
    dl->AddCircle(c, s, IM_COL32(40, 70, 64, 255), 32, 1.5f);
}

// Framed "photo" placeholder (used while a texture has no thumbnail).
void DrawTextureIcon(ImDrawList* dl, ImVec2 c, f32 s) {
    const ImVec2 p0(c.x - s, c.y - s * 0.8f);
    const ImVec2 p1(c.x + s, c.y + s * 0.8f);
    dl->AddRectFilled(p0, p1, IM_COL32(60, 75, 62, 255), s * 0.1f);
    dl->AddCircleFilled(ImVec2(c.x - s * 0.4f, c.y - s * 0.35f), s * 0.16f, kTextureAccent);
    dl->AddTriangleFilled(ImVec2(c.x - s * 0.8f, p1.y - 2), ImVec2(c.x - s * 0.05f, c.y - s * 0.1f),
                          ImVec2(c.x + s * 0.55f, p1.y - 2), IM_COL32(95, 150, 105, 255));
    dl->AddTriangleFilled(ImVec2(c.x - s * 0.1f, p1.y - 2), ImVec2(c.x + s * 0.45f, c.y + s * 0.05f),
                          ImVec2(c.x + s * 0.95f, p1.y - 2), IM_COL32(120, 185, 130, 255));
    dl->AddRect(p0, p1, kTextureAccent, s * 0.1f, 0, 1.5f);
}
} // namespace

// Layout persistence: keeps the ImGui .ini path alive (ImGui stores the pointer,
// not a copy) and remembers whether a saved layout existed at launch so the
// default DockBuilder arrangement is only applied on a fresh install.
static std::string g_layoutIniPath;
static bool g_hadSavedLayout = false;

void Editor::EnableLayoutPersistence(const char* iniPath) {
    g_layoutIniPath = iniPath ? iniPath : "";
    std::error_code ec;
    g_hadSavedLayout = !g_layoutIniPath.empty() && std::filesystem::exists(g_layoutIniPath, ec);
    ImGui::GetIO().IniFilename = g_layoutIniPath.empty() ? nullptr : g_layoutIniPath.c_str();
}

void Editor::ApplyTheme() {
    // Crisp system font instead of ImGui's bitmap default (13px ProggyClean).
    ImGuiIO& io = ImGui::GetIO();
    const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
    std::error_code ec;
    if (std::filesystem::exists(font, ec)) {
        io.Fonts->Clear();
        io.Fonts->AddFontFromFileTTF(font, 17.0f);
    }

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 5.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowPadding = ImVec2(10.0f, 8.0f);
    s.FramePadding = ImVec2(8.0f, 4.0f);
    s.CellPadding = ImVec2(6.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 10.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.TabBarBorderSize = 0.0f;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.SeparatorTextBorderSize = 2.0f;

    // Heartbreak palette: charcoal surfaces with a crimson accent.
    const ImVec4 accent(0.86f, 0.27f, 0.33f, 1.00f);
    const ImVec4 accentHi(0.95f, 0.36f, 0.42f, 1.00f);
    const ImVec4 accentLo(0.55f, 0.18f, 0.23f, 1.00f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.56f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.092f, 0.094f, 0.110f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.105f, 0.107f, 0.124f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.080f, 0.082f, 0.095f, 0.98f);
    c[ImGuiCol_Border]                = ImVec4(0.25f, 0.25f, 0.30f, 0.45f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.160f, 0.162f, 0.190f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.215f, 0.217f, 0.250f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.260f, 0.262f, 0.300f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.070f, 0.071f, 0.083f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.110f, 0.112f, 0.130f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.070f, 0.071f, 0.083f, 0.85f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.105f, 0.107f, 0.124f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.080f, 0.082f, 0.095f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.280f, 0.282f, 0.320f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.360f, 0.362f, 0.400f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = accentLo;
    c[ImGuiCol_CheckMark]             = accentHi;
    c[ImGuiCol_SliderGrab]            = ImVec4(0.70f, 0.30f, 0.36f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = accentHi;
    c[ImGuiCol_Button]                = ImVec4(0.185f, 0.187f, 0.215f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.300f, 0.250f, 0.285f, 1.00f);
    c[ImGuiCol_ButtonActive]          = accentLo;
    c[ImGuiCol_Header]                = ImVec4(0.205f, 0.185f, 0.215f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.305f, 0.245f, 0.275f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.360f, 0.265f, 0.300f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.250f, 0.252f, 0.290f, 0.60f);
    c[ImGuiCol_SeparatorHovered]      = accentLo;
    c[ImGuiCol_SeparatorActive]       = accent;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.280f, 0.282f, 0.320f, 0.40f);
    c[ImGuiCol_ResizeGripHovered]     = accentLo;
    c[ImGuiCol_ResizeGripActive]      = accent;
    c[ImGuiCol_Tab]                   = ImVec4(0.125f, 0.127f, 0.148f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.380f, 0.240f, 0.280f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.270f, 0.180f, 0.215f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.110f, 0.112f, 0.130f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.180f, 0.150f, 0.170f, 1.00f);
    c[ImGuiCol_DockingPreview]        = ImVec4(0.86f, 0.27f, 0.33f, 0.45f);
    c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.070f, 0.071f, 0.083f, 1.00f);
    c[ImGuiCol_PlotLines]             = accentHi;
    c[ImGuiCol_PlotLinesHovered]      = accentHi;
    c[ImGuiCol_PlotHistogram]         = accent;
    c[ImGuiCol_PlotHistogramHovered]  = accentHi;
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.140f, 0.142f, 0.165f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.250f, 0.252f, 0.290f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.180f, 0.182f, 0.210f, 1.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.025f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.86f, 0.27f, 0.33f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = accentHi;
    c[ImGuiCol_NavHighlight]          = accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.10f, 0.10f, 0.12f, 0.50f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.05f, 0.05f, 0.06f, 0.60f);
}

void Editor::BuildUI(Engine& engine) {
    engine_ = &engine;
    if (hubMode_) {
        // Hub: nothing but the Project Manager over the demo scene.
        DrawProjectManager();
        return;
    }

    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    const Input& input = engine.GetInput();
    const f32 dt = engine.DeltaTime();

    // The engine loads the project's startup scene before the first frame; adopt
    // it once so the editor knows whether a level (3-file) or a single scene is
    // open (drives the level-aware Save + hierarchy grouping).
    if (!startupSynced_ && Project::HasActive()) {
        startupSynced_ = true;
        const std::string& su = Project::Active().Settings().startupScene;
        if (!su.empty()) {
            const std::filesystem::path sp = Project::Active().AssetsDir() / su;
            if (scene::IsLevelMember(sp)) {
                currentLevel_ = scene::ResolveLevel(sp);
                levelOpen_ = true;
            }
        }
    }

    if (!panelsInit_) {
        for (bool& b : panelOpen_) b = true;
        panelOpen_[Panel_ProjectSettings] = false; // opened from Project/Window menu
        if (artMode_) {
            // Artist build: show only the painting-relevant panels.
            for (bool& b : panelOpen_) b = false;
            panelOpen_[Panel_Viewport] = true;
            panelOpen_[Panel_ArtEditor] = true;
            panelOpen_[Panel_Hierarchy] = true;
            panelOpen_[Panel_Inspector] = true;
            panelOpen_[Panel_Scenes] = true;
            panelOpen_[Panel_Assets] = true;
            paintActive_ = true;                  // start ready to paint
            // The full Post Process panel (SSAO/SSR/fog/...) stays hidden in the
            // artist build.
        }
        panelsInit_ = true;
    }

    ImGuizmo::BeginFrame();
    previewSubmitted_ = false; // reset the shared editor-preview claim each frame

    // Main menu bar.
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Project")) {
            if (ImGui::MenuItem("Project Manager...")) showProjectManager_ = true;
            ImGui::Separator();
            const bool hasProject = Project::HasActive();
            if (ImGui::MenuItem("Save All", "Ctrl+Shift+S", false, hasProject)) {
                SaveAll(engine);
            }
            // Engine/build tooling is hidden in the artist build (artMode_).
            if (!artMode_) {
                if (ImGui::MenuItem("Project Settings...", nullptr, false, hasProject)) {
                    panelOpen_[Panel_ProjectSettings] = true;
                    ImGui::SetWindowFocus("Project Settings");
                }
                if (ImGui::MenuItem("Build Settings...", nullptr, false, hasProject)) {
                    showBuildSettings_ = true;
                }
                if (ImGui::MenuItem("Build", nullptr, false, hasProject)) {
                    BuildShipping(buildResult_); // packs assets + assembles Build/
                }
            }
            if (!recentProjects_.empty()) {
                ImGui::Separator();
                const usize count = glm::min<usize>(recentProjects_.size(), 5);
                for (usize i = 0; i < count; ++i) {
                    const std::filesystem::path p = recentProjects_[i];
                    if (ImGui::MenuItem(p.stem().string().c_str())) OpenProject(p);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack_.empty())) {
                Undo(engine);
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redoStack_.empty())) {
                Redo(engine);
            }
            ImGui::Separator();
            const bool hasSel = selected_ != entt::null && scene.Registry().valid(selected_);
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSel)) CopySelection(scene);
            if (ImGui::MenuItem("Cut", "Ctrl+X", false, hasSel)) {
                CopySelection(scene);
                PushUndo(scene);
                DestroyRecursive(scene, selected_);
                selected_ = entt::null;
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clipboard_.empty()))
                PasteClipboard(engine);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel))
                DuplicateSelection(engine);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scene")) {
            const bool hasProject = Project::HasActive();
            if (ImGui::MenuItem("New")) {
                PushUndo(scene);
                scene.Registry().clear();
                selected_ = entt::null;
                currentScenePath_.clear();
            }
            if (ImGui::MenuItem("Save", nullptr, false, hasProject)) {
                SaveCurrent(scene);
            }
            if (ImGui::MenuItem("Save As...", nullptr, false, hasProject)) {
                wantSaveSceneAs_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Set as startup scene", nullptr, false,
                                hasProject && !currentScenePath_.empty())) {
                Project::Active().Settings().startupScene =
                    Project::Active().RelativeAssetPath(currentScenePath_);
                Project::Active().Save();
            }
            ImGui::EndMenu();
        }
        DrawWindowMenu();
        // Quick access to the painting systems from the general editor: a Paint
        // toggle that enables the brush and reveals the Art Editor panel (the art-
        // mode exe already boots straight into paint mode, so it's hidden there).
        if (!artMode_) {
            if (ImGui::MenuItem("Paint", nullptr, paintActive_)) {
                paintActive_ = !paintActive_;
                if (paintActive_) {
                    panelOpen_[Panel_ArtEditor] = true;
                    ImGui::SetWindowFocus("Art Editor");
                }
            }
        }
        ImGui::EndMainMenuBar();
    }

    // Keyboard shortcuts (suppressed while a text field has focus).
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.WantTextInput) {
            // While the *surface* paint tool is active, Ctrl+Z/Y drive the
            // stroke-level paint history (the scene undo would not capture canvas
            // pixels). In 3D-stroke mode the strokes are real scene entities, so
            // undo must route to the scene history instead - otherwise undoing a
            // 3D stroke would pop a stale surface stroke from a different mode.
            const bool paintHistory = paintActive_ && !paintStrokeMode_ &&
                                      (!paintStrokeOrder_.empty() || !paintStrokeRedo_.empty());
            if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                SaveAll(engine); // Ctrl+Shift+S: save project + scene
            } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                // Ctrl+S: save the level (both files) / active scene / Save As.
                SaveCurrent(engine.GetScene());
            } else if (paintHistory && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                io.KeyShift ? PaintRedo(engine) : PaintUndo(engine);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                io.KeyShift ? Redo(engine) : Undo(engine);
            }
            if (paintHistory && ImGui::IsKeyPressed(ImGuiKey_Y, false)) PaintRedo(engine);
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo(engine);

            // Entity clipboard: copy / paste / cut / duplicate the selection.
            Scene& s = engine.GetScene();
            const bool hasSel = selected_ != entt::null && s.Registry().valid(selected_);
            if (ImGui::IsKeyPressed(ImGuiKey_C, false) && hasSel) CopySelection(s);
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) PasteClipboard(engine);
            if (ImGui::IsKeyPressed(ImGuiKey_X, false) && hasSel) {
                CopySelection(s);
                PushUndo(s);
                DestroyRecursive(s, selected_);
                selected_ = entt::null;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_D, false) && hasSel) DuplicateSelection(engine);
        }
    }

    // Save-As modal (opened from the Scene menu; popups can't open in menus).
    if (wantSaveSceneAs_) {
        ImGui::OpenPopup("Save Scene As");
        wantSaveSceneAs_ = false;
    }
    if (ImGui::BeginPopupModal("Save Scene As", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputText("Name", sceneSaveName_, sizeof(sceneSaveName_));
        const bool canSave = Project::HasActive() && sceneSaveName_[0] != '\0';
        ImGui::BeginDisabled(!canSave);
        if (ImGui::Button("Save")) {
            const std::filesystem::path path = Project::Active().AssetsDir() / "Scenes" /
                                               (std::string(sceneSaveName_) + ".hbscene");
            if (SaveSceneToDisk(scene, path)) {
                currentScenePath_ = path;
                RefreshAssets();
                scenesScanned_ = false;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Finalize any streamed (additive) scene load that finished this frame.
    streamer_.Pump(scene, renderer);

    // Full-window dockspace. On first run, build a Unity-style default layout -
    // unless a saved layout was restored from the .ini, which then wins.
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (!layoutBuilt_) {
        layoutBuilt_ = true;
        if (!g_hadSavedLayout) {
        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->Size);

        ImGuiID center = dockspace;
        if (artMode_) {
            // Painting-focused layout: a wide Art Editor on the right, the
            // viewport center, hierarchy left, scenes/assets along the bottom.
            ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.15f, nullptr, &center);
            ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
            ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, nullptr, &center);
            ImGui::DockBuilderDockWindow("Hierarchy", left);
            ImGui::DockBuilderDockWindow("Art Editor", right);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Scenes", bottom);
            ImGui::DockBuilderDockWindow("Assets", bottom);
            ImGui::DockBuilderDockWindow("Viewport", center);
            ImGui::DockBuilderFinish(dockspace);
            layoutBuilt_ = true;
        } else {
        ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
        ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
        // The bottom strip splits in two: a wide content browser on the left,
        // tool/log panels tabbed on the right.
        ImGuiID bottomRight =
            ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.42f, nullptr, &bottom);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Asset Viewer", right);
        ImGui::DockBuilderDockWindow("Project Settings", right);
        ImGui::DockBuilderDockWindow("Post Process", right);
        ImGui::DockBuilderDockWindow("Navigation", right);
        ImGui::DockBuilderDockWindow("Art Editor", right);
        ImGui::DockBuilderDockWindow("Assets", bottom);
        ImGui::DockBuilderDockWindow("Scenes", bottom);
        ImGui::DockBuilderDockWindow("Stats", bottomRight);
        ImGui::DockBuilderDockWindow("Timeline", bottomRight);
        ImGui::DockBuilderDockWindow("Audio Mixer", bottomRight);
        ImGui::DockBuilderDockWindow("Music", bottomRight);
        ImGui::DockBuilderDockWindow("Streaming", bottomRight);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("Game", center);
        ImGui::DockBuilderDockWindow("Schematic Editor", center);
        ImGui::DockBuilderFinish(dockspace);
        } // end full-editor layout
        } // end default-layout (skipped when a saved .ini was restored)
    }

    DrawProjectManager(); // modal; auto-opens while no project is active

    // Every panel below is gated by its panelOpen_ flag; the artist build
    // (artMode_) just defaults that set to the painting panels, so the same draw
    // path serves both the full editor and the focused Art Editor exe.
    DrawViewport(engine); // includes the transform gizmo
    DrawGameView(engine); // play/pause/stop + the game image
    // The editor freecam owns the viewport camera ONLY in edit mode. In play
    // mode the scene's game camera drives the view (cam::Update in the engine
    // loop, before this hook); running the freecam here would overwrite it with
    // the stale editor pose every frame, so the game camera would never show.
    if (!engine.IsGameCameraEnabled()) UpdateFreecam(renderer, input, dt);
    DrawHierarchy(scene, renderer);
    DrawInspector(scene, renderer);
    DrawStats(engine);
    DrawPostProcess(engine);
    DrawProjectSettings(engine);
    DrawNavigation(engine);
    DrawStreaming(engine);
    DrawTimeline(engine);
    DrawAssetBrowser(engine);
    DrawAssetViewer(engine);
    DrawSceneManager(engine);
    DrawAudioMixer(engine);
    DrawMusicEditor(engine);
    DrawBuildSettings(engine);
    DrawArtEditor(engine);
    DrawSchematicEditor(engine);
    DrawSelectionOutline(scene, renderer);
    DrawNavOverlay(scene, renderer);
    UpdateTerrainTool(engine); // terrain sculpt brush (consumes the click below)
    UpdateArtTool(engine);     // surface paint brush (also consumes the click)
    // Billboard icons for non-mesh entities (+ click-select). After the tools so
    // it can yield to an in-progress sculpt/paint stroke.
    DrawEntityIcons(scene, renderer);
    // PiP of the selected camera's view (after the asset viewer so it yields the
    // shared preview target to an active mesh preview).
    DrawCameraPreview(engine);

    // Left-click on the viewport image to select an entity. The gizmo only
    // blocks the click while a selection exists (ImGuizmo's hover state is
    // stale otherwise), and clicking always freezes the auto-orbit FIRST so
    // the pick ray matches the image that was actually on screen.
    const bool gizmoBlocks =
        ImGuizmo::IsUsing() ||
        (selected_ != entt::null && scene.Registry().valid(selected_) && ImGuizmo::IsOver());
    if (vpClicked_ && !gizmoBlocks && !freecamActive_ && !terrainConsumedClick_ &&
        !paintConsumedClick_ && !splineConsumedClick_ && !iconConsumedClick_) {
        if (renderer.IsOrbitEnabled()) {
            SyncFreecam(renderer);
            renderer.SetOrbitEnabled(false);
        }
        PickEntity(scene, renderer);
    }

    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
}

void Editor::DrawWindowMenu() {
    if (!ImGui::BeginMenu("Window")) return;

    // One toggle per dockable panel (titles match the ImGui window names).
    static const char* const kNames[Panel_Count] = {
        "Viewport",     "Game",       "Hierarchy",   "Inspector",
        "Asset Viewer", "Project Settings", "Post Process", "Navigation",
        "Streaming",    "Stats",      "Timeline",    "Scenes",
        "Audio Mixer",  "Assets",     "Art Editor",
        "Schematic Editor", "Music"};
    if (artMode_) {
        // Artist build: only the painting-relevant panels are listed/reachable.
        static const Panel kArtPanels[] = {Panel_Viewport, Panel_ArtEditor,
                                           Panel_Hierarchy, Panel_Inspector,
                                           Panel_Scenes, Panel_Assets};
        for (const Panel p : kArtPanels) ImGui::MenuItem(kNames[p], nullptr, &panelOpen_[p]);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            for (bool& b : panelOpen_) b = false;
            for (const Panel p : kArtPanels) panelOpen_[p] = true;
            layoutBuilt_ = false;
        }
        ImGui::EndMenu();
        return;
    }
    for (int i = 0; i < Panel_Count; ++i) {
        ImGui::MenuItem(kNames[i], nullptr, &panelOpen_[i]);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Show All Panels")) {
        for (bool& b : panelOpen_) b = true;
    }
    if (ImGui::MenuItem("Reset Layout")) {
        // Re-run the default DockBuilder arrangement next frame and reveal the
        // standard panels (Project Settings stays hidden, like first launch).
        for (bool& b : panelOpen_) b = true;
        panelOpen_[Panel_ProjectSettings] = false;
        layoutBuilt_ = false;
        g_hadSavedLayout = false; // force the built-in default over the saved .ini
    }
    ImGui::Separator();
    ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
    ImGui::EndMenu();
}

void Editor::SaveAll(Engine& engine) {
    if (!Project::HasActive()) {
        buildResult_ = "No project open.";
        return;
    }
    // Save the open level (both layer files) / active scene / Save As modal.
    SaveCurrent(engine.GetScene());
    // Project settings (environment / build / audio / startup scene).
    Project::Active().Save();
    buildResult_ = levelOpen_      ? "Saved project + level '" + currentLevel_.Name() + "'."
                   : currentScenePath_.empty()
                       ? "Saved project; choose a name for the scene."
                       : "Saved project + scene '" + currentScenePath_.stem().string() + "'.";
    HBE_INFO("{}", buildResult_);
}

void Editor::UpdateTerrainTool(Engine& engine) {
    terrainHitValid_ = false;
    terrainConsumedClick_ = false;
    if (!terrainSculpt_ || !vpVisible_) return;
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    auto& reg = scene.Registry();
    if (selected_ == entt::null || !reg.valid(selected_)) return;
    TerrainComponent* t = reg.try_get<TerrainComponent>(selected_);
    if (!t) return;

    // Mouse in normalized viewport-image coordinates.
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const f32 mx = (mp.x - vpX_) / glm::max(vpW_, 1.0f);
    const f32 my = (mp.y - vpY_) / glm::max(vpH_, 1.0f);
    const bool over = vpHovered_ && mx >= 0.0f && mx <= 1.0f && my >= 0.0f && my <= 1.0f;

    // World ray through the cursor -> terrain-local space.
    const Camera& cam = renderer.GetCamera();
    const glm::mat4 invVP = glm::inverse(cam.ViewProjection());
    const glm::vec2 ndc(mx * 2.0f - 1.0f, 1.0f - my * 2.0f);
    glm::vec4 pn = invVP * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 pf = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    pn /= pn.w;
    pf /= pf.w;
    const glm::vec3 ro(pn), rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
    const glm::mat4 world = scene.WorldMatrix(selected_);
    const glm::mat4 invWorld = glm::inverse(world);
    const glm::vec3 lo = glm::vec3(invWorld * glm::vec4(ro, 1.0f));
    const glm::vec3 ld = glm::vec3(invWorld * glm::vec4(rd, 0.0f));

    glm::vec3 localHit;
    if (!terrain::RaycastLocal(*t, lo, ld, localHit)) {
        terrainStroking_ = terrainStroking_ && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        return;
    }
    terrainHit_ = glm::vec3(world * glm::vec4(localHit, 1.0f));
    terrainHitValid_ = true;

    // Brush ring overlay (projected circle in the terrain surface).
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->PushClipRect(ImVec2(vpX_, vpY_), ImVec2(vpX_ + vpW_, vpY_ + vpH_), true);
        const glm::mat4 vp = cam.ViewProjection();
        ImVec2 pts[33];
        bool ok = true;
        for (int i = 0; i <= 32 && ok; ++i) {
            const f32 a = static_cast<f32>(i) / 32.0f * 6.2831853f;
            const f32 lx = localHit.x + std::cos(a) * terrainRadius_;
            const f32 lz = localHit.z + std::sin(a) * terrainRadius_;
            const glm::vec3 lp(lx, terrain::SampleHeight(*t, lx, lz), lz);
            glm::vec4 clip = vp * world * glm::vec4(lp, 1.0f);
            if (clip.w <= 0.001f) { ok = false; break; }
            const glm::vec2 nd = glm::vec2(clip) / clip.w;
            pts[i] = ImVec2(vpX_ + (nd.x * 0.5f + 0.5f) * vpW_,
                            vpY_ + (0.5f - nd.y * 0.5f) * vpH_);
        }
        if (ok) dl->AddPolyline(pts, 33, IM_COL32(255, 220, 90, 235), 0, 2.0f);
        dl->PopClipRect();
    }

    // Sculpt while LMB is held over the viewport. Freeze auto-orbit so the
    // camera doesn't drift under the brush.
    const bool lmb = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (over && lmb) {
        if (renderer.IsOrbitEnabled()) {
            SyncFreecam(renderer);
            renderer.SetOrbitEnabled(false);
        }
        if (!terrainStroking_) {
            PushUndo(scene); // one undo step per stroke
            terrainStroking_ = true;
            terrainFlatten_ = terrain::SampleHeight(*t, localHit.x, localHit.z);
        }
        const auto brush = static_cast<terrain::Brush>(terrainBrush_);
        const f32 dt = engine.DeltaTime();
        const f32 amount =
            (brush == terrain::Brush::Raise || brush == terrain::Brush::Lower)
                ? terrainStrength_ * dt
                : glm::clamp(terrainStrength_ * dt * 0.5f, 0.0f, 1.0f);
        terrain::Sculpt(scene, renderer, selected_, localHit.x, localHit.z,
                        terrainRadius_, amount, brush, terrainFlatten_);
        terrainConsumedClick_ = true; // don't also pick an entity
    } else {
        terrainStroking_ = false;
    }
}

// --- Art Editor (surface painting) ------------------------------------------

// Layer kind from a "<name>.static/.dynamic.hbscene" path (defined below).
static SceneKind KindFromScenePath(const std::string& p);

void Editor::SaveCurrent(Scene& scene) {
    if (levelOpen_) {
        if (SaveSceneToDisk(scene, {})) scenesScanned_ = false; // level: two files
        // Loose UI authored while a level is open isn't part of the level and has
        // no file yet: prompt for a standalone UI scene name (otherwise it'd be
        // dropped on reload). Only fires when there genuinely is untagged UI.
        if (currentScenePath_.empty()) {
            auto& reg = scene.Registry();
            for (const entt::entity e : reg.view<entt::entity>()) {
                if (reg.valid(e) && !reg.all_of<SceneSource>(e)) {
                    wantSaveSceneAs_ = true;
                    break;
                }
            }
        }
    } else if (currentScenePath_.empty()) {
        wantSaveSceneAs_ = true; // never saved -> name it
    } else if (SaveSceneToDisk(scene, currentScenePath_)) {
        scenesScanned_ = false;
    }
}

bool Editor::SaveSceneToDisk(Scene& scene, const std::filesystem::path& path) {
    auto& reg = scene.Registry();
    if (levelOpen_) {
        // A level (or several, composed additively): every NON-UI entity is tagged
        // with its layer FILE. UI is never folded into a level (EnsureLevelMembership
        // skips it), so it stays untagged and saves as the active scene below.
        EnsureLevelMembership(scene);
        if (Project::HasActive()) {
            scene::SavePaintCanvases(scene, Project::Active().AssetsDir(),
                                     currentLevel_.Name());
        }
    } else if (Project::HasActive()) {
        // Writes every loaded canvas; each scene's JSON then references its own.
        const std::string stem = path.empty() ? std::string("Scene") : path.stem().string();
        scene::SavePaintCanvases(scene, Project::Active().AssetsDir(), stem);
    }

    // Tagged groups (level layers + additively-loaded scenes) each write back to
    // their own SceneSource file.
    bool ok = SaveStreamedScenes(scene);

    // The active scene owns only UNTAGGED entities: loose UI while a level is open,
    // or the whole single scene otherwise. It saves to its own file and NEVER into
    // a level layer, so UI can't bleed into the level.
    const auto activeOnly = [&reg](entt::entity e) { return !reg.all_of<SceneSource>(e); };
    bool hasActive = false;
    for (const entt::entity e : reg.view<entt::entity>()) {
        if (reg.valid(e) && !reg.all_of<SceneSource>(e)) { hasActive = true; break; }
    }
    const std::filesystem::path activePath = !path.empty() ? path : currentScenePath_;
    if (hasActive && !activePath.empty())
        ok &= scene::SaveScene(scene, activePath, activeOnly);
    return ok;
}

bool Editor::SaveStreamedScenes(Scene& scene) {
    auto& reg = scene.Registry();
    std::set<std::string> paths;
    for (const entt::entity e : reg.view<SceneSource>()) {
        const std::string& p = reg.get<SceneSource>(e).scene;
        if (!p.empty()) paths.insert(p);
    }
    bool ok = true;
    for (const std::string& p : paths) {
        const auto fromThisScene = [&reg, &p](entt::entity e) {
            const SceneSource* ss = reg.try_get<SceneSource>(e);
            return ss && ss->scene == p;
        };
        // A level layer file (<name>.static/.dynamic) writes its kind into the
        // header; the filename is authoritative. Plain scenes stay Full.
        const SceneKind kind = KindFromScenePath(p);
        if (!scene::SaveScene(scene, std::filesystem::path(p), fromThisScene, kind)) ok = false;
    }
    return ok;
}

void Editor::MakePaintable(Scene& scene, Renderer& renderer, entt::entity e) {
    auto& reg = scene.Registry();
    if (!reg.valid(e) || !reg.all_of<MeshInstance>(e)) return;
    PaintComponent& pc = reg.get_or_emplace<PaintComponent>(e);
    pc.opacity = paintOpacity_;
    pc.heightScale = paintHeightScale_;
    pc.reliefEnabled = paintReliefDefault_;
    pc.lodBias = paintLodBias_;
    pc.layer = (paintActiveLayer_.empty() || paintActiveLayer_ == "All") ? "Default"
                                                                         : paintActiveLayer_;
    // Default to the mesh's own UVs (primitives now ship clean per-face / planar
    // unwraps). Box projection stays an opt-in per-object choice for cases that
    // need world-scaled, no-stretch mapping on a non-uniformly scaled object.
    pc.projection = 0;
    paint::EnsureCanvas(pc, static_cast<u32>(paintRes_));
    paint::Sync(renderer, pc);
}

const MeshData* Editor::GetCpuMesh(Scene& scene, entt::entity e) {
    auto& reg = scene.Registry();
    const MeshRef* ref = reg.try_get<MeshRef>(e);
    if (!ref || ref->source.empty()) return nullptr;
    if (auto it = cpuMeshCache_.find(ref->source); it != cpuMeshCache_.end())
        return it->second.vertices.empty() ? nullptr : &it->second;

    MeshData md;
    const std::string& src = ref->source;
    if (src.rfind("prim:", 0) == 0) {
        md = mesh::GeneratePrimitive(src.substr(5));
    } else if (src.rfind("uaf:", 0) == 0 && Project::HasActive()) {
        std::string rel = src.substr(4);
        u32 submesh = 0;
        if (const auto h = rel.find('#'); h != std::string::npos) {
            for (usize k = h + 1; k < rel.size() && std::isdigit((unsigned char)rel[k]); ++k)
                submesh = submesh * 10 + static_cast<u32>(rel[k] - '0');
            rel = rel.substr(0, h);
        }
        if (std::optional<Model> model = uaf::ReadMesh(Project::Active().AssetsDir() / rel)) {
            if (submesh < model->size()) md = std::move((*model)[submesh]);
        }
    }
    auto [ins, ok] = cpuMeshCache_.emplace(src, std::move(md));
    return ins->second.vertices.empty() ? nullptr : &ins->second;
}

// Commits a finished stroke/fill/clear to entity `e`'s stroke database + the
// global paint-order log; clears the redo stack (a new op invalidates redo).
void Editor::CommitStroke(entt::entity e, PaintComponent& pc, paint::Stroke&& s) {
    pc.strokes.push_back(std::move(s));
    paintStrokeOrder_.push_back(e);
    paintStrokeRedo_.clear();
}

// Stroke-level undo: pop the last committed stroke (across all painted canvases)
// and rebake that canvas from its remaining strokes. No pixel snapshots - the
// stroke database is the source of truth, so undo is just "drop the last stroke".
void Editor::PaintUndo(Engine& engine) {
    if (paintStrokeOrder_.empty()) return;
    auto& reg = engine.GetScene().Registry();
    const entt::entity e = paintStrokeOrder_.back();
    paintStrokeOrder_.pop_back();
    PaintComponent* pc = reg.valid(e) ? reg.try_get<PaintComponent>(e) : nullptr;
    if (!pc || pc->strokes.empty()) return;
    paintStrokeRedo_.emplace_back(e, std::move(pc->strokes.back()));
    pc->strokes.pop_back();
    // The CPU mesh is needed to rebake any projection (mode-2) strokes.
    paint::BakeFromStrokes(*pc, GetCpuMesh(engine.GetScene(), e));
    paint::Sync(engine.GetRenderer(), *pc);
}

void Editor::PaintRedo(Engine& engine) {
    if (paintStrokeRedo_.empty()) return;
    auto& reg = engine.GetScene().Registry();
    entt::entity e = paintStrokeRedo_.back().first;
    paint::Stroke s = std::move(paintStrokeRedo_.back().second);
    paintStrokeRedo_.pop_back();
    PaintComponent* pc = reg.valid(e) ? reg.try_get<PaintComponent>(e) : nullptr;
    if (!pc) return;
    pc->strokes.push_back(std::move(s));
    paintStrokeOrder_.push_back(e);
    paint::BakeFromStrokes(*pc, GetCpuMesh(engine.GetScene(), e));
    paint::Sync(engine.GetRenderer(), *pc);
}

std::string Editor::EnsureStrokeMaterial() {
    if (!Project::HasActive()) return {};
    const std::filesystem::path assets = Project::Active().AssetsDir();
    // Reuse a stroke material identical in tip + tint + metal/rough.
    char sig[192];
    std::snprintf(sig, sizeof(sig), "%d|%.2f|%.2f|%.2f|%.2f|%.3f|%.3f|%.3f|%.3f|%.2f|%.2f|%d",
                  brushDef_.shape, brushDef_.hardness, brushDef_.grain, brushDef_.bristles,
                  brushDef_.scatter, brushColor_.r, brushColor_.g, brushColor_.b, brushColor_.a,
                  brushMetallic_, brushRoughness_, brushDef_.HasCustom() ? 1 : 0);
    if (const auto it = strokeMatCache_.find(sig); it != strokeMatCache_.end()) return it->second;

    // Bake the current brush tip to a 128^2 RGBA texture (white RGB, A = shape).
    const u32 dim = 128;
    const paint::BrushTip tip = paint::MakeBrushTip(brushDef_, dim);
    uaf::Texture tex;
    tex.width = dim;
    tex.height = dim;
    tex.format = static_cast<u32>(rhi::Format::R8G8B8A8_UNORM);
    tex.mipCount = 1;
    tex.pixels.assign(static_cast<usize>(dim) * dim * 4, 0);
    for (usize i = 0; i < static_cast<usize>(dim) * dim; ++i) {
        tex.pixels[i * 4 + 0] = 255;
        tex.pixels[i * 4 + 1] = 255;
        tex.pixels[i * 4 + 2] = 255;
        tex.pixels[i * 4 + 3] = static_cast<u8>(glm::clamp(tip.alpha[i], 0.0f, 1.0f) * 255.0f);
    }
    std::error_code ec;
    std::filesystem::create_directories(assets / "Strokes", ec);
    const int id = strokeMatCounter_++;
    const std::string tipRel = "Strokes/stroketip_" + std::to_string(id) + ".uaf";
    const std::string matRel = "Strokes/stroke_" + std::to_string(id) + ".hbmat";
    if (!uaf::WriteTexture(assets / tipRel, tex)) return {};
    MaterialAsset m;
    m.name = "Stroke";
    m.albedoTex = tipRel;
    m.baseColor = brushColor_;   // tint; .a = stroke opacity
    m.metallic = brushMetallic_;
    m.roughness = brushRoughness_;
    // Free-standing strokes float just off the surface; casting a shadow back
    // onto it looks wrong, so they don't cast by default.
    m.flags = rhi::MaterialFlag_Transparent | rhi::MaterialFlag_NoShadow;
    if (!assets::SaveMaterial(assets / matRel, m)) return {};
    strokeMatCache_[sig] = matRel;
    return matRel;
}

std::string Editor::EnsureRibbonMaterial() {
    if (!Project::HasActive()) return {};
    const std::filesystem::path assets = Project::Active().AssetsDir();
    char sig[224];
    std::snprintf(sig, sizeof(sig), "rib|%d|%.2f|%.2f|%.2f|%.3f|%.3f|%.3f|%.3f|%.2f|%.2f",
                  brushDef_.shape, brushDef_.hardness, brushDef_.bristles, brushDef_.grain,
                  brushColor_.r, brushColor_.g, brushColor_.b, brushColor_.a,
                  brushMetallic_, brushRoughness_);
    if (const auto it = strokeMatCache_.find(sig); it != strokeMatCache_.end()) return it->second;

    // Bake a LONG bristle-streak texture (u = along the stroke, v = across): a soft
    // cross-section, bristle lines + value variation running along it, RGB = the brush
    // colour. Tiled along the ribbon it reads as one loaded oil mark, not blobs.
    const u32 dim = 128;
    const auto hash = [](int x, int y) {
        u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (h & 0xFFFFFFu) / static_cast<f32>(0xFFFFFF);
    };
    const auto vnoise = [&](f32 x, f32 y) {
        const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
        const f32 fx = x - xi, fy = y - yi;
        const f32 sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
        const f32 a = hash(xi, yi), b = hash(xi + 1, yi), c = hash(xi, yi + 1), d = hash(xi + 1, yi + 1);
        return glm::mix(glm::mix(a, b, sx), glm::mix(c, d, sx), sy);
    };
    const f32 hard = glm::clamp(brushDef_.hardness, 0.0f, 1.0f);
    const f32 bris = glm::clamp(brushDef_.bristles, 0.0f, 1.0f);
    const f32 grain = glm::clamp(brushDef_.grain, 0.0f, 1.0f);
    uaf::Texture tex;
    tex.width = dim;
    tex.height = dim;
    tex.format = static_cast<u32>(rhi::Format::R8G8B8A8_UNORM);
    tex.mipCount = 1;
    tex.pixels.assign(static_cast<usize>(dim) * dim * 4, 0);
    for (u32 y = 0; y < dim; ++y) {
        for (u32 x = 0; x < dim; ++x) {
            const f32 fu = (x + 0.5f) / dim; // along the stroke
            const f32 fv = (y + 0.5f) / dim; // across the stroke
            // Cross-section: a SOLID opaque core with a feathered edge (a proper paint
            // brush, not a faint smear). Soft brush -> wide feather, hard -> crisp edge.
            const f32 d = 1.0f - std::fabs(fv * 2.0f - 1.0f); // 0 at edges, 1 centre
            const f32 feather = glm::mix(0.5f, 0.1f, hard);
            f32 a = glm::smoothstep(0.0f, feather, d);        // opaque plateau + soft rim
            // Bristle/grain only break up a brush that asks for it (kept subtle).
            const f32 bristle = vnoise(fv * 22.0f, fu * 4.0f);
            if (bris > 0.0f) a *= glm::mix(1.0f, 0.45f + 0.55f * bristle, bris);
            if (grain > 0.0f) {
                const f32 g = vnoise(fu * 30.0f, fv * 30.0f);
                a *= glm::mix(1.0f, g < 0.45f ? 0.55f : 1.0f, grain);
            }
            a = glm::clamp(a, 0.0f, 1.0f);
            // Grayscale VALUE (mostly full so the colour reads clean); subtle bristle
            // darkening only. Modulates both the lit albedo AND the emissive (shadeless
            // colour), so the mark shows the picked colour with a touch of paint texture.
            f32 val = 1.0f;
            if (bris > 0.0f) val = glm::clamp(1.0f - (1.0f - bristle) * 0.35f * bris, 0.55f, 1.0f);
            const u8 vb = static_cast<u8>(val * 255.0f);
            const usize i = (static_cast<usize>(y) * dim + x) * 4;
            tex.pixels[i + 0] = vb;
            tex.pixels[i + 1] = vb;
            tex.pixels[i + 2] = vb;
            tex.pixels[i + 3] = static_cast<u8>(a * 255.0f);
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(assets / "Strokes", ec);
    const int id = strokeMatCounter_++;
    const std::string tipRel = "Strokes/ribbontip_" + std::to_string(id) + ".uaf";
    const std::string matRel = "Strokes/ribbonmat_" + std::to_string(id) + ".hbmat";
    if (!uaf::WriteTexture(assets / tipRel, tex)) return {};
    MaterialAsset m;
    m.name = "Stroke";
    m.albedoTex = tipRel;
    // LIT paint: the picked colour is the ALBEDO, so the stroke reacts to the scene -
    // the sun/lights/IBL shade and tint it and shadows darken it, like any surface.
    // A small emissive FLOOR (same texture, dim) keeps the paint readable in shadow
    // or at night so it never crushes to black; lighting dominates above it.
    m.baseColor = brushColor_; // .a = stroke opacity
    m.metallic = brushMetallic_;
    m.roughness = brushRoughness_;
    m.emissiveTex = tipRel;
    m.emissiveColor = glm::vec3(brushColor_) * 0.12f; // self-lit floor
    m.emissiveIntensity = 1.0f;
    // NoShadow: free-floating strokes shouldn't cast onto the surface they hover over
    // (they still RECEIVE the shadowed sun + lights, so they react to lighting).
    // DepthWrite: the stroke writes depth + velocity so depth of field + TAA treat it
    // as a real in-focus surface, instead of blurring it where it floats off a surface.
    m.flags = rhi::MaterialFlag_Transparent | rhi::MaterialFlag_NoShadow |
              rhi::MaterialFlag_DepthWrite;
    if (!assets::SaveMaterial(assets / matRel, m)) return {};
    strokeMatCache_[sig] = matRel;
    return matRel;
}

// Small deterministic per-stroke depth offset (0..~12mm) so stacked, otherwise-coplanar
// brush strokes don't z-fight. A high-frequency hash of the stroke centroid means even
// near-identical positions map to well-separated offsets; being position-derived (not a
// running counter) it never accumulates into a visible float and is stable across
// reload/replay.
static float StrokeDepthJitter(const glm::vec3& seed) {
    const float s =
        std::sin(seed.x * 127.1f + seed.y * 311.7f + seed.z * 74.7f) * 43758.5453f;
    return 0.012f * (s - std::floor(s));
}

void Editor::SpawnStroke(Engine& engine, const glm::vec3& hitWorld, const glm::vec3& worldNormal) {
    if (!Project::HasActive()) return;
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    auto& reg = scene.Registry();
    if (!strokeQuadMesh_.IsValid())
        strokeQuadMesh_ = renderer.UploadMesh(mesh::GeneratePlane(1.0f, 1));
    if (!strokeQuadMesh_.IsValid()) return;
    const std::string matRel = EnsureStrokeMaterial();
    if (matRel.empty()) return;

    PushUndo(scene);
    const std::filesystem::path assets = Project::Active().AssetsDir();
    const entt::entity e = scene.CreateEntity("Stroke");
    // Orient the quad's +Y axis to the surface normal.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 N = glm::normalize(worldNormal);
    const float d = glm::clamp(glm::dot(up, N), -1.0f, 1.0f);
    glm::quat q;
    if (d > 0.9999f) q = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    else if (d < -0.9999f) q = glm::angleAxis(3.14159265f, glm::vec3(1.0f, 0.0f, 0.0f));
    else q = glm::angleAxis(std::acos(d), glm::normalize(glm::cross(up, N)));
    Transform t;
    // Lift off the surface + a per-stroke depth nudge so stacked taps don't z-fight.
    t.position = hitWorld + N * (0.02f + StrokeDepthJitter(hitWorld));
    t.rotation = q;
    t.scale = glm::vec3(brushRadius_ * 2.0f, 1.0f, brushRadius_ * 2.0f);
    reg.emplace<Transform>(e, t);

    const std::optional<MaterialAsset> mat = assets::LoadMaterial(assets / matRel);
    MeshInstance mi;
    mi.mesh = strokeQuadMesh_;
    mi.baseColor = brushColor_;
    if (mat) assets::ApplyMaterial(renderer, assets, *mat, mi, textureCache_);
    reg.emplace<MeshInstance>(e, mi);
    reg.emplace<MeshRef>(e, MeshRef{"prim:plane"});
    if (mat) reg.emplace<MaterialRef>(e, MaterialRef{matRel});
    glm::vec3 mn, mx;
    ComputeBounds(mesh::GeneratePlane(1.0f, 1), mn, mx);
    reg.emplace<AABB>(e, AABB{mn, mx});
    selected_ = e;
}

void Editor::BuildSplineStroke(Engine& engine) {
    if (!Project::HasActive() || strokePath_.empty()) {
        strokePath_.clear(); strokePathN_.clear();
        return;
    }
    // A tap (single point) just drops a quad stroke.
    if (strokePath_.size() < 2) {
        SpawnStroke(engine, strokePath_[0],
                    strokePathN_.empty() ? glm::vec3(0, 1, 0) : strokePathN_[0]);
        strokePath_.clear(); strokePathN_.clear();
        return;
    }
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    auto& reg = scene.Registry();
    const std::string matRel = EnsureRibbonMaterial();

    // --- Stroke dynamics (Photoshop/GIMP-style): smooth the drawn path, then taper
    // the ends to a point, jitter the width, and wobble the path - so the mark is a
    // gestural brush stroke, not a uniform ribbon. All per-point, in world space.
    std::vector<glm::vec3> pts = strokePath_;
    const std::vector<glm::vec3>& nrm = strokePathN_;
    const int n0 = static_cast<int>(pts.size());
    const f32 sm = glm::clamp(brushDef_.smoothing, 0.0f, 1.0f);
    if (sm > 0.0f) {
        const int passes = 1 + static_cast<int>(sm * 3.0f);
        for (int p = 0; p < passes; ++p) {
            std::vector<glm::vec3> tmp = pts;
            for (int i = 1; i < n0 - 1; ++i)
                tmp[i] = glm::mix(pts[i], (pts[i - 1] + pts[i + 1]) * 0.5f, sm * 0.6f);
            pts.swap(tmp);
        }
    }
    // Deterministic per-stroke noise (so a rebuild is identical).
    const u32 seed = static_cast<u32>(strokeMeshCounter_) * 2654435761u + 1u;
    const auto nz = [&](f32 x) {
        const f32 s = std::sin(x * 12.9898f + static_cast<f32>(seed) * 1e-4f) * 43758.5453f;
        return s - std::floor(s); // 0..1
    };
    std::vector<f32> len(n0, 0.0f);
    for (int i = 1; i < n0; ++i) len[i] = len[i - 1] + glm::distance(pts[i], pts[i - 1]);
    const f32 total = std::max(len.back(), 1e-4f);
    const f32 baseHalf = brushRadius_ * 1.4f;
    const f32 ts = glm::clamp(brushDef_.taperStart, 0.0f, 1.0f);
    const f32 te = glm::clamp(brushDef_.taperEnd, 0.0f, 1.0f);
    const f32 sj = glm::clamp(brushDef_.sizeJitter, 0.0f, 1.0f);
    const f32 wob = glm::clamp(brushDef_.wobble, 0.0f, 1.0f);
    std::vector<f32> widths(n0);
    for (int i = 0; i < n0; ++i) {
        const f32 t = len[i] / total;
        const f32 a0 = (ts > 1e-3f) ? glm::smoothstep(0.0f, ts, t) : 1.0f;        // start taper
        const f32 a1 = (te > 1e-3f) ? glm::smoothstep(0.0f, te, 1.0f - t) : 1.0f; // end taper
        f32 w = baseHalf * std::max(a0 * a1, 0.04f);
        if (sj > 0.0f) w *= 1.0f + (nz(t * 7.0f + 3.1f) - 0.5f) * sj * 1.2f;      // size jitter
        widths[i] = std::max(w, baseHalf * 0.02f);
        if (wob > 0.0f && i > 0 && i < n0 - 1) {                                   // path wobble
            const glm::vec3 N = glm::normalize(nrm[i]);
            const glm::vec3 T = glm::normalize(pts[i + 1] - pts[i - 1]);
            const glm::vec3 Bv = glm::normalize(glm::cross(N, T));
            pts[i] += Bv * ((nz(t * 9.0f + 17.0f) - 0.5f) * 2.0f * wob * baseHalf);
        }
    }
    // Double-sided + arc-length UV so the stroke shows from any angle.
    MeshData ribbon = paint::BuildRibbon(pts, nrm, widths, true, true);
    if (ribbon.vertices.empty()) { strokePath_.clear(); strokePathN_.clear(); return; }

    // Center the ribbon at the path centroid; the entity transform places it.
    glm::vec3 centroid(0.0f);
    for (const glm::vec3& p : pts) centroid += p;
    centroid /= static_cast<f32>(pts.size());
    for (Vertex& v : ribbon.vertices) v.position -= centroid;

    const std::filesystem::path assets = Project::Active().AssetsDir();
    std::error_code ec;
    std::filesystem::create_directories(assets / "Strokes", ec);
    const std::string meshRel = "Strokes/ribbon_" + std::to_string(strokeMeshCounter_++) + ".uaf";
    uaf::WriteMesh(assets / meshRel, Model{ribbon}); // persist the ribbon geometry

    PushUndo(scene);
    const rhi::MeshHandle handle = renderer.UploadMesh(ribbon);
    const entt::entity e = scene.CreateEntity("Stroke");
    Transform t;
    // Per-stroke depth nudge along the draw-plane normal. Every ribbon is lifted the
    // SAME +0.03 in BuildRibbon, so where several strokes cross they become coplanar
    // and z-fight. A tiny deterministic offset (high-frequency hash of the centroid,
    // 0..~12mm) decorrelates their depths so they layer cleanly. Hash-based (not a
    // counter) so it's stable across reload/replay and never accumulates into a
    // visible float no matter how many strokes the object carries.
    t.position = centroid + strokePlaneN_ * StrokeDepthJitter(centroid);
    reg.emplace<Transform>(e, t);
    const std::optional<MaterialAsset> mat =
        matRel.empty() ? std::nullopt : assets::LoadMaterial(assets / matRel);
    MeshInstance mi;
    mi.mesh = handle;
    mi.baseColor = brushColor_;
    if (mat) assets::ApplyMaterial(renderer, assets, *mat, mi, textureCache_);
    reg.emplace<MeshInstance>(e, mi);
    reg.emplace<MeshRef>(e, MeshRef{"uaf:" + meshRel + "#0"});
    if (mat) reg.emplace<MaterialRef>(e, MaterialRef{matRel});
    glm::vec3 mn, mx;
    ComputeBounds(ribbon, mn, mx);
    reg.emplace<AABB>(e, AABB{mn, mx});
    // Group strokes under one "Paint Strokes" node so they don't clutter the root
    // hierarchy. Find-or-create by name (reused across strokes + scene reloads); it's
    // an empty node at the origin, so a child keeps its world position. The whole
    // group moves/duplicates/hides together and collapses to a single tree row.
    entt::entity group = entt::null;
    for (const entt::entity ge : reg.view<Name>())
        if (reg.get<Name>(ge).value == "Paint Strokes") { group = ge; break; }
    if (group == entt::null) {
        group = scene.CreateEntity("Paint Strokes");
        reg.emplace<Transform>(group, Transform{});
    }
    reg.emplace<Parent>(e, Parent{group});
    selected_ = e;
    strokePath_.clear(); strokePathN_.clear();
}

void Editor::EnsureBrushes() {
    if (brushesLoaded_) return;
    LoadBrushes();
    brushesLoaded_ = true;
}

void Editor::SelectBrush(int index) {
    if (brushes_.empty()) return;
    brushIndex_ = glm::clamp(index, 0, static_cast<int>(brushes_.size()) - 1);
    brushDef_ = brushes_[brushIndex_];
    std::snprintf(brushNameBuf_, sizeof(brushNameBuf_), "%s", brushDef_.name.c_str());
    // Selecting a preset also seeds the live tool defaults it shipped with.
    brushFlow_ = brushDef_.flow;
    brushRadius_ = brushDef_.size;
    brushHeight_ = brushDef_.relief;
    brushColorVar_ = brushDef_.colorVar; // painterly colour pooling (the Colour Var brush)
    brushDirty_ = true;
}

void Editor::LoadBrushes() {
    brushes_.clear();
    if (Project::HasActive()) {
        std::ifstream in(Project::Active().Root() / "brushes.json");
        if (in) {
            try {
                nlohmann::json j;
                in >> j;
                for (const auto& e : j.value("brushes", nlohmann::json::array())) {
                    paint::BrushDef d;
                    d.name = e.value("name", d.name);
                    d.shape = e.value("shape", d.shape);
                    d.hardness = e.value("hardness", d.hardness);
                    d.grain = e.value("grain", d.grain);
                    d.bristles = e.value("bristles", d.bristles);
                    d.scatter = e.value("scatter", d.scatter);
                    d.flow = e.value("flow", d.flow);
                    d.spacing = e.value("spacing", d.spacing);
                    d.size = e.value("size", d.size);
                    d.relief = e.value("relief", d.relief);
                    d.mode = e.value("mode", d.mode);
                    d.colorVar = e.value("colorVar", d.colorVar);
                    d.taperStart = e.value("taperStart", d.taperStart);
                    d.taperEnd = e.value("taperEnd", d.taperEnd);
                    d.sizeJitter = e.value("sizeJitter", d.sizeJitter);
                    d.wobble = e.value("wobble", d.wobble);
                    d.smoothing = e.value("smoothing", d.smoothing);
                    d.customSize = e.value("customSize", 0u);
                    if (e.contains("customAlpha"))
                        d.customAlpha = e["customAlpha"].get<std::vector<u8>>();
                    brushes_.push_back(std::move(d));
                }
            } catch (const std::exception&) {
                // Corrupt library: fall back to defaults below.
            }
        }
    }
    if (brushes_.empty()) brushes_ = paint::DefaultBrushes();
    // Merge in any built-in brushes the saved library is missing, so new presets
    // (the painting kit) show up in existing projects without losing custom ones.
    for (const paint::BrushDef& def : paint::DefaultBrushes()) {
        bool have = false;
        for (const paint::BrushDef& b : brushes_)
            if (b.name == def.name) { have = true; break; }
        if (!have) brushes_.push_back(def);
    }
    SelectBrush(brushIndex_);
}

void Editor::SaveBrushes() const {
    if (!Project::HasActive()) return;
    nlohmann::json j;
    auto& arr = j["brushes"] = nlohmann::json::array();
    for (const paint::BrushDef& d : brushes_) {
        nlohmann::json e = {{"name", d.name}, {"shape", d.shape}, {"hardness", d.hardness},
                            {"grain", d.grain}, {"bristles", d.bristles}, {"scatter", d.scatter},
                            {"flow", d.flow}, {"spacing", d.spacing}, {"size", d.size},
                            {"relief", d.relief}, {"mode", d.mode}, {"colorVar", d.colorVar},
                            {"taperStart", d.taperStart}, {"taperEnd", d.taperEnd},
                            {"sizeJitter", d.sizeJitter}, {"wobble", d.wobble},
                            {"smoothing", d.smoothing}};
        if (d.HasCustom()) {
            e["customSize"] = d.customSize;
            e["customAlpha"] = d.customAlpha; // grayscale stamp (image / hand-painted)
        }
        arr.push_back(std::move(e));
    }
    std::ofstream out(Project::Active().Root() / "brushes.json");
    if (out) out << j.dump(2);
}

namespace {
// Loads an image file and downsamples it to a `size` x `size` grayscale brush
// stamp (uses the alpha channel when the image has one, else luminance).
bool LoadBrushStamp(const std::filesystem::path& path, u32 size, std::vector<u8>& out) {
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path.string().c_str(), &w, &h, &ch, 4); // force RGBA
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    const bool hasAlpha = (ch == 2 || ch == 4);
    out.assign(static_cast<usize>(size) * size, 0);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const int sx = std::min(static_cast<int>((x + 0.5f) / size * w), w - 1);
            const int sy = std::min(static_cast<int>((y + 0.5f) / size * h), h - 1);
            const stbi_uc* p = px + (static_cast<usize>(sy) * w + sx) * 4;
            const u8 cov = hasAlpha
                               ? p[3]
                               : static_cast<u8>((p[0] * 30 + p[1] * 59 + p[2] * 11) / 100);
            out[static_cast<usize>(y) * size + x] = cov;
        }
    }
    stbi_image_free(px);
    return true;
}
} // namespace

void Editor::DrawBrushEditor() {
    // Preset library.
    for (int i = 0; i < static_cast<int>(brushes_.size()); ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        const bool sel = brushIndex_ == i;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button((brushes_[i].name + "##bp" + std::to_string(i)).c_str(), ImVec2(78, 0)))
            SelectBrush(i);
        if (sel) ImGui::PopStyleColor();
    }

    if (brushDirty_ || !brushTip_.Valid()) {
        brushTip_ = paint::MakeBrushTip(brushDef_);
        brushDirty_ = false;
    }

    // Live preview of the working brush tip in the current colour.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float box = 64.0f;
        dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), IM_COL32(32, 32, 36, 255), 4.0f);
        if (brushTip_.Valid()) {
            const int N = 30;
            const float cell = box / N;
            for (int gy = 0; gy < N; ++gy)
                for (int gx = 0; gx < N; ++gx) {
                    const float a = brushTip_.Sample((gx + 0.5f) / N * (brushTip_.size - 1),
                                                     (gy + 0.5f) / N * (brushTip_.size - 1)) *
                                    brushFlow_;
                    if (a <= 0.01f) continue;
                    const ImVec2 c0(p.x + gx * cell, p.y + gy * cell);
                    dl->AddRectFilled(c0, ImVec2(c0.x + cell + 1.0f, c0.y + cell + 1.0f),
                                      ImGui::GetColorU32(ImVec4(brushColor_.r, brushColor_.g,
                                                               brushColor_.b, a)));
                }
        }
        ImGui::Dummy(ImVec2(box, box));
    }

    // Editable parameters of the working brush.
    if (ImGui::TreeNode("Edit brush")) {
        bool ch = false;
        ch |= ImGui::Combo("Shape##bd", &brushDef_.shape, "Round\0Flat\0");
        ch |= ImGui::SliderFloat("Hardness##bd", &brushDef_.hardness, 0.0f, 1.0f, "%.2f");
        ch |= ImGui::SliderFloat("Grain##bd", &brushDef_.grain, 0.0f, 1.0f, "%.2f");
        ch |= ImGui::SliderFloat("Bristles##bd", &brushDef_.bristles, 0.0f, 1.0f, "%.2f");
        ch |= ImGui::SliderFloat("Scatter##bd", &brushDef_.scatter, 0.0f, 1.0f, "%.2f");
        ch |= ImGui::SliderFloat("Spacing##bd", &brushDef_.spacing, 0.02f, 0.6f, "%.2f");
        ImGui::Combo("Mode##bd", &brushDef_.mode, "Paint\0Smudge\0Edge Darken\0");
        if (ImGui::SliderFloat("Colour Var##bd", &brushDef_.colorVar, 0.0f, 1.0f, "%.2f"))
            brushColorVar_ = brushDef_.colorVar;
        if (ch) brushDirty_ = true;
        // Stroke dynamics (Photoshop/GIMP-style): shape the whole 3D brush stroke -
        // a real start/end taper, procedural width + path variation while painting,
        // and path smoothing. (Take effect on the next stroke; no tip rebake needed.)
        ImGui::SeparatorText("Stroke dynamics (3D)");
        ImGui::SliderFloat("Taper start##bd", &brushDef_.taperStart, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Taper end##bd", &brushDef_.taperEnd, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Size jitter##bd", &brushDef_.sizeJitter, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Wobble##bd", &brushDef_.wobble, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Smoothing##bd", &brushDef_.smoothing, 0.0f, 1.0f, "%.2f");
        ImGui::InputText("Name##bd", brushNameBuf_, sizeof(brushNameBuf_));
        if (ImGui::Button("Save as New")) {
            paint::BrushDef d = brushDef_;
            d.name = brushNameBuf_[0] ? brushNameBuf_ : "Brush";
            d.flow = brushFlow_; d.size = brushRadius_; d.relief = brushHeight_;
            brushes_.push_back(d);
            SelectBrush(static_cast<int>(brushes_.size()) - 1);
            SaveBrushes();
        }
        ImGui::SameLine();
        if (ImGui::Button("Update")) {
            brushDef_.name = brushNameBuf_[0] ? brushNameBuf_ : brushDef_.name;
            brushDef_.flow = brushFlow_; brushDef_.size = brushRadius_; brushDef_.relief = brushHeight_;
            if (brushIndex_ < static_cast<int>(brushes_.size())) brushes_[brushIndex_] = brushDef_;
            SaveBrushes();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(brushes_.size() <= 1);
        if (ImGui::Button("Delete")) {
            brushes_.erase(brushes_.begin() + brushIndex_);
            SelectBrush(brushIndex_); // clamps
            SaveBrushes();
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    // Custom tip: import an image or hand-paint the stamp pixel-by-pixel.
    if (ImGui::TreeNode("Custom tip (image / pixels)")) {
        constexpr u32 kStamp = 32; // edit/import resolution
        if (ImGui::Button("Import Image...")) {
            wchar_t file[2048] = {};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = static_cast<DWORD>(std::size(file));
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn) &&
                LoadBrushStamp(std::filesystem::path(file), kStamp, brushDef_.customAlpha)) {
                brushDef_.customSize = kStamp;
                brushDirty_ = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Paint blank")) {
            brushDef_.customSize = kStamp;
            brushDef_.customAlpha.assign(kStamp * kStamp, 0);
            brushDirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("From current")) {
            // Bake the current (procedural) tip into editable pixels.
            paint::BrushDef proc = brushDef_;
            proc.customSize = 0; proc.customAlpha.clear();
            const paint::BrushTip tip = paint::MakeBrushTip(proc, kStamp);
            brushDef_.customSize = kStamp;
            brushDef_.customAlpha.resize(kStamp * kStamp);
            for (usize i = 0; i < brushDef_.customAlpha.size(); ++i)
                brushDef_.customAlpha[i] = static_cast<u8>(std::clamp(tip.alpha[i], 0.0f, 1.0f) * 255.0f);
            brushDirty_ = true;
        }
        if (brushDef_.HasCustom()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                brushDef_.customAlpha.clear();
                brushDef_.customSize = 0;
                brushDirty_ = true;
            }
            // Interactive pixel grid: LMB paints, RMB erases.
            const u32 cs = brushDef_.customSize;
            const float gridPx = 168.0f;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##pixedit", ImVec2(gridPx, gridPx));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float cell = gridPx / cs;
            dl->AddRectFilled(origin, ImVec2(origin.x + gridPx, origin.y + gridPx),
                              IM_COL32(18, 18, 22, 255));
            for (u32 y = 0; y < cs; ++y)
                for (u32 x = 0; x < cs; ++x) {
                    const float a = brushDef_.customAlpha[y * cs + x] / 255.0f;
                    if (a <= 0.02f) continue;
                    const ImVec2 c0(origin.x + x * cell, origin.y + y * cell);
                    dl->AddRectFilled(c0, ImVec2(c0.x + cell + 0.5f, c0.y + cell + 0.5f),
                                      ImGui::GetColorU32(ImVec4(brushColor_.r, brushColor_.g,
                                                               brushColor_.b, a)));
                }
            dl->AddRect(origin, ImVec2(origin.x + gridPx, origin.y + gridPx), IM_COL32(90, 90, 96, 255));
            const bool erase = ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (ImGui::IsItemActive() || erase) {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const int gx = static_cast<int>((mp.x - origin.x) / cell);
                const int gy = static_cast<int>((mp.y - origin.y) / cell);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = gx + dx, yy = gy + dy;
                        if (xx < 0 || yy < 0 || xx >= static_cast<int>(cs) || yy >= static_cast<int>(cs))
                            continue;
                        if (dx * dx + dy * dy > 2) continue; // round-ish nib
                        brushDef_.customAlpha[static_cast<usize>(yy) * cs + xx] = erase ? 0 : 255;
                    }
                brushDirty_ = true;
            }
            ImGui::TextDisabled("LMB paint, RMB erase");
        }
        ImGui::TreePop();
    }
}

void Editor::DrawArtEditor(Engine& engine) {
    if (!panelOpen_[Panel_ArtEditor]) return;
    if (!ImGui::Begin("Art Editor", &panelOpen_[Panel_ArtEditor])) {
        ImGui::End();
        return;
    }
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    auto& reg = scene.Registry();

    if (!Project::HasActive()) {
        ImGui::TextDisabled("Open a project to paint surfaces.");
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Paint mode", &paintActive_);
    ImGui::SameLine();
    ImGui::TextDisabled("(drag in the Scene view)");
    // 3D brush strokes (grease-pencil): drag to lay a brush stroke as its own entity,
    // on a plane tangent to the surface you START on (it goes off the surface's
    // rotation, not the camera's). Off = the brush paints into the surface texture.
    ImGui::Checkbox("3D brush strokes", &paintStrokeMode_);
    ImGui::SameLine();
    ImGui::TextDisabled(paintStrokeMode_ ? "(drag: brush stroke on the surface)"
                                         : "(off: brush paints the surface texture)");

    // Save: writes the scene AND every paint canvas (.hbpaint) to disk.
    if (ImGui::Button("Save Scene")) {
        SaveCurrent(scene);
    }
    ImGui::SameLine();
    if (currentScenePath_.empty())
        ImGui::TextDisabled("(unsaved scene)");
    else
        ImGui::TextDisabled("%s", currentScenePath_.stem().string().c_str());

    ImGui::SeparatorText("Brush");
    EnsureBrushes();
    DrawBrushEditor(); // preset library + custom-brush editor + live preview
    ImGui::Checkbox("Erase", &paintErase_);
    ImGui::SameLine();
    // Masking: restrict strokes to the selected object (ignore everything else).
    ImGui::Checkbox("Selected only", &paintSelectedOnly_);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-create", &paintAutoCreate_); // every mesh paintable on first stroke
    ImGui::SliderFloat("Size (world)", &brushRadius_, 0.002f, 5.0f, "%.3f",
                       ImGuiSliderFlags_Logarithmic); // allows very small brushes
    ImGui::SliderFloat("Flow", &brushFlow_, 0.05f, 1.0f, "%.2f");
    ImGui::SliderFloat("Relief", &brushHeight_, -1.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Colour variation", &brushColorVar_, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Broken colour: each dab shifts value + warm/cool a little,\n"
                          "so strokes read as mixed oil paint, not flat fill.");

    ImGui::Checkbox("Paint colour", &brushPaintColor_);
    ImGui::SameLine();
    ImGui::Checkbox("Paint material", &brushPaintMaterial_);

    ImGui::SeparatorText("Colour");
    ImGui::ColorPicker4("##brushcolor", &brushColor_.x,
                        ImGuiColorEditFlags_PickerHueWheel |
                            ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_AlphaBar);
    // Eyedropper: sample a colour straight off the rendered scene. Arm it, then click
    // anywhere in the Scene view and that on-screen pixel becomes the brush colour.
    if (colorPickMode_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Picking - click the scene (Esc to cancel)")) colorPickMode_ = false;
        ImGui::PopStyleColor();
    } else if (ImGui::Button("Pick from scene (eyedropper)")) {
        colorPickMode_ = true;
    }

    // PBR material the brush lays down (when "Paint material" is on).
    ImGui::SeparatorText("Material");
    ImGui::SliderFloat("Metallic", &brushMetallic_, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Roughness", &brushRoughness_, 0.0f, 1.0f, "%.2f");
    if (ImGui::BeginCombo("From material", "(load .hbmat)")) {
        for (const std::string& mp : ListAssetsByExt(".hbmat")) {
            if (ImGui::Selectable(mp.c_str())) {
                if (auto mat = assets::LoadMaterial(Project::Active().AssetsDir() / mp)) {
                    brushColor_ = mat->baseColor;
                    brushMetallic_ = mat->metallic;
                    brushRoughness_ = mat->roughness;
                    brushPaintMaterial_ = true;
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("New canvas");
    const char* resNames[] = {"256", "512", "1024", "2048"};
    const int resVals[] = {256, 512, 1024, 2048};
    int curRes = 2;
    for (int i = 0; i < 4; ++i)
        if (resVals[i] == paintRes_) curRes = i;
    if (ImGui::Combo("Resolution", &curRes, resNames, 4)) paintRes_ = resVals[curRes];
    ImGui::SliderFloat("Opacity", &paintOpacity_, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Relief (normal)##new", &paintReliefDefault_);
    if (paintReliefDefault_)
        ImGui::SliderFloat("Relief scale", &paintHeightScale_, 0.0f, 0.3f, "%.3f");
    ImGui::SliderFloat("Distance LOD", &paintLodBias_, 0.0f, 4.0f, "%.2f");

    const bool selValid = selected_ != entt::null && reg.valid(selected_);
    const bool selMesh = selValid && reg.all_of<MeshInstance>(selected_);
    ImGui::BeginDisabled(!selMesh);
    if (ImGui::Button("Make Selected Paintable")) {
        PushUndo(scene);
        MakePaintable(scene, renderer, selected_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Make All Paintable")) {
        PushUndo(scene);
        for (const entt::entity e : reg.view<MeshInstance>())
            MakePaintable(scene, renderer, e);
    }

    ImGui::BeginDisabled(paintStrokeOrder_.empty());
    if (ImGui::Button("Undo Stroke")) PaintUndo(engine);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(paintStrokeRedo_.empty());
    if (ImGui::Button("Redo Stroke")) PaintRedo(engine);
    ImGui::EndDisabled();

    // Object groups: organizational groups of paintable OBJECTS (distinct from a
    // canvas's paint-layer stack). The active group masks the brush; a group can
    // show/hide/lock all its members together.
    ImGui::SeparatorText("Object groups");
    std::set<std::string> layerSet{"Default"};
    for (const entt::entity e : reg.view<PaintComponent>())
        layerSet.insert(reg.get<PaintComponent>(e).layer);
    const std::vector<std::string> layerList(layerSet.begin(), layerSet.end());

    if (ImGui::BeginCombo("Active", paintActiveLayer_.c_str())) {
        if (ImGui::Selectable("All", paintActiveLayer_ == "All")) paintActiveLayer_ = "All";
        for (const std::string& ln : layerList)
            if (ImGui::Selectable(ln.c_str(), paintActiveLayer_ == ln)) paintActiveLayer_ = ln;
        ImGui::EndCombo();
    }
    if (paintActiveLayer_ != "All")
        ImGui::TextDisabled("Brush + new canvases bind to '%s'.", paintActiveLayer_.c_str());
    else
        ImGui::TextDisabled("Painting any layer (no mask).");

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("##newlayer", paintNewLayerBuf_, sizeof(paintNewLayerBuf_));
    ImGui::SameLine();
    if (ImGui::Button("New / Activate") && paintNewLayerBuf_[0])
        paintActiveLayer_ = paintNewLayerBuf_; // becomes active; assign objects to it

    ImGui::BeginDisabled(!(selMesh && reg.all_of<PaintComponent>(selected_) &&
                           paintActiveLayer_ != "All"));
    if (ImGui::Button("Bind Selected")) {
        PushUndo(scene);
        reg.get<PaintComponent>(selected_).layer = paintActiveLayer_;
    }
    ImGui::EndDisabled();
    if (paintActiveLayer_ != "All") {
        const auto layerApply = [&](auto&& fn) {
            for (const entt::entity e : reg.view<PaintComponent>()) {
                PaintComponent& pc = reg.get<PaintComponent>(e);
                if (pc.layer == paintActiveLayer_) fn(pc);
            }
        };
        ImGui::SameLine();
        if (ImGui::SmallButton("Show")) layerApply([](PaintComponent& pc) { pc.enabled = true; });
        ImGui::SameLine();
        if (ImGui::SmallButton("Hide")) layerApply([](PaintComponent& pc) { pc.enabled = false; });
        ImGui::SameLine();
        if (ImGui::SmallButton("Lock")) layerApply([](PaintComponent& pc) { pc.locked = true; });
        ImGui::SameLine();
        if (ImGui::SmallButton("Unlock")) layerApply([](PaintComponent& pc) { pc.locked = false; });
    }

    ImGui::SeparatorText("Paintable objects");
    auto pv = reg.view<PaintComponent>();
    if (pv.begin() == pv.end())
        ImGui::TextDisabled("None yet - select a mesh and 'Make Paintable'.");
    entt::entity removeFrom = entt::null;
    for (const entt::entity e : pv) {
        PaintComponent& pc = pv.get<PaintComponent>(e);
        ImGui::PushID(static_cast<int>(e));
        const Name* nm = reg.try_get<Name>(e);
        const std::string label = nm && !nm->value.empty()
                                      ? nm->value
                                      : ("Entity " + std::to_string(static_cast<u32>(e)));
        if (ImGui::Selectable(label.c_str(), selected_ == e, 0, ImVec2(120, 0)))
            selected_ = e;
        ImGui::SameLine();
        ImGui::Checkbox("Show", &pc.enabled);
        ImGui::SameLine();
        ImGui::Checkbox("Lock", &pc.locked);
        ImGui::SameLine();
        if (ImGui::SmallButton("Fill")) {
            paint::Stroke s;
            s.type = paint::StrokeType::Fill;
            s.layer = pc.activeLayer;
            s.brush = brushDef_;
            s.color = brushColor_;
            s.metallic = brushMetallic_;
            s.roughness = brushRoughness_;
            s.paintColor = brushPaintColor_;
            s.paintMaterial = brushPaintMaterial_;
            paint::Dab dab;
            dab.color = brushColor_;
            dab.metallic = brushMetallic_;
            dab.roughness = brushRoughness_;
            dab.paintColor = brushPaintColor_;
            dab.paintMaterial = brushPaintMaterial_;
            paint::FillLayer(pc, pc.activeLayer, dab, false);
            paint::Sync(renderer, pc);
            CommitStroke(e, pc, std::move(s));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            paint::Stroke s;
            s.type = paint::StrokeType::Clear;
            s.layer = pc.activeLayer;
            paint::FillLayer(pc, pc.activeLayer, paint::Dab{}, true);
            paint::Sync(renderer, pc);
            CommitStroke(e, pc, std::move(s));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) removeFrom = e;
        ImGui::SliderFloat("Opacity##o", &pc.opacity, 0.0f, 1.0f, "%.2f");
        ImGui::Checkbox("Relief##re", &pc.reliefEnabled); // normal deformation on/off
        if (pc.reliefEnabled) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("##r", &pc.heightScale, 0.0f, 1.0f, "%.3f"); // up to heavy impasto
        }
        ImGui::SliderFloat("LOD##l", &pc.lodBias, 0.0f, 4.0f, "%.2f");
        // Projection: Mesh UV / Box (no-stretch auto-unwrap) / 3D projection paint
        // (stamps by surface proximity - crosses UV seams, never stretches).
        ImGui::SetNextItemWidth(130.0f);
        ImGui::Combo("Unwrap##proj", &pc.projection,
                     "Mesh UV\0Box (no stretch)\0Projection (3D)\0");
        ImGui::SameLine();
        // Layer assignment.
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo("Group##grp", pc.layer.c_str())) {
            for (const std::string& ln : layerList)
                if (ImGui::Selectable(ln.c_str(), pc.layer == ln)) pc.layer = ln;
            ImGui::EndCombo();
        }

        // Per-object PAINT LAYER STACK (only for the selected object to keep the
        // list compact). Layers composite top-over-bottom; the active layer (radio)
        // is the one the brush paints.
        if (selected_ == e) {
            ImGui::SeparatorText("Paint layers");
            ImGui::TextDisabled("radio = active (brush paints it); top of list = top of stack");
            int del = -1, swapA = -1, swapB = -1;
            for (int li = static_cast<int>(pc.layers.size()) - 1; li >= 0; --li) {
                PaintLayer& L = pc.layers[li];
                ImGui::PushID(1000 + li);
                if (ImGui::RadioButton("##act", pc.activeLayer == li)) pc.activeLayer = li;
                ImGui::SameLine();
                if (ImGui::Checkbox("##vis", &L.visible)) pc.dirty = true;
                ImGui::SameLine();
                char nb[64];
                std::snprintf(nb, sizeof(nb), "%s", L.name.c_str());
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputText("##nm", nb, sizeof(nb))) L.name = nb;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f);
                if (ImGui::SliderFloat("##op", &L.opacity, 0.0f, 1.0f, "%.2f")) pc.dirty = true;
                ImGui::SameLine();
                if (ImGui::SmallButton("^") && li + 1 < static_cast<int>(pc.layers.size())) {
                    swapA = li; swapB = li + 1;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("v") && li > 0) { swapA = li; swapB = li - 1; }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) del = li;
                ImGui::PopID();
            }
            if (ImGui::SmallButton("+ Add Layer")) {
                paint::AddLayer(pc, "Layer " + std::to_string(pc.layers.size() + 1));
                pc.activeLayer = static_cast<int>(pc.layers.size()) - 1;
                pc.dirty = true;
            }
            if (swapA >= 0) {
                std::swap(pc.layers[swapA], pc.layers[swapB]);
                if (pc.activeLayer == swapA) pc.activeLayer = swapB;       // follow the moved layer
                else if (pc.activeLayer == swapB) pc.activeLayer = swapA;
                pc.dirty = true;
            }
            if (del >= 0 && pc.layers.size() > 1) {
                pc.layers.erase(pc.layers.begin() + del);
                if (pc.activeLayer >= del) --pc.activeLayer;
                pc.dirty = true;
            }
            pc.activeLayer = glm::clamp(pc.activeLayer, 0, static_cast<int>(pc.layers.size()) - 1);
            if (pc.dirty) paint::Sync(renderer, pc);
        }

        ImGui::Separator();
        ImGui::PopID();
    }
    if (removeFrom != entt::null) {
        PushUndo(scene);
        reg.remove<PaintComponent>(removeFrom);
    }

    ImGui::End();
}

void Editor::UpdateArtTool(Engine& engine) {
    paintConsumedClick_ = false;
    // Eyedropper: while armed, the next left-click in the Scene view grabs that pixel's
    // on-screen colour (the lit, painted result you see) as the brush colour. Reads the
    // OS framebuffer at the cursor, so it samples exactly what's displayed.
    if (colorPickMode_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) colorPickMode_ = false;
        if (vpVisible_ && vpHovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            POINT pt{};
            if (::GetCursorPos(&pt)) {
                HDC dc = ::GetDC(nullptr); // screen DC
                const COLORREF c = ::GetPixel(dc, pt.x, pt.y);
                ::ReleaseDC(nullptr, dc);
                if (c != CLR_INVALID)
                    brushColor_ = glm::vec4(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                                            GetBValue(c) / 255.0f, brushColor_.a);
            }
            colorPickMode_ = false;
            paintConsumedClick_ = true; // don't paint / pick an entity with this click
        }
        return; // suppress painting while the eyedropper is armed
    }
    if (!paintActive_ || !vpVisible_ || freecamActive_) {
        paintStroking_ = false;
        return;
    }
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    auto& reg = scene.Registry();

    const ImVec2 mp = ImGui::GetIO().MousePos;
    const f32 mx = (mp.x - vpX_) / glm::max(vpW_, 1.0f);
    const f32 my = (mp.y - vpY_) / glm::max(vpH_, 1.0f);
    const bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool over = vpHovered_ && mx >= 0.0f && mx <= 1.0f && my >= 0.0f && my <= 1.0f;
    // Stroke ended (even off-surface): flush any throttled dabs to the GPU once,
    // then commit the recorded stroke to the database so it joins the undo history.
    // This runs regardless of `over`, so it's the one reliable release handler.
    if (paintStroking_ && !lmbDown) {
        if (paintTarget_ != entt::null && reg.valid(paintTarget_)) {
            if (PaintComponent* p = reg.try_get<PaintComponent>(paintTarget_)) {
                if (p->dirty) paint::Sync(renderer, *p);
                if (curStrokeActive_ && !curStroke_.path.empty())
                    CommitStroke(paintTarget_, *p, std::move(curStroke_));
            }
        }
        curStroke_ = paint::Stroke{};
        curStrokeActive_ = false;
        paintStroking_ = false;
        paintHasLast_ = false;
    }
    if (!over) {
        if (!lmbDown) paintStroking_ = false;
        paintHasLast_ = false; // re-entry starts a fresh stroke segment (no streak)
        return;
    }

    // Paint-in-space (grease-pencil 3D strokes): DRAG in screen space and the stroke
    // becomes its own free-floating ribbon entity. The first cursor-ray hit sets a
    // DRAW PLANE (surface depth + camera orientation); every later point projects onto
    // that plane, so the stroke floats in real space instead of conforming to the mesh.
    if (paintStrokeMode_) {
        if (renderer.IsOrbitEnabled()) { SyncFreecam(renderer); renderer.SetOrbitEnabled(false); }
        const bool lmb = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const Camera& cam = renderer.GetCamera();
        // Cursor view ray.
        const glm::mat4 invVP = glm::inverse(cam.ViewProjection());
        const glm::vec2 ndc(mx * 2.0f - 1.0f, 1.0f - my * 2.0f);
        glm::vec4 pn4 = invVP * glm::vec4(ndc, 0.0f, 1.0f);
        glm::vec4 pf4 = invVP * glm::vec4(ndc, 1.0f, 1.0f);
        pn4 /= pn4.w; pf4 /= pf4.w;
        const glm::vec3 ro(pn4), rd = glm::normalize(glm::vec3(pf4) - glm::vec3(pn4));
        // Intersect the cursor ray with the current draw plane.
        const auto onPlane = [&]() {
            const f32 denom = glm::dot(rd, strokePlaneN_);
            const f32 t = (std::fabs(denom) > 1e-6f)
                              ? glm::dot(strokePlaneP_ - ro, strokePlaneN_) / denom
                              : 5.0f;
            return ro + rd * glm::max(t, 0.05f);
        };

        if (lmb && !strokeDrawing_) {
            // Stroke start: the first surface hit under the cursor sets BOTH the draw-
            // plane depth AND its orientation - the plane is tangent to that surface
            // (normal = the surface normal), so the stroke lies flat ON the surface
            // it started on, not facing the camera. No hit (drawing over empty space)
            // -> fall back to a camera-facing plane at a default depth.
            glm::vec3 planeP = ro + rd * 5.0f;
            glm::vec3 planeN = glm::normalize(cam.Forward());
            const entt::entity tgt = EntityUnderPixel(scene, renderer, mx, my);
            if (tgt != entt::null && reg.valid(tgt)) {
                if (const MeshData* hm = GetCpuMesh(scene, tgt)) {
                    const glm::mat4 w = scene.WorldMatrix(tgt);
                    const glm::mat4 iw = glm::inverse(w);
                    paint::PaintHit hit;
                    if (paint::RaycastMesh(*hm, glm::vec3(iw * glm::vec4(ro, 1.0f)),
                                           glm::vec3(iw * glm::vec4(rd, 0.0f)), hit)) {
                        planeP = glm::vec3(w * glm::vec4(hit.localPos, 1.0f));
                        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(w)));
                        planeN = glm::normalize(nm * hit.localNormal); // surface normal
                        // RaycastMesh accepts either winding, so the geometric
                        // normal can point AWAY from the camera. The ribbon lifts
                        // along +N; if N faces away the stroke is pushed into the
                        // mesh and "falls through" (gets occluded). Force N to face
                        // the viewer so the lift always lands in front of the surface.
                        if (glm::dot(planeN, rd) > 0.0f) planeN = -planeN;
                    }
                }
            }
            strokePlaneP_ = planeP;
            strokePlaneN_ = planeN; // tangent to the starting surface (or camera if none)
            strokePath_.clear(); strokePathN_.clear();
            strokePath_.push_back(onPlane());
            strokePathN_.push_back(strokePlaneN_);
            strokeDrawing_ = true;
            paintConsumedClick_ = true;
        } else if (lmb && strokeDrawing_) {
            const glm::vec3 p = onPlane();
            const f32 minStep = glm::max(brushRadius_ * 0.3f, 0.004f);
            if (strokePath_.empty() || glm::distance(p, strokePath_.back()) >= minStep) {
                strokePath_.push_back(p);
                strokePathN_.push_back(strokePlaneN_);
            }
            paintConsumedClick_ = true;
        } else if (strokeDrawing_ && !lmb) {
            BuildSplineStroke(engine); // finish on release -> its own entity
            strokeDrawing_ = false;
        }
        // Preview the drawn path (projected to screen).
        if (strokeDrawing_ && strokePath_.size() >= 2) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->PushClipRect(ImVec2(vpX_, vpY_), ImVec2(vpX_ + vpW_, vpY_ + vpH_), true);
            const glm::mat4 vp = cam.ViewProjection();
            ImVec2 prev{};
            bool havePrev = false;
            const ImU32 col = ImGui::GetColorU32(ImVec4(brushColor_.r, brushColor_.g, brushColor_.b, 0.95f));
            for (const glm::vec3& wp : strokePath_) {
                const glm::vec4 clip = vp * glm::vec4(wp, 1.0f);
                if (clip.w <= 0.001f) { havePrev = false; continue; }
                const glm::vec2 nd = glm::vec2(clip) / clip.w;
                const ImVec2 sp(vpX_ + (nd.x * 0.5f + 0.5f) * vpW_, vpY_ + (0.5f - nd.y * 0.5f) * vpH_);
                if (havePrev) dl->AddLine(prev, sp, col, 3.0f);
                prev = sp; havePrev = true;
            }
            dl->PopClipRect();
        }
        return;
    }

    // Choose the paint target. "Selected only" masks to the selected mesh;
    // otherwise the mesh under the cursor. Every mesh is paintable - a canvas is
    // created on the first dab (auto-create), so the target need NOT already have
    // a PaintComponent.
    entt::entity target = entt::null;
    if (paintSelectedOnly_) {
        if (selected_ != entt::null && reg.valid(selected_) && reg.all_of<MeshInstance>(selected_))
            target = selected_;
    } else {
        target = EntityUnderPixel(scene, renderer, mx, my);
    }
    if (target == entt::null || !reg.valid(target) || !reg.all_of<MeshInstance>(target)) {
        if (!lmbDown) paintStroking_ = false;
        paintHasLast_ = false;
        return;
    }
    const MeshData* mesh = GetCpuMesh(scene, target);
    if (!mesh) {
        if (!lmbDown) paintStroking_ = false;
        paintHasLast_ = false;
        return;
    }
    PaintComponent* pc = reg.try_get<PaintComponent>(target);
    if (pc) {
        // Existing canvas: respect lock / hidden / layer masks.
        const bool layerMasked = paintActiveLayer_ != "All" && pc->layer != paintActiveLayer_;
        if (pc->locked || !pc->enabled || layerMasked) {
            if (!lmbDown) paintStroking_ = false;
            paintHasLast_ = false;
            return;
        }
    } else if (!paintAutoCreate_) {
        // No canvas yet and auto-create disabled: nothing to paint here.
        if (!lmbDown) paintStroking_ = false;
        paintHasLast_ = false;
        return;
    }

    // World ray through the cursor -> entity-local space.
    const Camera& cam = renderer.GetCamera();
    const glm::mat4 invVP = glm::inverse(cam.ViewProjection());
    const glm::vec2 ndc(mx * 2.0f - 1.0f, 1.0f - my * 2.0f);
    glm::vec4 pn = invVP * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 pf = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    pn /= pn.w;
    pf /= pf.w;
    const glm::vec3 ro(pn), rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
    const glm::mat4 world = scene.WorldMatrix(target);
    const glm::mat4 invWorld = glm::inverse(world);
    const glm::vec3 lo = glm::vec3(invWorld * glm::vec4(ro, 1.0f));
    const glm::vec3 ld = glm::vec3(invWorld * glm::vec4(rd, 0.0f));

    paint::PaintHit hit;
    if (!paint::RaycastMesh(*mesh, lo, ld, hit)) {
        if (!lmbDown) paintStroking_ = false;
        paintHasLast_ = false; // off-surface gap: don't streak across it
        return;
    }

    // Brush ring overlay: a camera-facing circle at the hit point.
    {
        const glm::vec3 hitW = glm::vec3(world * glm::vec4(hit.localPos, 1.0f));
        const glm::vec3 fwd = cam.Forward();
        glm::vec3 right = glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::dot(right, right) < 1e-5f) right = glm::vec3(1.0f, 0.0f, 0.0f);
        right = glm::normalize(right);
        const glm::vec3 up = glm::normalize(glm::cross(right, fwd));
        const glm::mat4 vp = cam.ViewProjection();
        ImVec2 pts[33];
        bool ok = true;
        for (int i = 0; i <= 32 && ok; ++i) {
            const f32 a = static_cast<f32>(i) / 32.0f * 6.2831853f;
            const glm::vec3 wp =
                hitW + (right * std::cos(a) + up * std::sin(a)) * brushRadius_;
            const glm::vec4 clip = vp * glm::vec4(wp, 1.0f);
            if (clip.w <= 0.001f) { ok = false; break; }
            const glm::vec2 nd = glm::vec2(clip) / clip.w;
            pts[i] = ImVec2(vpX_ + (nd.x * 0.5f + 0.5f) * vpW_,
                            vpY_ + (0.5f - nd.y * 0.5f) * vpH_);
        }
        if (ok) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->PushClipRect(ImVec2(vpX_, vpY_), ImVec2(vpX_ + vpW_, vpY_ + vpH_), true);
            const ImU32 ring = paintErase_
                                   ? IM_COL32(240, 240, 240, 235)
                                   : ImGui::GetColorU32(ImVec4(brushColor_.r, brushColor_.g,
                                                               brushColor_.b, 0.95f));
            dl->AddPolyline(pts, 33, ring, 0, 2.0f);
            dl->PopClipRect();
        }
    }

    // Stamp while LMB is held; freeze auto-orbit so the surface stays put.
    if (lmbDown) {
        if (renderer.IsOrbitEnabled()) {
            SyncFreecam(renderer);
            renderer.SetOrbitEnabled(false);
        }
        if (!pc) { // every mesh paintable: make a canvas on the first dab
            MakePaintable(scene, renderer, target);
            pc = reg.try_get<PaintComponent>(target);
            if (!pc) { paintStroking_ = false; return; }
        }
        if (!paintStroking_) {
            paintStroking_ = true;
            paintHasLast_ = false;
            paintSyncTick_ = 0;
            paintTarget_ = target;
            selected_ = target; // painting selects the object (selection feedback)
            // Begin recording this stroke into the database (committed on release).
            curStroke_ = paint::Stroke{};
            curStroke_.type = paint::StrokeType::Path;
            curStroke_.layer = pc->activeLayer;
            curStroke_.projection = pc->projection; // mesh-uv / box / 3D projection
            curStroke_.brush = brushDef_;
            curStroke_.color = brushColor_;
            curStroke_.metallic = brushMetallic_;
            curStroke_.roughness = brushRoughness_;
            curStroke_.height = brushHeight_ * 0.08f; // impasto buildup per dab
            curStroke_.flow = brushFlow_;
            curStroke_.colorVar = paintErase_ ? 0.0f : brushColorVar_;
            curStroke_.paintColor = brushPaintColor_;
            curStroke_.paintMaterial = brushPaintMaterial_;
            curStroke_.erase = paintErase_;
            curStrokeActive_ = true;
        }
        if (brushDirty_ || !brushTip_.Valid()) {
            brushTip_ = paint::MakeBrushTip(brushDef_);
            brushDirty_ = false;
        }
        // Paint coordinate + brush radius depend on the projection: mesh UV (raw
        // hit UV) or box projection (world-scaled, no stretch). The box canvas UV
        // and radius come from the same params the shader uses.
        glm::vec2 puv = hit.uv;
        f32 uvRadius = brushRadius_ * hit.uvPerWorld;
        if (pc->projection == 1) {
            glm::vec3 wmin(0.0f), wmax(0.0f);
            if (const AABB* box = reg.try_get<AABB>(target)) { wmin = box->min; wmax = box->max; }
            const glm::vec3 ws(glm::length(glm::vec3(world[0])),
                               glm::length(glm::vec3(world[1])),
                               glm::length(glm::vec3(world[2])));
            const paint::BoxParams bp = paint::ComputeBoxParams(wmin, wmax, ws);
            puv = paint::BoxProjectUV(hit.localPos, hit.localNormal, bp);
            uvRadius = brushRadius_ * bp.invM * 0.25f; // canvas-per-world * radius
        }
        // The dab the brush lays down: albedo + PBR material + relief. Per-dab colour
        // pooling is done DETERMINISTICALLY inside paint::Stamp / StampProjected
        // (hashed from the dab position) so a rebake from the stroke DB matches.
        paint::Dab dab;
        dab.color = brushColor_;
        dab.metallic = brushMetallic_;
        dab.roughness = brushRoughness_;
        dab.height = brushHeight_ * 0.08f; // impasto buildup per dab (matches stroke)
        dab.flow = brushFlow_;
        dab.paintColor = brushPaintColor_;
        dab.paintMaterial = brushPaintMaterial_;
        dab.erase = paintErase_;
        dab.mode = brushDef_.mode;                          // Smudge / Edge Darken
        dab.colorVar = paintErase_ ? 0.0f : brushColorVar_; // painterly colour pooling
        const int activeLayer = pc->activeLayer;

        if (pc->projection == 2) {
            // 3D PROJECTION paint: stamp by surface proximity (crosses UV-island
            // seams, never stretches). Interpolate dab centres in mesh-local space.
            const glm::vec3 ws(glm::length(glm::vec3(world[0])),
                               glm::length(glm::vec3(world[1])),
                               glm::length(glm::vec3(world[2])));
            const f32 avgScale = glm::max((ws.x + ws.y + ws.z) / 3.0f, 1e-6f);
            const f32 localRadius = brushRadius_ / avgScale; // world radius -> local units
            const glm::vec3 lc = hit.localPos;
            const glm::vec3 ln = hit.localNormal;
            const f32 angle = paintHasLast_ ? paint::SurfaceAngle(ln, lc - paintLastLocal_) : 0.0f;
            const auto stamp3D = [&](const glm::vec3& c, const glm::vec3& nrm, f32 rad) {
                paint::StampProjected(*pc, activeLayer, *mesh, c, nrm, rad, brushTip_, angle, dab);
            };
            if (paintHasLast_) {
                const glm::vec3 d = lc - paintLastLocal_;
                const f32 dist = glm::length(d);
                const f32 spacing = glm::max(localRadius * brushDef_.spacing, 1e-5f);
                const int steps = glm::min(static_cast<int>(dist / spacing), 512);
                for (int s = 1; s <= steps; ++s) {
                    const f32 t = static_cast<f32>(s) / static_cast<f32>(steps + 1);
                    stamp3D(glm::mix(paintLastLocal_, lc, t),
                            glm::mix(paintLastNormal_, ln, t),
                            glm::mix(paintLastLocalRadius_, localRadius, t));
                }
                stamp3D(lc, ln, localRadius);
            } else {
                stamp3D(lc, ln, localRadius);
            }
            paintLastLocal_ = lc;
            paintLastNormal_ = ln;
            paintLastLocalRadius_ = localRadius;
            paintLastUV_ = puv; // keep UV bookkeeping coherent for the ring/preview
            paintHasLast_ = true;
            if (curStrokeActive_) {
                paint::StrokePoint sp{puv, uvRadius, 1.0f};
                sp.localPos = lc;
                sp.localNormal = ln;
                sp.localRadius = localRadius;
                curStroke_.path.push_back(sp);
            }
        } else {
            // Orient directional tips (flat/bristle) along the stroke direction.
            f32 angle = 0.0f;
            if (paintHasLast_) {
                const glm::vec2 d0 = puv - paintLastUV_;
                if (glm::dot(d0, d0) > 1e-10f) angle = std::atan2(d0.y, d0.x);
            }
            const auto stampAt = [&](const glm::vec2& uv) {
                paint::Stamp(*pc, activeLayer, uv, uvRadius, brushTip_, angle, dab);
            };
            // Continuous stroke: fill the gap from the last stamp with evenly-spaced
            // stamps (spacing < radius) so a drag paints one smooth line, not circles.
            if (paintHasLast_) {
                const glm::vec2 d = puv - paintLastUV_;
                const f32 dist = glm::length(d);
                if (dist > 0.25f) {
                    stampAt(puv); // big jump (cell/island boundary or fast move): no streak
                } else {
                    // Dab spacing (per brush) -> overlapping dabs blend into a stroke.
                    const f32 texel = 1.0f / glm::max(static_cast<f32>(pc->resolution), 1.0f);
                    const f32 spacing = glm::max(uvRadius * brushDef_.spacing, 0.5f * texel);
                    const int steps = glm::min(static_cast<int>(dist / spacing), 512);
                    for (int s = 1; s <= steps; ++s)
                        stampAt(paintLastUV_ + d * (static_cast<f32>(s) / static_cast<f32>(steps + 1)));
                    stampAt(puv);
                }
            } else {
                stampAt(puv);
            }
            paintLastUV_ = puv;
            paintHasLast_ = true;
            // Record the path point into the in-flight stroke (the source of truth).
            // The live stamps above paint incrementally for responsiveness; this
            // stroke is what undo/redo rebake from, so the two stay in lock-step.
            if (curStrokeActive_)
                curStroke_.path.push_back(paint::StrokePoint{puv, uvRadius, 1.0f});
        }
        // Throttle the GPU re-upload during a stroke - a full flatten + mip-gen +
        // synchronous upload every frame (x layers) stalls the GPU and makes
        // painting janky. Sync every few frames (no edge dilation mid-stroke - the
        // release flush does that once); the final dab flushes on release.
        if ((paintSyncTick_++ % 3) == 0) paint::Sync(renderer, *pc, false);
        paintConsumedClick_ = true; // don't also pick an entity
    } else {
        if (paintStroking_ && pc && pc->dirty) paint::Sync(renderer, *pc); // flush last dabs
        paintStroking_ = false;
        paintHasLast_ = false;
    }
}

void Editor::DrawProjectSettings(Engine& engine) {
    if (!panelOpen_[Panel_ProjectSettings]) return;
    if (!ImGui::Begin("Project Settings", &panelOpen_[Panel_ProjectSettings])) {
        ImGui::End();
        return;
    }
    if (!Project::HasActive()) {
        ImGui::TextDisabled("Open a project to edit its environment.");
        ImGui::End();
        return;
    }

    ProjectSettings& ps = Project::Active().Settings();
    EnvironmentSettings& env = ps.environment;
    // Regenerating the sky + IBL is a few-second CPU job (Debug), so it runs ONLY
    // on the explicit button - never live while dragging a colour.
    bool rebuild = false;

    ImGui::TextDisabled("Project: %s", ps.name.c_str());
    ImGui::Spacing();

    // Day/Night + Weather are project-global (applied to the scene by
    // SetupEnvironment / stamped each frame), so they must persist to the .hbproj.
    // Auto-save when the user finishes an edit (no item active) - matches the AA/GTAO
    // controls below, and means the day/night state ships in a build without needing
    // a manual "Save Project" (the bug where dynamic sky worked live but not built).
    static bool s_skyDirty = false;
    if (ImGui::CollapsingHeader("Day / Night", ImGuiTreeNodeFlags_DefaultOpen)) {
        SceneEnvironment& se = engine.GetScene().Environment();
        ImGui::TextDisabled("Real-time analytic sky; the sun is driven by the time of day.");
        bool dyn = env.dynamicSky != 0;
        if (ImGui::Checkbox("Dynamic sky (day/night cycle)", &dyn)) {
            env.dynamicSky = dyn ? 1u : 0u;
            se.dynamicSky = env.dynamicSky; // live
            s_skyDirty = true;
        }
        ImGui::BeginDisabled(!dyn);
        if (ImGui::SliderFloat("Time of Day", &env.timeOfDay, 0.0f, 24.0f, "%.1f h")) {
            se.timeOfDay = env.timeOfDay; // live scrub
            se.dynamicSky = env.dynamicSky;
            s_skyDirty = true;
        }
        if (ImGui::DragFloat("Day Length (sec)", &env.dayLengthSeconds, 1.0f, 0.0f, 3600.0f,
                             "%.0f")) {
            se.dayLengthSeconds = env.dayLengthSeconds;
            s_skyDirty = true;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("0 day length = time held; scrub it above. No re-bake needed.");
    }

    if (ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen)) {
        SceneEnvironment& se = engine.GetScene().Environment();
        ImGui::TextDisabled("Procedural cloud layer, sun-lit, drifting over time.");
        if (ImGui::SliderFloat("Cloud Cover", &env.cloudCoverage, 0.0f, 1.0f, "%.2f")) {
            se.cloudCoverage = env.cloudCoverage;
            s_skyDirty = true;
        }
        if (ImGui::SliderFloat("Cloud Density", &env.cloudDensity, 0.0f, 1.0f, "%.2f")) {
            se.cloudDensity = env.cloudDensity;
            s_skyDirty = true;
        }
        if (ImGui::SliderFloat("Overcast", &env.overcast, 0.0f, 1.0f, "%.2f")) {
            se.overcast = env.overcast;
            s_skyDirty = true;
        }
        if (ImGui::SliderFloat("Wind Direction", &env.windAngle, 0.0f, 360.0f, "%.0f deg")) {
            se.windAngle = env.windAngle;
            s_skyDirty = true;
        }
        if (ImGui::SliderFloat("Wind Speed", &env.windSpeed, 0.0f, 0.2f, "%.3f")) {
            se.windSpeed = env.windSpeed;
            s_skyDirty = true;
        }
    }
    if (s_skyDirty && !ImGui::IsAnyItemActive()) {
        Project::Active().Save();
        s_skyDirty = false;
    }

    if (ImGui::CollapsingHeader("Skybox", ImGuiTreeNodeFlags_DefaultOpen)) {
        SkySettings& s = env.sky;
        ImGui::TextDisabled("Static sun colours/intensity (used when Dynamic sky is off).");
        ImGui::ColorEdit3("Horizon", glm::value_ptr(s.horizonColor));
        ImGui::ColorEdit3("Zenith", glm::value_ptr(s.zenithColor));
        ImGui::ColorEdit3("Ground", glm::value_ptr(s.groundColor));
        ImGui::DragFloat3("Sun Direction", glm::value_ptr(s.sunDirection), 0.01f, -1.0f, 1.0f);
        ImGui::ColorEdit3("Sun Tint", glm::value_ptr(s.sunTint));
        ImGui::DragFloat("Sun Intensity", &s.sunIntensity, 0.5f, 0.0f, 500.0f);
        ImGui::DragFloat("Sky Brightness", &s.skyIntensity, 0.01f, 0.0f, 8.0f);
        if (ImGui::Button("Rebuild Sky + Lighting")) rebuild = true;
        ImGui::SameLine();
        ImGui::TextDisabled("(regenerates IBL, ~secs)");
    }

    if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Fallback sun (a scene's Directional Light wins).");
        ImGui::ColorEdit3("Sun Color", glm::value_ptr(env.sunColor));
        ImGui::DragFloat("Sun Light Intensity", &env.sunLightIntensity, 0.05f, 0.0f, 50.0f);
        ImGui::DragFloat("Ambient (IBL)", &env.ambientIntensity, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat("Exposure", &env.exposure, 0.01f, 0.05f, 16.0f);
        if (ImGui::Button("Apply to Scene")) {
            // Push lighting/exposure into the live scene environment now.
            SceneEnvironment& se = engine.GetScene().Environment();
            se.ambientIntensity = env.ambientIntensity;
            se.exposure = env.exposure;
            se.sun.direction = glm::normalize(-env.sky.sunDirection);
            se.sun.color = env.sunColor;
            se.sun.intensity = env.sunLightIntensity;
        }
    }

    if (ImGui::CollapsingHeader("Rendering Quality (AA / GTAO)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Project-wide quality for the whole game; Save Project writes");
        ImGui::TextDisabled("the .hbproj. The post 'look' (bloom/fog/DoF/grade/exposure)");
        ImGui::TextDisabled("is per-scene and per Post Volume - see the Post Process panel.");
        // Auto-persist: AA/GTAO are project-global and stamped onto the scene every
        // frame, so they must live in the .hbproj. Save when the user finishes an
        // edit (no item active) so it sticks across boots without a manual button.
        static bool s_qualityDirty = false;
        if (DrawPostQualityControls(env.post)) s_qualityDirty = true;
        if (s_qualityDirty && !ImGui::IsAnyItemActive()) {
            Project::Active().Save();
            s_qualityDirty = false;
        }
        // The engine stamps these AA/GTAO fields onto the live scene every frame
        // (Engine loop), so the choice is authoritative regardless of scene/volume.
    }

    ImGui::Separator();
    if (ImGui::Button("Save Project")) {
        Project::Active().Save();
        buildResult_ = "Project settings saved (.hbproj).";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Stored in the .hbproj");

    // Regenerate the sky + image-based lighting from the edited parameters
    // (button-driven; reads the project's environment we just edited).
    if (rebuild) {
        scene::SetupEnvironment(engine.GetScene(), engine.GetRenderer());
        buildResult_ = "Rebuilt sky + IBL from project settings.";
    }

    ImGui::End();
}

void Editor::RefreshAssets() {
    assets_.clear();
    assetsScanned_ = true;
    if (!Project::HasActive()) return;
    const std::filesystem::path root = Project::Active().AssetsDir();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return;

    // Clamp the browsing directory inside Assets/ (it may have been deleted,
    // or belong to a previously opened project).
    if (currentDir_.empty() || !std::filesystem::exists(currentDir_, ec)) {
        currentDir_ = root;
    } else {
        const auto rel = std::filesystem::relative(currentDir_, root, ec);
        if (ec || rel.empty() || rel.native().starts_with(L"..")) currentDir_ = root;
    }

    for (const auto& entry : std::filesystem::directory_iterator(currentDir_, ec)) {
        AssetItem item;
        item.path = entry.path();
        if (entry.is_directory()) {
            item.isFolder = true;
            item.label = entry.path().filename().string();
            item.typeName = "Folder";
        } else if (entry.is_regular_file() && entry.path().extension() == ".hbscene") {
            item.isScene = true;
            item.label = entry.path().stem().string();
            item.typeName = "Scene";
        } else if (entry.is_regular_file() && entry.path().extension() == ".hbmat") {
            item.isMaterial = true;
            item.label = entry.path().stem().string();
            item.typeName = "Material";
        } else if (entry.is_regular_file() && entry.path().extension() == ".hbevent") {
            item.isAudioEvent = true;
            item.label = entry.path().stem().string();
            item.typeName = "Audio Event";
        } else if (entry.is_regular_file() && entry.path().extension() == ".hbschem") {
            item.isSchematic = true;
            item.label = entry.path().stem().string();
            item.typeName = "Schematic";
        } else if (entry.is_regular_file() && entry.path().extension() == ".hbprefab") {
            item.isPrefab = true;
            item.label = entry.path().stem().string();
            item.typeName = "Prefab";
        } else if (entry.is_regular_file() && entry.path().extension() == ".uaf") {
            const uaf::AssetType type = uaf::PeekType(entry.path()); // header only
            item.isMesh = (type == uaf::AssetType::Mesh);
            item.isAudio = (type == uaf::AssetType::Audio);
            item.isTexture = (type == uaf::AssetType::Texture);
            item.isFont = (type == uaf::AssetType::Font);
            item.label = entry.path().stem().string();
            item.typeName = uaf::ToString(type);
        } else {
            continue;
        }
        assets_.push_back(std::move(item));
    }

    // Folders first, then by type (meshes are the spawnable ones), then name.
    std::sort(assets_.begin(), assets_.end(), [](const AssetItem& a, const AssetItem& b) {
        auto rank = [](const AssetItem& it) {
            return it.isFolder    ? -2
                   : it.isScene   ? -1
                   : it.isPrefab  ? 0
                   : it.isSchematic ? 0
                   : it.isMesh    ? 1
                   : it.isMaterial ? 2
                   : it.isAudioEvent ? 3
                   : it.isAudio   ? 4
                   : it.isTexture ? 5
                                  : 6;
        };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a.label < b.label;
    });
}

void Editor::LoadThumbnail(Renderer& renderer, AssetItem& item) {
    item.thumbTried = true;
    const std::string key = item.path.string();
    if (auto it = thumbCache_.find(key); it != thumbCache_.end()) {
        item.thumbId = it->second;
        return;
    }

    // Meshes: software-rasterize a small three-quarter-view preview.
    if (item.isMesh) {
        const std::optional<Model> model = assets::LoadMesh(item.path);
        if (!model) return;
        constexpr u32 kDim = 96;
        const std::vector<u32> px = editor::RasterizeMeshThumbnail(*model, kDim);
        rhi::TextureDesc d;
        d.width = kDim;
        d.height = kDim;
        d.format = rhi::Format::R8G8B8A8_UNORM;
        d.pixels = px.data();
        d.debugName = "mesh_thumbnail";
        const rhi::TextureHandle handle = renderer.UploadTexture(d);
        item.thumbId = renderer.TextureUIId(handle);
        thumbCache_[key] = item.thumbId;
        return;
    }

    const std::optional<uaf::Texture> tex = uaf::ReadTexture(item.path);
    if (!tex || tex->width == 0 || tex->height == 0) return;
    const auto fmt = static_cast<rhi::Format>(tex->format);
    const bool fourBytes = fmt == rhi::Format::R8G8B8A8_UNORM ||
                           fmt == rhi::Format::R8G8B8A8_SRGB ||
                           fmt == rhi::Format::B8G8R8A8_UNORM ||
                           fmt == rhi::Format::B8G8R8A8_SRGB;
    const usize mip0Size = static_cast<usize>(tex->width) * tex->height * 4;
    if (!fourBytes || tex->pixels.size() < mip0Size) return;

    // Nearest-sample mip 0 down to thumbnail resolution.
    constexpr u32 kMaxDim = 96;
    u32 dw = tex->width, dh = tex->height;
    if (dw > kMaxDim || dh > kMaxDim) {
        const f32 scale = static_cast<f32>(glm::max(dw, dh)) / kMaxDim;
        dw = glm::max(1u, static_cast<u32>(dw / scale));
        dh = glm::max(1u, static_cast<u32>(dh / scale));
    }
    std::vector<u32> px(static_cast<usize>(dw) * dh);
    const u32* src = reinterpret_cast<const u32*>(tex->pixels.data());
    for (u32 y = 0; y < dh; ++y) {
        const u32 sy = y * tex->height / dh;
        for (u32 x = 0; x < dw; ++x) {
            const u32 sx = x * tex->width / dw;
            px[y * dw + x] = src[static_cast<usize>(sy) * tex->width + sx];
        }
    }

    rhi::TextureDesc d;
    d.width = dw;
    d.height = dh;
    // ImGui doesn't gamma-encode on output, so upload sRGB sources as UNORM
    // (pass-through bytes) or the thumbnail displays too dark.
    d.format = (fmt == rhi::Format::B8G8R8A8_UNORM || fmt == rhi::Format::B8G8R8A8_SRGB)
                   ? rhi::Format::B8G8R8A8_UNORM
                   : rhi::Format::R8G8B8A8_UNORM;
    d.pixels = px.data();
    d.debugName = "asset_thumbnail";
    const rhi::TextureHandle handle = renderer.UploadTexture(d);
    item.thumbId = renderer.TextureUIId(handle);
    thumbCache_[key] = item.thumbId;
}

void Editor::DrawAssetTile(Engine& engine, AssetItem& item) {
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();

    const f32 tile = assetTileSize_;
    const f32 labelH = ImGui::GetTextLineHeight() + 8.0f;
    const ImVec2 cell(tile + 12.0f, tile + labelH);

    ImGui::PushID(item.path.string().c_str());
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tile", cell);
    const bool hovered = ImGui::IsItemHovered();
    const bool doubleClicked =
        hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cellMax(origin.x + cell.x, origin.y + cell.y);

    // Drag any tile (assets move into folders; folders can be nested).
    if (ImGui::BeginDragDropSource()) {
        const std::string src = item.path.string();
        ImGui::SetDragDropPayload("HBE_ASSET_PATH", src.c_str(), src.size() + 1);
        ImGui::TextUnformatted(item.label.c_str());
        ImGui::EndDragDropSource();
    }
    // Folders accept dropped tiles: move the file/directory inside.
    if (item.isFolder && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HBE_ASSET_PATH")) {
            const std::filesystem::path src(static_cast<const char*>(p->Data));
            std::error_code ec;
            if (src != item.path) {
                std::filesystem::rename(src, item.path / src.filename(), ec);
                if (!ec) assetsDirty_ = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Tile background + accent strip under the icon area.
    const ImU32 bg = hovered ? IM_COL32(64, 64, 74, 255) : IM_COL32(47, 47, 54, 255);
    dl->AddRectFilled(origin, cellMax, bg, 6.0f);
    const ImU32 accent = item.isFolder     ? IM_COL32(235, 190, 95, 255)
                         : item.isScene    ? IM_COL32(170, 130, 230, 255)
                         : item.isMaterial ? IM_COL32(95, 175, 155, 255)
                         : item.isSchematic ? IM_COL32(120, 170, 235, 255)
                         : item.isPrefab   ? IM_COL32(230, 160, 95, 255)
                         : item.isMesh     ? kMeshAccent
                         : item.isTexture  ? kTextureAccent
                         : item.isAudioEvent ? IM_COL32(240, 150, 90, 255)
                         : item.isAudio    ? kAudioAccent
                                           : kOtherAccent;

    // Icon area: a centered square at the top of the tile.
    const f32 iconSz = tile - 14.0f;
    const ImVec2 iconMin(origin.x + (cell.x - iconSz) * 0.5f, origin.y + 7.0f);
    const ImVec2 iconMax(iconMin.x + iconSz, iconMin.y + iconSz);
    const ImVec2 iconCenter((iconMin.x + iconMax.x) * 0.5f, (iconMin.y + iconMax.y) * 0.5f);

    if (item.isFolder) {
        DrawFolderIcon(dl, iconCenter, iconSz * 0.42f);
    } else if (item.isScene) {
        DrawSceneIcon(dl, iconCenter, iconSz * 0.42f);
    } else if (item.isMaterial) {
        DrawMaterialIcon(dl, iconCenter, iconSz * 0.40f);
    } else if (item.isSchematic) {
        // Mini node-graph glyph: two wired boxes.
        const f32 r = iconSz * 0.40f;
        const ImU32 c = IM_COL32(120, 170, 235, 255);
        const ImVec2 a0(iconCenter.x - r, iconCenter.y - r * 0.55f);
        const ImVec2 a1(iconCenter.x - r * 0.25f, iconCenter.y + r * 0.05f);
        const ImVec2 b0(iconCenter.x + r * 0.25f, iconCenter.y - r * 0.05f);
        const ImVec2 b1(iconCenter.x + r, iconCenter.y + r * 0.55f);
        dl->AddLine(ImVec2(a1.x, (a0.y + a1.y) * 0.5f), ImVec2(b0.x, (b0.y + b1.y) * 0.5f), c, 2.0f);
        dl->AddRectFilled(a0, a1, IM_COL32(60, 70, 95, 255), 3.0f);
        dl->AddRect(a0, a1, c, 3.0f, 0, 1.5f);
        dl->AddRectFilled(b0, b1, IM_COL32(60, 70, 95, 255), 3.0f);
        dl->AddRect(b0, b1, c, 3.0f, 0, 1.5f);
        dl->AddCircleFilled(ImVec2(a1.x, (a0.y + a1.y) * 0.5f), 2.0f, c);
        dl->AddCircleFilled(ImVec2(b0.x, (b0.y + b1.y) * 0.5f), 2.0f, c);
    } else if (item.isPrefab) {
        // A box "package" glyph (a cube with a corner tab).
        const f32 r = iconSz * 0.36f;
        const ImU32 c = IM_COL32(230, 160, 95, 255);
        dl->AddRectFilled(ImVec2(iconCenter.x - r, iconCenter.y - r),
                          ImVec2(iconCenter.x + r, iconCenter.y + r), IM_COL32(70, 55, 38, 255),
                          3.0f);
        dl->AddRect(ImVec2(iconCenter.x - r, iconCenter.y - r),
                    ImVec2(iconCenter.x + r, iconCenter.y + r), c, 3.0f, 0, 2.0f);
        dl->AddLine(ImVec2(iconCenter.x - r, iconCenter.y), ImVec2(iconCenter.x + r, iconCenter.y),
                    c, 1.5f);
        dl->AddLine(ImVec2(iconCenter.x, iconCenter.y - r), ImVec2(iconCenter.x, iconCenter.y), c,
                    1.5f);
    } else if (item.isFont) {
        // "Aa" glyph card.
        dl->AddRectFilled(ImVec2(iconCenter.x - iconSz * 0.40f, iconCenter.y - iconSz * 0.40f),
                          ImVec2(iconCenter.x + iconSz * 0.40f, iconCenter.y + iconSz * 0.40f),
                          IM_COL32(60, 62, 80, 255), 6.0f);
        const ImVec2 ts = ImGui::CalcTextSize("Aa");
        dl->AddText(ImVec2(iconCenter.x - ts.x * 0.5f, iconCenter.y - ts.y * 0.5f),
                    IM_COL32(225, 225, 235, 255), "Aa");
    } else if ((item.isTexture || item.isMesh) && item.thumbId != 0) {
        dl->AddImageRounded(static_cast<ImTextureID>(item.thumbId), iconMin, iconMax,
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4.0f);
    } else if (item.isMesh) {
        DrawMeshIcon(dl, iconCenter, iconSz * 0.42f);
    } else if (item.isAudioEvent) {
        // Speaker + an "event" spark to set it apart from raw audio clips.
        DrawAudioIcon(dl, iconCenter, iconSz * 0.34f);
        const ImVec2 spark(iconCenter.x + iconSz * 0.28f, iconCenter.y - iconSz * 0.28f);
        dl->AddCircleFilled(spark, iconSz * 0.10f, IM_COL32(240, 150, 90, 255));
    } else if (item.isAudio) {
        DrawAudioIcon(dl, iconCenter, iconSz * 0.38f);
    } else if (item.isTexture) {
        DrawTextureIcon(dl, iconCenter, iconSz * 0.42f);
    } else {
        const ImVec2 q = ImGui::CalcTextSize("?");
        dl->AddText(ImVec2(iconCenter.x - q.x * 0.5f, iconCenter.y - q.y * 0.5f),
                    kOtherAccent, "?");
    }
    dl->AddRectFilled(ImVec2(iconMin.x, iconMax.y + 3.0f),
                      ImVec2(iconMax.x, iconMax.y + 5.0f), accent, 1.0f);

    // Label: centered, clipped to the tile.
    dl->PushClipRect(ImVec2(origin.x + 3, origin.y), ImVec2(cellMax.x - 3, cellMax.y), true);
    const ImVec2 ts = ImGui::CalcTextSize(item.label.c_str());
    const f32 tx = ts.x < cell.x - 8.0f ? origin.x + (cell.x - ts.x) * 0.5f : origin.x + 4.0f;
    dl->AddText(ImVec2(tx, iconMax.y + 9.0f), IM_COL32(222, 222, 228, 255),
                item.label.c_str());
    dl->PopClipRect();

    if (hovered) {
        ImGui::SetTooltip("%s  [%s]%s", item.path.filename().string().c_str(),
                          item.typeName.c_str(),
                          item.isFolder     ? "\nDouble-click to open"
                          : item.isScene    ? "\nDouble-click to load"
                          : item.isMaterial ? "\nClick to edit in the Asset Viewer; drag onto an object to apply"
                          : item.isSchematic ? "\nDouble-click to edit in the Schematic Editor"
                          : item.isPrefab   ? "\nDouble-click to instantiate (or drag into the viewport)"
                          : item.isMesh     ? "\nDouble-click to spawn (or drag into the viewport)"
                          : item.isAudioEvent ? "\nDouble-click to post (play through the mixer)"
                          : item.isAudio    ? "\nDouble-click to play"
                                            : "");
    }
    // A plain click selects the asset into the Asset Viewer panel.
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !item.isFolder) {
        SelectAsset(item.path);
    }
    if (doubleClicked) {
        if (item.isFolder) navTarget_ = item.path;
        else if (item.isScene) LoadSceneInEditor(engine, item.path);
        else if (item.isSchematic) OpenSchematic(item.path);
        else if (item.isPrefab) InstantiatePrefab(engine, item.path);
        else if (item.isMesh) SpawnMeshAsset(scene, renderer, item.path);
        else if (item.isAudio) engine.GetAudio().PlayUAF(item.path);
        else if (item.isAudioEvent) {
            if (const auto ev = assets::LoadAudioEvent(item.path)) {
                engine.GetAudio().PostEvent(*ev, Project::Active().AssetsDir());
            }
        }
    }
    if (ImGui::BeginPopupContextItem("##assetctx")) {
        if (item.isFolder && ImGui::MenuItem("Open")) navTarget_ = item.path;
        if (item.isScene) {
            if (ImGui::MenuItem("Load (replace)")) LoadSceneInEditor(engine, item.path);
            if (ImGui::MenuItem("Stream in (additive)")) {
                streamer_.BeginLoad(item.path, Project::Active().AssetsDir(),
                                    scene::LoadMode::Additive);
            }
        }
        if (item.isMesh && ImGui::MenuItem("Spawn in scene")) {
            SpawnMeshAsset(scene, renderer, item.path);
        }
        if (item.isMaterial) {
            const bool hasSel = selected_ != entt::null &&
                                scene.Registry().valid(selected_) &&
                                scene.Registry().all_of<MeshInstance>(selected_);
            if (ImGui::MenuItem("Apply to selected entity", nullptr, false, hasSel)) {
                ApplyMaterialToEntity(engine, selected_, item.path);
            }
        }
        if (item.isSchematic && ImGui::MenuItem("Edit")) {
            OpenSchematic(item.path);
        }
        if (item.isPrefab && ImGui::MenuItem("Instantiate")) {
            InstantiatePrefab(engine, item.path);
        }
        if (item.isAudio && ImGui::MenuItem("Play")) {
            engine.GetAudio().PlayUAF(item.path);
        }
        if (item.isAudioEvent && ImGui::MenuItem("Post event")) {
            if (const auto ev = assets::LoadAudioEvent(item.path)) {
                engine.GetAudio().PostEvent(*ev, Project::Active().AssetsDir());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
            renameAsset_ = item.path;
            // Seed with the current name: stem for files, whole name for folders.
            const std::string seed =
                item.isFolder ? item.path.filename().string() : item.path.stem().string();
            std::snprintf(renameAssetBuf_, sizeof(renameAssetBuf_), "%s", seed.c_str());
        }
        if (ImGui::MenuItem("Copy path")) {
            ImGui::SetClipboardText(item.path.string().c_str());
        }
        if (ImGui::MenuItem("Delete")) {
            std::error_code ec;
            if (item.isFolder) {
                std::filesystem::remove(item.path, ec); // empty folders only
            } else {
                std::filesystem::remove(item.path, ec);
            }
            if (!ec) {
                assetsDirty_ = true;
                scene::ClearInstantiateCaches();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void Editor::DrawAssetBrowser(Engine& engine) {
    Renderer& renderer = engine.GetRenderer();
    if (!panelOpen_[Panel_Assets]) return;
    ImGui::Begin("Assets", &panelOpen_[Panel_Assets]);
    if (!Project::HasActive()) {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }
    if (!assetsScanned_) RefreshAssets(); // scan once, then on demand (not per frame)

    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("##tilesize", &assetTileSize_, 64.0f, 160.0f, "Size %.0f");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Project::Active().Settings().name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(right-click for actions)");

    // Breadcrumbs: Assets / sub / folder (each segment navigates).
    {
        const std::filesystem::path root = Project::Active().AssetsDir();
        if (ImGui::SmallButton("Assets")) navTarget_ = root;
        std::error_code ec;
        const auto rel = std::filesystem::relative(
            currentDir_.empty() ? root : currentDir_, root, ec);
        std::filesystem::path walk = root;
        if (!ec && rel != ".") {
            for (const auto& seg : rel) {
                walk /= seg;
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextDisabled("/");
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::PushID(walk.string().c_str());
                if (ImGui::SmallButton(seg.string().c_str())) navTarget_ = walk;
                ImGui::PopID();
            }
        }
    }
    ImGui::Separator();

    // Decode at most one thumbnail per frame (keeps the UI responsive even
    // when the project has many large textures/meshes).
    for (AssetItem& item : assets_) {
        if ((item.isTexture || item.isMesh) && !item.thumbTried) {
            LoadThumbnail(renderer, item);
            break;
        }
    }

    // Icon grid (wraps to the panel width).
    ImGui::BeginChild("##assetgrid", ImVec2(0, 0), ImGuiChildFlags_None);
    const f32 cellW = assetTileSize_ + 12.0f;
    const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = glm::max(
        1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (cellW + spacing)));
    int i = 0;
    for (AssetItem& item : assets_) {
        if (i++ % columns != 0) ImGui::SameLine();
        DrawAssetTile(engine, item);
    }
    if (assets_.empty()) {
        ImGui::TextDisabled("Empty - right-click to import or create assets.");
    }

    // Browser actions live in the grid's right-click context menu (empty space
    // only: tiles have their own context menus).
    bool wantNewFolder = false;
    if (ImGui::BeginPopupContextWindow("##assetctx",
                                       ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
        const std::filesystem::path dst =
            currentDir_.empty() ? Project::Active().AssetsDir() : currentDir_;
        if (ImGui::MenuItem("Import...")) {
            wchar_t file[2048] = {};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter =
                L"Assets\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.hdr;*.gltf;*.glb;*.obj;*.fbx;*.wav;*.ttf;*.otf\0"
                L"All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = static_cast<DWORD>(std::size(file));
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                hbe::importer::Import(std::filesystem::path(file), dst);
                // A re-import may overwrite an asset the GPU caches as resident.
                scene::ClearInstantiateCaches();
                ui::ClearFontCache();
                assetsDirty_ = true;
            }
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Create")) {
            if (ImGui::MenuItem("Folder")) wantNewFolder = true;
            // Each opens a name prompt (see the NewAsset modal below); a script's
            // class is named after the file.
            const auto beginCreate = [&](int kind, const char* defName) {
                pendingCreateKind_ = kind;
                pendingCreateDir_ = dst;
                std::snprintf(newAssetNameBuf_, sizeof(newAssetNameBuf_), "%s", defName);
            };
            if (ImGui::MenuItem("Material")) beginCreate(1, "NewMaterial");
            if (ImGui::MenuItem("Audio Event")) beginCreate(3, "NewAudioEvent");
            if (ImGui::MenuItem("Schematic")) beginCreate(4, "NewSchematic");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Refresh")) assetsDirty_ = true;
        ImGui::EndPopup();
    }
    if (wantNewFolder) ImGui::OpenPopup("NewAssetFolder");
    if (ImGui::BeginPopup("NewAssetFolder")) {
        static char folderName[128] = "NewFolder";
        ImGui::SetNextItemWidth(180.0f);
        const bool enter = ImGui::InputText("##foldername", folderName, sizeof(folderName),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Create") || enter) {
            if (folderName[0] != '\0') {
                std::error_code ec;
                std::filesystem::create_directory(
                    (currentDir_.empty() ? Project::Active().AssetsDir() : currentDir_) /
                        folderName,
                    ec);
                if (!ec) assetsDirty_ = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    // New-asset name prompt (Create > Material/Audio Event/Schematic).
    if (pendingCreateKind_ != 0 && !ImGui::IsPopupOpen("New Asset")) {
        ImGui::OpenPopup("New Asset");
    }
    if (ImGui::BeginPopupModal("New Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* label = pendingCreateKind_ == 1   ? "Material name"
                            : pendingCreateKind_ == 4 ? "Schematic name"
                            : pendingCreateKind_ == 5 ? "Prefab name"
                                                      : "Audio event name";
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter =
            ImGui::InputText("##newassetname", newAssetNameBuf_, sizeof(newAssetNameBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        const bool ok = newAssetNameBuf_[0] != '\0';
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Create") || (enter && ok)) {
            std::filesystem::path created;
            switch (pendingCreateKind_) {
                case 1: created = CreateMaterialAsset(pendingCreateDir_, newAssetNameBuf_); break;
                case 3: created = CreateAudioEventAsset(pendingCreateDir_, newAssetNameBuf_); break;
                case 4: created = CreateSchematicAsset(pendingCreateDir_, newAssetNameBuf_); break;
                case 5:
                    created = CreatePrefabFromSelection(engine.GetScene(), pendingCreateDir_,
                                                        newAssetNameBuf_);
                    break;
                default: break;
            }
            if (!created.empty()) {
                assetsDirty_ = true;
                if (pendingCreateKind_ == 4) OpenSchematic(created); // jump to canvas
                else SelectAsset(created);
            }
            pendingCreateKind_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pendingCreateKind_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Rename modal (opened from a tile's context menu). Keeps the extension for
    // files; renames the whole name for folders.
    if (!renameAsset_.empty() && !ImGui::IsPopupOpen("Rename Asset")) {
        ImGui::OpenPopup("Rename Asset");
    }
    if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::error_code rec;
        const bool isDir = std::filesystem::is_directory(renameAsset_, rec);
        const std::string ext = isDir ? std::string() : renameAsset_.extension().string();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter =
            ImGui::InputText("##renameasset", renameAssetBuf_, sizeof(renameAssetBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (!ext.empty()) {
            ImGui::SameLine();
            ImGui::TextUnformatted(ext.c_str());
        }
        ImGui::TextDisabled("Note: references to this asset are not auto-updated.");
        const bool canRename = renameAssetBuf_[0] != '\0';
        ImGui::BeginDisabled(!canRename);
        if (ImGui::Button("Rename") || (enter && canRename)) {
            std::error_code ec;
            std::filesystem::path to = renameAsset_;
            to.replace_filename(std::string(renameAssetBuf_) + ext);
            if (to != renameAsset_ && !std::filesystem::exists(to, ec)) {
                std::filesystem::rename(renameAsset_, to, ec);
                if (!ec) {
                    if (currentScenePath_ == renameAsset_) currentScenePath_ = to;
                    scene::ClearInstantiateCaches();
                    scenesScanned_ = false;
                    assetsDirty_ = true;
                }
            }
            renameAsset_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            renameAsset_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Deferred mutations (the tile loop iterates assets_; don't refresh inside).
    if (!navTarget_.empty()) {
        currentDir_ = navTarget_;
        navTarget_.clear();
        RefreshAssets();
    } else if (assetsDirty_) {
        assetsDirty_ = false;
        RefreshAssets();
    }
    ImGui::End();
}

void Editor::LoadSceneInEditor(Engine& engine, const std::filesystem::path& path) {
    PushUndo(engine.GetScene()); // Ctrl+Z returns to the previous world
    selected_ = entt::null;
    engine.GetPhysics().SetEditedEntity(entt::null);
    // Drop the pathfinding debug overlay; GridNav rebuilds from the new scene.
    navBuilt_ = false;
    navCells_.clear();
    navPath_.clear();
    if (scene::LoadScene(engine.GetScene(), engine.GetRenderer(), path)) {
        currentScenePath_ = path;
        levelOpen_ = false; // a standalone scene, not a level
        currentLevel_ = {};
    }
}

void Editor::OpenLevel(Engine& engine, const scene::LevelPaths& level, bool additive) {
    Scene& scene = engine.GetScene();
    auto& reg = scene.Registry();
    PushUndo(scene);
    // Additive: don't reload a level that's already in the world - just make it
    // the active one (new objects join it).
    if (additive) {
        const std::string s = level.Member(SceneKind::Static).string();
        const std::string d = level.Member(SceneKind::Dynamic).string();
        for (const entt::entity e : reg.view<SceneSource>()) {
            const std::string& src = reg.get<SceneSource>(e).scene;
            if (src == s || src == d) {
                currentLevel_ = level;
                levelOpen_ = true;
                return;
            }
        }
    } else {
        selected_ = entt::null;
        engine.GetPhysics().SetEditedEntity(entt::null);
    }
    navBuilt_ = false; // pathfinding debug overlay; GridNav rebuilds from the new level
    navCells_.clear();
    navPath_.clear();
    if (scene::LoadLevel(scene, engine.GetRenderer(), level, nullptr, additive)) {
        currentLevel_ = level; // the active level (new objects join it)
        levelOpen_ = true;
        // A non-additive load replaced the world, so there's no active scene left;
        // an additive load keeps any active UI scene (and its save target) resident.
        if (!additive) currentScenePath_.clear();
    }
}

void Editor::CreateLevel(Engine& engine, const std::filesystem::path& base) {
    const scene::LevelPaths lp = scene::ResolveLevel(base);
    // A level is static + dynamic only (UI lives in standalone scenes). Write the
    // two empty layer files so the level exists on disk, then open it.
    Scene empty;
    for (const SceneKind k : {SceneKind::Static, SceneKind::Dynamic}) {
        scene::SaveScene(empty, lp.Member(k), {}, k);
    }
    scenesScanned_ = false;
    OpenLevel(engine, lp);
}

entt::entity Editor::RootOf(Scene& scene, entt::entity e) const {
    auto& reg = scene.Registry();
    entt::entity cur = e;
    for (int guard = 0; guard < 256 && reg.valid(cur); ++guard) {
        const Parent* p = reg.try_get<Parent>(cur);
        if (p && reg.valid(p->entity)) cur = p->entity;
        else break;
    }
    return cur;
}

SceneKind Editor::EffectiveLayer(Scene& scene, entt::entity e) const {
    auto& reg = scene.Registry();
    const entt::entity root = RootOf(scene, e);
    if (const SceneLayer* sl = reg.try_get<SceneLayer>(root)) return sl->kind;
    return ClassifyLayer(scene, root);
}

std::filesystem::path Editor::LevelBaseOf(Scene& scene, entt::entity e) const {
    auto& reg = scene.Registry();
    const entt::entity root = RootOf(scene, e);
    if (const SceneSource* ss = reg.try_get<SceneSource>(root); ss && !ss->scene.empty()) {
        const std::filesystem::path p(ss->scene);
        if (scene::IsLevelMember(p)) return scene::ResolveLevel(p).base;
    }
    return currentLevel_.base; // the active level
}

void Editor::AssignToLevel(Scene& scene, entt::entity e, const std::filesystem::path& base,
                           SceneKind kind) {
    if (kind == SceneKind::Full || base.empty()) return;
    auto& reg = scene.Registry();
    scene::LevelPaths lp;
    lp.base = base;
    const std::string src = lp.Member(kind).string();
    // Tag the whole hierarchy so it moves as a unit: SceneSource = the layer FILE
    // it saves to, SceneLayer = the kind the navmesh reads.
    std::vector<entt::entity> stack{RootOf(scene, e)};
    while (!stack.empty()) {
        const entt::entity cur = stack.back();
        stack.pop_back();
        if (!reg.valid(cur)) continue;
        reg.emplace_or_replace<SceneSource>(cur, SceneSource{src});
        reg.emplace_or_replace<SceneLayer>(cur, SceneLayer{kind});
        for (const entt::entity c : reg.view<Parent>())
            if (reg.get<Parent>(c).entity == cur) stack.push_back(c);
    }
}

void Editor::AssignToLayer(Scene& scene, entt::entity e, SceneKind kind) {
    AssignToLevel(scene, e, LevelBaseOf(scene, e), kind); // keep level, change layer
}

SceneKind Editor::ClassifyLayer(Scene& scene, entt::entity root) const {
    auto& reg = scene.Registry();
    bool anyUI = false, anyDynamic = false;
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (!reg.valid(e)) continue;
        if (reg.any_of<UICanvas, UIElement>(e)) anyUI = true;
        if (reg.any_of<Animator, CharacterController, NavigationAgent, Rotator,
                       SchematicComponent>(e)) {
            anyDynamic = true;
        }
        if (const RigidBody* rb = reg.try_get<RigidBody>(e);
            rb && rb->motion == RigidBody::Motion::Dynamic) {
            anyDynamic = true;
        }
        for (const entt::entity c : reg.view<Parent>())
            if (reg.get<Parent>(c).entity == e) stack.push_back(c);
    }
    // A level has no UI layer (HUD/menus are separate standalone scenes), so UI
    // created while a level is open falls into the dynamic layer rather than
    // being lost. Author proper UI in its own scene.
    if (anyUI || anyDynamic) return SceneKind::Dynamic; // actors, physics, scripted, UI
    return SceneKind::Static;                           // world geometry (navmesh source)
}

void Editor::EnsureLevelMembership(Scene& scene) {
    // Only while authoring a level. NOT during play: entities spawned by the
    // simulation are transient and must not be folded into the level files, and
    // the play snapshot already preserves each entity's membership.
    if (!levelOpen_ || playMode_ || currentLevel_.base.empty()) return;
    auto& reg = scene.Registry();
    // UI is NEVER part of a level: menus and HUDs live in their own standalone
    // scenes. Folding UI into a level's layer files is the "UI bleeds into the
    // level" bug. UI hierarchies often have a plain (non-UI) ROOT grouping UI
    // children, so detect UI by the whole tree: a ROOT is a "UI root" when its
    // subtree contains any UICanvas/UIElement, and every entity under such a root
    // is skipped (stays untagged -> saves as the active scene instead).
    std::unordered_set<u32> uiRoots;
    for (const entt::entity e : reg.view<UICanvas>())
        uiRoots.insert(static_cast<u32>(RootOf(scene, e)));
    for (const entt::entity e : reg.view<UIElement>())
        uiRoots.insert(static_cast<u32>(RootOf(scene, e)));
    const auto isUI = [&](entt::entity e) {
        return uiRoots.count(static_cast<u32>(RootOf(scene, e))) != 0;
    };
    // Every NON-UI entity must belong to a level layer FILE (SceneSource) so Save
    // writes it back - nothing is ever lost. Fill only the untagged ones: assign
    // each to its hierarchy's level (its root's, else the active level) and layer
    // (its root's SceneLayer, else auto-classified). New objects auto-sort; loaded
    // and manually-set memberships are left alone.
    std::vector<entt::entity> missing;
    for (const entt::entity e : reg.view<entt::entity>()) {
        if (!reg.valid(e) || reg.all_of<TerrainChunk>(e)) continue; // generated
        if (!reg.all_of<SceneSource>(e) && !isUI(e)) missing.push_back(e);
    }
    for (const entt::entity e : missing) {
        const entt::entity root = RootOf(scene, e);
        const SceneLayer* sl = reg.try_get<SceneLayer>(root);
        const SceneKind kind = sl ? sl->kind : ClassifyLayer(scene, root);
        scene::LevelPaths lp;
        lp.base = LevelBaseOf(scene, e);
        if (lp.base.empty()) continue;
        reg.emplace_or_replace<SceneSource>(e, SceneSource{lp.Member(kind).string()});
        reg.emplace_or_replace<SceneLayer>(e, SceneLayer{kind});
    }
}

entt::entity Editor::SpawnMeshAsset(Scene& scene, Renderer& renderer,
                                    const std::filesystem::path& uafPath, bool frameCamera) {
    std::optional<Model> model = assets::LoadMesh(uafPath);
    if (!model) return entt::null;
    PushUndo(scene);

    const std::filesystem::path assetsDir = Project::Active().AssetsDir();
    auto loadTex = [&](const std::string& name) -> rhi::TextureHandle {
        if (name.empty()) return {};
        if (auto it = textureCache_.find(name); it != textureCache_.end()) return it->second;
        const rhi::TextureHandle h = assets::LoadTexture(renderer, assetsDir / name);
        textureCache_[name] = h;
        return h;
    };

    // Multi-mesh assets spawn as a hierarchy: one root + a child per submesh.
    entt::entity root = entt::null;
    if (model->size() > 1) {
        root = scene.CreateEntity(uafPath.stem().string());
        scene.Registry().emplace<Transform>(root);
    }

    const std::string relUaf = Project::Active().RelativeAssetPath(uafPath);
    glm::vec3 bmin(1e30f), bmax(-1e30f);
    u32 submeshIndex = 0;
    entt::entity lastSpawned = entt::null;
    for (const MeshData& md : *model) {
        const u32 thisSubmesh = submeshIndex++;
        const rhi::MeshHandle handle = renderer.UploadMesh(md);
        if (!handle.IsValid()) continue;
        const entt::entity e =
            scene.CreateEntity(md.name.empty() ? uafPath.stem().string() : md.name);
        lastSpawned = e;
        scene.Registry().emplace<Transform>(e);
        scene.Registry().emplace<MeshRef>(
            e, MeshRef{"uaf:" + relUaf + "#" + std::to_string(thisSubmesh)});
        if (root != entt::null) scene.Registry().emplace<Parent>(e, Parent{root});
        MeshInstance mi;
        mi.mesh = handle;
        // Imported models carry a generated .hbmat per material (v4+); apply
        // it as the source of truth and link the entity. Older assets fall
        // back to the material values baked into the mesh.
        if (!md.material.materialAsset.empty()) {
            if (const auto mat =
                    assets::LoadMaterial(assetsDir / md.material.materialAsset)) {
                assets::ApplyMaterial(renderer, assetsDir, *mat, mi, textureCache_);
                scene.Registry().emplace<MaterialRef>(
                    e, MaterialRef{md.material.materialAsset});
            }
        }
        if (!scene.Registry().all_of<MaterialRef>(e)) {
            mi.baseColor = md.material.baseColor;
            mi.metallic = md.material.metallic;
            mi.roughness = md.material.roughness;
            mi.albedoTexture = loadTex(md.material.baseColorTex);
            mi.normalTexture = loadTex(md.material.normalTex);
            mi.mrTexture = loadTex(md.material.mrTex);
            mi.aoTexture = loadTex(md.material.aoTex);
            mi.emissiveTexture = loadTex(md.material.emissiveTex);
            mi.emissiveColor = md.material.emissive;
        }
        scene.Registry().emplace<MeshInstance>(e, mi);

        // Skinned submeshes get an Animator: the asset's rig drives them (the
        // first clip autoplays; the Inspector retargets/edits playback).
        const bool skinned = std::any_of(
            md.vertices.begin(), md.vertices.end(),
            [](const Vertex& v) { return v.weights[0] > 0.0f; });
        if (skinned) {
            scene.Registry().emplace<Animator>(e);
        }

        glm::vec3 mn, mx;
        ComputeBounds(md, mn, mx);
        scene.Registry().emplace<AABB>(e, AABB{mn, mx});

        // Static box collider fitted to the mesh bounds (scenery default);
        // switch shape/motion in the Inspector for gameplay objects.
        RigidBody rb;
        rb.shape = RigidBody::Shape::Box;
        rb.motion = RigidBody::Motion::Static;
        rb.halfExtents = glm::max((mx - mn) * 0.5f, glm::vec3(0.01f));
        rb.centerOffset = (mn + mx) * 0.5f;
        rb.radius = glm::max(rb.halfExtents.x, glm::max(rb.halfExtents.y, rb.halfExtents.z));
        scene.Registry().emplace<RigidBody>(e, rb);

        bmin = glm::min(bmin, mn);
        bmax = glm::max(bmax, mx);
    }

    // Frame the camera on the imported model's bounds so it is visible.
    if (frameCamera && bmax.x >= bmin.x) {
        const glm::vec3 center = (bmin + bmax) * 0.5f;
        const f32 radius = glm::length(bmax - bmin) * 0.5f;
        if (radius > 0.0f) renderer.FocusOn(center, radius);
    }
    return root != entt::null ? root : lastSpawned;
}

namespace {
// The game's target aspect (w/h) from the project's build resolution. The editor
// letterboxes the viewport to this so UI authored/previewed in the editor lands
// exactly where it will in the exported game (the UI canvas scales with the render
// aspect, so a free-aspect editor panel would misplace anchored UI).
f32 GameTargetAspect() {
    if (Project::HasActive()) {
        const BuildSettings& b = Project::Active().Settings().build;
        if (b.width > 0 && b.height > 0)
            return static_cast<f32>(b.width) / static_cast<f32>(b.height);
    }
    return 16.0f / 9.0f;
}
} // namespace

void Editor::DrawViewport(Engine& engine) {
    Renderer& renderer = engine.GetRenderer();
    vpVisible_ = false;
    if (!panelOpen_[Panel_Viewport]) {
        vpHovered_ = false;
        vpClicked_ = false;
        return;
    }
    if (focusViewport_) {
        ImGui::SetNextWindowFocus();
        focusViewport_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // A hidden tab (e.g. the Schematic Editor or Game view is active in this dock
    // node) draws nothing - and the selection outline / gizmo must not either.
    if (!ImGui::Begin("Viewport", &panelOpen_[Panel_Viewport])) {
        vpHovered_ = false;
        vpClicked_ = false;
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // Letterbox to the game's target aspect: what you author/see here is exactly
    // what the exported game renders (centered, with bars on the long axis).
    const f32 aspect = GameTargetAspect();
    f32 fw = avail.x > 1.0f ? avail.x : 1.0f;
    f32 fh = avail.y > 1.0f ? avail.y : 1.0f;
    if (fw / fh > aspect) fw = fh * aspect; else fh = fw / aspect;
    vpX_ = pos.x + (avail.x - fw) * 0.5f;
    vpY_ = pos.y + (avail.y - fh) * 0.5f;
    vpW_ = fw; vpH_ = fh;

    renderer.SetViewportSize(static_cast<u32>(vpW_), static_cast<u32>(vpH_));
    const u64 tex = renderer.ViewportTextureId();
    if (tex != 0) {
        renderer.GetCamera().SetAspect(vpW_ / vpH_); // match the offscreen target
        ImGui::SetCursorScreenPos(ImVec2(vpX_, vpY_)); // center the letterboxed image
        ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(vpW_, vpH_));
        vpVisible_ = true;
        // Item-precise input state: only the rendered image counts, not the
        // tab bar or window padding.
        vpHovered_ = ImGui::IsItemHovered();
        vpClicked_ = vpHovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        // UI layout editing happens here in the Scene view (the overlay
        // renders into this same image). A consumed click never 3D-picks.
        if (!playMode_ &&
            DrawUIEditOverlay(engine, {vpX_, vpY_}, {vpW_, vpH_})) {
            vpClicked_ = false;
        }
    } else {
        ImGui::TextDisabled("(viewport target not available on this backend)");
        vpHovered_ = false;
        vpClicked_ = false;
    }

    // Drop assets straight into the world: meshes spawn at the point under
    // the cursor; scenes stream in additively; everything else (materials,
    // textures, scripts, audio, fonts) applies to the object under the cursor.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HBE_ASSET_PATH")) {
            const std::filesystem::path src(static_cast<const char*>(p->Data));
            const ImVec2 mouse = ImGui::GetMousePos();
            const f32 mx = (mouse.x - vpX_) / vpW_;
            const f32 my = (mouse.y - vpY_) / vpH_;
            Scene& scene = engine.GetScene();
            const bool isMeshAsset = src.extension() == ".uaf" &&
                                     uaf::PeekType(src) == uaf::AssetType::Mesh;
            if (src.extension() == ".hbscene") {
                streamer_.BeginLoad(src, Project::Active().AssetsDir(),
                                    scene::LoadMode::Additive);
            } else if (src.extension() == ".hbprefab") {
                const glm::vec3 at = DropPointInWorld(scene, renderer, mx, my);
                InstantiatePrefab(engine, src, &at);
            } else if (!isMeshAsset) {
                // Paint/attach onto the object under the cursor.
                const entt::entity target = EntityUnderPixel(scene, renderer, mx, my);
                if (target != entt::null) {
                    ApplyAssetDropToEntity(engine, target, src);
                }
            } else if (isMeshAsset) {
                const glm::vec3 at = DropPointInWorld(scene, renderer, mx, my);
                const entt::entity e = SpawnMeshAsset(scene, renderer, src,
                                                      /*frameCamera=*/false);
                if (e != entt::null) {
                    auto& reg = scene.Registry();
                    // Center the spawned model's bounds on the drop point.
                    glm::vec3 wmin(1e30f), wmax(-1e30f);
                    bool any = false;
                    auto accumulate = [&](entt::entity ent) {
                        if (const AABB* box = reg.try_get<AABB>(ent)) {
                            const glm::mat4 m = scene.WorldMatrix(ent);
                            for (int c = 0; c < 8; ++c) {
                                const glm::vec3 corner((c & 1) ? box->max.x : box->min.x,
                                                       (c & 2) ? box->max.y : box->min.y,
                                                       (c & 4) ? box->max.z : box->min.z);
                                const glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
                                wmin = glm::min(wmin, w);
                                wmax = glm::max(wmax, w);
                                any = true;
                            }
                        }
                    };
                    accumulate(e);
                    for (const entt::entity c : reg.view<Parent>()) {
                        if (reg.get<Parent>(c).entity == e) accumulate(c);
                    }
                    if (Transform* t = reg.try_get<Transform>(e)) {
                        const glm::vec3 center =
                            any ? (wmin + wmax) * 0.5f : glm::vec3(0.0f);
                        // Rest the model ON the surface rather than centering
                        // through it: lift by half its height.
                        const f32 lift = any ? (wmax.y - wmin.y) * 0.5f : 0.0f;
                        t->position += at - center + glm::vec3(0.0f, lift, 0.0f);
                    }
                    selected_ = e;
                    if (renderer.IsOrbitEnabled()) {
                        SyncFreecam(renderer);
                        renderer.SetOrbitEnabled(false);
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // The gizmo MUST be drawn inside this window with the window's draw list:
    // ImGuizmo only accepts input while ImGui's hovered window matches the
    // draw list's owner (a foreground-drawlist gizmo renders but ignores the
    // mouse entirely).
    DrawGizmo(engine);

    ImGui::End();
    ImGui::PopStyleVar();
}

// --- Game view / play mode ----------------------------------------------------

void Editor::EnterPlayMode(Engine& engine) {
    if (!playMode_) {
        // Snapshot the authored scene; Stop restores it exactly.
        playSnapshot_ = scene::SaveSceneToString(engine.GetScene());
        playMode_ = true;
        focusGameView_ = true;
    }
    playPaused_ = false;
    engine.GetPhysics().SetRunning(true); // scripts follow physics' play state
    engine.SetGameCameraEnabled(true);    // the primary CameraComponent renders
}

void Editor::StopPlayMode(Engine& engine) {
    if (!playMode_) return;
    engine.GetPhysics().SetRunning(false);
    engine.SetGameCameraEnabled(false); // back to the editor camera
    // Resume the editor freecam from the game camera's final pose so the view
    // doesn't snap back to a stale editor position when play stops.
    SyncFreecam(engine.GetRenderer());
    playMode_ = false;
    playPaused_ = false;
    if (!playSnapshot_.empty()) {
        RestoreSnapshot(engine, playSnapshot_);
        playSnapshot_.clear();
    }
    focusViewport_ = true;
}

void Editor::DrawGameView(Engine& engine) {
    Renderer& renderer = engine.GetRenderer();
    if (!panelOpen_[Panel_Game]) return;
    if (focusGameView_) {
        ImGui::SetNextWindowFocus();
        focusGameView_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("Game", &panelOpen_[Panel_Game])) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // --- Transport bar (centered Play / Pause / Stop) -----------------------
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        ImGui::BeginChild("##transport", ImVec2(0, ImGui::GetFrameHeight() + 12.0f),
                          ImGuiChildFlags_None);
        const f32 buttonW = 74.0f;
        const int buttonCount = playMode_ ? 2 : 1;
        const f32 total = buttonW * buttonCount + ImGui::GetStyle().ItemSpacing.x *
                                                      (buttonCount - 1);
        ImGui::SetCursorPosX(
            glm::max((ImGui::GetContentRegionAvail().x - total) * 0.5f, 0.0f));
        ImGui::SetCursorPosY(6.0f);

        if (!playMode_) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.42f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.55f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.36f, 0.21f, 1.0f));
            if (ImGui::Button("Play", ImVec2(buttonW, 0))) EnterPlayMode(engine);
            ImGui::PopStyleColor(3);
        } else {
            if (ImGui::Button(playPaused_ ? "Resume" : "Pause", ImVec2(buttonW, 0))) {
                playPaused_ = !playPaused_;
                engine.GetPhysics().SetRunning(!playPaused_);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.52f, 0.17f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.66f, 0.22f, 0.26f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.44f, 0.14f, 0.17f, 1.0f));
            if (ImGui::Button("Stop", ImVec2(buttonW, 0))) StopPlayMode(engine);
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(playPaused_ ? "PAUSED" : "PLAYING");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // --- Game image (the same offscreen target the Viewport shows) -----------
    // Letterbox to the project's target aspect so this preview is a true WYSIWYG
    // of the exported game (UI layout depends on the render aspect).
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 gorigin = ImGui::GetCursorScreenPos();
    const f32 gAspect = GameTargetAspect();
    f32 w = glm::max(avail.x, 1.0f);
    f32 h = glm::max(avail.y, 1.0f);
    if (w / h > gAspect) w = h * gAspect; else h = w / gAspect;
    const ImVec2 imgPos(gorigin.x + (avail.x - w) * 0.5f, gorigin.y + (avail.y - h) * 0.5f);
    renderer.SetViewportSize(static_cast<u32>(w), static_cast<u32>(h));
    const u64 tex = renderer.ViewportTextureId();
    if (tex != 0) {
        renderer.GetCamera().SetAspect(w / h);
        ImGui::SetCursorScreenPos(imgPos);
        ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(w, h));
        // RMB-fly works from the Game view too (no picking/gizmo here).
        vpHovered_ = vpHovered_ || ImGui::IsItemHovered();
        // Feed the in-game UI pointer (normalized over the game image). The
        // Game view is a pure preview - UI layout editing lives in the Scene
        // viewport, like the rest of the scene.
        if (ImGui::IsItemHovered()) {
            const ImVec2 mouse = ImGui::GetMousePos();
            engine.SetUIPointer((mouse.x - imgPos.x) / w, (mouse.y - imgPos.y) / h);
        } else {
            engine.SetUIPointer(-1.0f, -1.0f);
        }
    } else {
        ImGui::TextDisabled("(viewport target not available on this backend)");
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

namespace {
// The project's canvas configuration (mirrors what the engine uses).
ui::CanvasConfig CanvasConfigFromProject() {
    ui::CanvasConfig config;
    if (Project::HasActive()) {
        const BuildSettings& build = Project::Active().Settings().build;
        config.mode = static_cast<ui::ScaleMode>(glm::clamp(build.uiScaleMode, 0u, 2u));
        config.refWidth = static_cast<f32>(glm::max(build.uiRefWidth, 64u));
        config.refHeight = static_cast<f32>(glm::max(build.uiRefHeight, 64u));
    }
    return config;
}
} // namespace

bool Editor::DrawUIEditOverlay(Engine& engine, const glm::vec2& imgPos,
                               const glm::vec2& imgSize) {
    bool consumedClick = false;
    Scene& scene = engine.GetScene();
    auto& reg = scene.Registry();

    // Lay out every UI tree exactly like the renderer does; each item carries
    // its own canvas size (canvases can differ), so screen mapping is per-item.
    static std::vector<ui::LayoutItem> layout;
    ui::LayoutUI(scene, imgSize, CanvasConfigFromProject(), layout);

    const auto toScreen = [&](glm::vec2 c, glm::vec2 canvas) {
        return ImVec2(imgPos.x + c.x / canvas.x * imgSize.x,
                      imgPos.y + c.y / canvas.y * imgSize.y);
    };
    const ImVec2 mouseIm = ImGui::GetMousePos();
    const glm::vec2 mouseNorm((mouseIm.x - imgPos.x) / imgSize.x,
                              (mouseIm.y - imgPos.y) / imgSize.y);
    const bool mouseOverImage =
        mouseNorm.x >= 0.0f && mouseNorm.x <= 1.0f && mouseNorm.y >= 0.0f &&
        mouseNorm.y <= 1.0f;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(imgPos.x, imgPos.y),
                       ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y), true);

    // Faint outlines for every UI element (so they're discoverable even when
    // transparent), strong outline + handles for the selection.
    constexpr f32 kHandlePx = 5.0f;
    const ui::LayoutItem* selItem = nullptr;
    for (const ui::LayoutItem& item : layout) {
        if (item.entity == selected_) {
            selItem = &item;
            continue;
        }
        draw->AddRect(toScreen({item.rect.x0, item.rect.y0}, item.canvas),
                      toScreen({item.rect.x1, item.rect.y1}, item.canvas),
                      IM_COL32(255, 255, 255, 36));
    }
    if (selItem) {
        const UIElement& el = reg.get<UIElement>(selected_);
        const ImVec2 r0 = toScreen({selItem->rect.x0, selItem->rect.y0}, selItem->canvas);
        const ImVec2 r1 = toScreen({selItem->rect.x1, selItem->rect.y1}, selItem->canvas);
        draw->AddRect(r0, r1, IM_COL32(255, 160, 40, 235), 0.0f, 0, 2.0f);
        if (!el.fullscreen) {
            for (const ImVec2 corner :
                 {r0, ImVec2(r1.x, r0.y), r1, ImVec2(r0.x, r1.y)}) {
                draw->AddRectFilled(ImVec2(corner.x - kHandlePx, corner.y - kHandlePx),
                                    ImVec2(corner.x + kHandlePx, corner.y + kHandlePx),
                                    IM_COL32(255, 160, 40, 255));
            }
        }
    }
    draw->PopClipRect();

    // --- Interaction ---------------------------------------------------------
    // Dragging works on the element's rect in ITS canvas units, then back-
    // solves the RectTransform (anchors/pivot preserved).
    const auto snap = [&](f32 v) {
        return uiSnap_ ? std::round(v / uiSnapStep_) * uiSnapStep_ : v;
    };
    const f32 kMagnet = 14.0f; // canvas units: canvas center/edge magnetism

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouseOverImage &&
        uiDragMode_ == 0) {
        // Corner handles first (resize), then body (move), then re-select.
        bool started = false;
        if (selItem && !reg.get<UIElement>(selected_).fullscreen) {
            const glm::vec2 c[4] = {
                {selItem->rect.x0, selItem->rect.y0},  // TL
                {selItem->rect.x1, selItem->rect.y0},  // TR
                {selItem->rect.x1, selItem->rect.y1},  // BR
                {selItem->rect.x0, selItem->rect.y1}}; // BL
            const f32 grabPx = kHandlePx + 4.0f;
            for (int i = 0; i < 4 && !started; ++i) {
                const ImVec2 s = toScreen(c[i], selItem->canvas);
                if (std::abs(mouseIm.x - s.x) <= grabPx &&
                    std::abs(mouseIm.y - s.y) <= grabPx) {
                    PushUndo(scene);
                    uiDragMode_ = 2 + i;
                    uiDragStartMouse_ = mouseNorm * selItem->canvas;
                    uiDragStartCenter_ = (c[0] + c[2]) * 0.5f;
                    uiDragStartSize_ = c[2] - c[0];
                    uiDragParentRect_ = {selItem->parentRect.x0, selItem->parentRect.y0,
                                         selItem->parentRect.x1, selItem->parentRect.y1};
                    uiDragCanvasSize_ = selItem->canvas;
                    started = true;
                    consumedClick = true;
                }
            }
        }
        if (!started) {
            // Topmost element under the cursor (last laid out wins).
            const ui::LayoutItem* hit = nullptr;
            for (const ui::LayoutItem& item : layout) {
                if (item.rect.Contains(mouseNorm * item.canvas)) hit = &item;
            }
            if (hit) {
                consumedClick = true;
                if (hit->entity == selected_ &&
                    !reg.get<UIElement>(hit->entity).fullscreen) {
                    PushUndo(scene);
                    uiDragMode_ = 1; // move
                    uiDragStartMouse_ = mouseNorm * hit->canvas;
                    uiDragStartCenter_ = (hit->rect.Min() +
                                          glm::vec2(hit->rect.x1, hit->rect.y1)) * 0.5f;
                    uiDragStartSize_ = hit->rect.Size();
                    uiDragParentRect_ = {hit->parentRect.x0, hit->parentRect.y0,
                                         hit->parentRect.x1, hit->parentRect.y1};
                    uiDragCanvasSize_ = hit->canvas;
                } else {
                    selected_ = hit->entity;
                }
            }
        }
    }

    const bool selectedIsUI = selected_ != entt::null && reg.valid(selected_) &&
                              reg.all_of<UIElement>(selected_);
    if (uiDragMode_ != 0 && selectedIsUI && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        UIElement& el = reg.get<UIElement>(selected_);
        const glm::vec2 canvas = uiDragCanvasSize_;
        const ui::Rect parent{uiDragParentRect_.x, uiDragParentRect_.y,
                              uiDragParentRect_.z, uiDragParentRect_.w};
        const glm::vec2 mouseCanvas = mouseNorm * canvas;
        const glm::vec2 delta = mouseCanvas - uiDragStartMouse_;

        glm::vec2 center = uiDragStartCenter_;
        glm::vec2 size = uiDragStartSize_;
        if (uiDragMode_ == 1) {
            center = uiDragStartCenter_ + delta;
            center = {snap(center.x), snap(center.y)};
            if (uiSnap_) {
                // Canvas-center and edge magnetism.
                const glm::vec2 half = size * 0.5f;
                if (std::abs(center.x - canvas.x * 0.5f) < kMagnet) center.x = canvas.x * 0.5f;
                if (std::abs(center.y - canvas.y * 0.5f) < kMagnet) center.y = canvas.y * 0.5f;
                if (std::abs(center.x - half.x) < kMagnet) center.x = half.x;
                if (std::abs(center.y - half.y) < kMagnet) center.y = half.y;
                if (std::abs(canvas.x - (center.x + half.x)) < kMagnet) center.x = canvas.x - half.x;
                if (std::abs(canvas.y - (center.y + half.y)) < kMagnet) center.y = canvas.y - half.y;
            }
        } else {
            // Resize: the dragged corner follows the mouse, its opposite stays.
            const int corner = uiDragMode_ - 2; // 0 TL, 1 TR, 2 BR, 3 BL
            const glm::vec2 sign((corner == 1 || corner == 2) ? 1.0f : -1.0f,
                                 (corner == 2 || corner == 3) ? 1.0f : -1.0f);
            const glm::vec2 fixed = uiDragStartCenter_ -
                                    sign * uiDragStartSize_ * 0.5f; // opposite corner
            glm::vec2 moving = uiDragStartCenter_ + sign * uiDragStartSize_ * 0.5f +
                               delta;
            moving = {snap(moving.x), snap(moving.y)};
            size = glm::max(glm::abs(moving - fixed), glm::vec2(8.0f));
            center = (moving + fixed) * 0.5f;
        }
        const ui::Rect desired{center.x - size.x * 0.5f, center.y - size.y * 0.5f,
                               center.x + size.x * 0.5f, center.y + size.y * 0.5f};
        ui::SolveElementFromRect(el, parent, desired);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) uiDragMode_ = 0;
    return consumedClick || uiDragMode_ != 0;
}

void Editor::SyncFreecam(Renderer& renderer) {
    freecam_.SyncFrom(renderer.GetCamera());
}

void Editor::UpdateFreecam(Renderer& renderer, const Input& input, f32 dt) {
    // Start flying only when right-dragging over the viewport; keep flying while
    // the button stays held even if the cursor leaves the panel. Gamepad input
    // (handled inside the controller) steers regardless of hover.
    const bool rmb = input.IsMouseDown(MouseButton::Right);
    const bool wantFly = rmb && (freecamActive_ || vpHovered_) && !ImGuizmo::IsUsing();
    freecamActive_ = wantFly;

    freecam_.Update(input, renderer, dt, wantFly);
}

void Editor::EnsurePrimitives(Renderer& renderer) {
    if (!cubeMesh_.IsValid()) cubeMesh_ = renderer.UploadMesh(mesh::GenerateCube(1.0f));
    if (!sphereMesh_.IsValid()) sphereMesh_ = renderer.UploadMesh(mesh::GenerateSphere(0.5f, 32, 16));
}

entt::entity Editor::CreateEntityPrim(Scene& scene, Renderer& renderer, int prim) {
    PushUndo(scene);
    const char* name = prim == 1 ? "Cube" : prim == 2 ? "Sphere" : "Empty";
    const entt::entity e = scene.CreateEntity(name);
    scene.Registry().emplace<Transform>(e);
    if (prim != 0) {
        EnsurePrimitives(renderer);
        MeshInstance mi;
        mi.mesh = (prim == 1) ? cubeMesh_ : sphereMesh_;
        mi.baseColor = {0.8f, 0.8f, 0.82f, 1.0f};
        mi.roughness = 0.5f;
        scene.Registry().emplace<MeshInstance>(e, mi);
        scene.Registry().emplace<MeshRef>(
            e, MeshRef{prim == 1 ? "prim:cube" : "prim:sphere"});
        // Cube(1.0) and Sphere(0.5) both span +/-0.5.
        scene.Registry().emplace<AABB>(e, AABB{glm::vec3(-0.5f), glm::vec3(0.5f)});

        // Matching collider so "Simulate physics" just works on new prims.
        RigidBody rb;
        rb.shape = (prim == 2) ? RigidBody::Shape::Sphere : RigidBody::Shape::Box;
        rb.radius = 0.5f;
        rb.halfExtents = glm::vec3(0.5f);
        rb.restitution = 0.3f;
        scene.Registry().emplace<RigidBody>(e, rb);
    }
    return e;
}

entt::entity Editor::CreateMeshEntity(Scene& scene, Renderer& renderer, const char* prim) {
    PushUndo(scene);
    auto& reg = scene.Registry();
    MeshData md = mesh::GeneratePrimitive(prim);
    // Capitalize the label (cube -> Cube).
    std::string label = prim;
    if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    const entt::entity e = scene.CreateEntity(label);
    reg.emplace<Transform>(e);
    if (md.vertices.empty()) return e; // unknown prim -> empty entity
    MeshInstance mi;
    mi.mesh = renderer.UploadMesh(md);
    mi.baseColor = {0.8f, 0.8f, 0.82f, 1.0f};
    mi.roughness = 0.5f;
    reg.emplace<MeshInstance>(e, mi);
    reg.emplace<MeshRef>(e, MeshRef{std::string("prim:") + prim});
    glm::vec3 bmin, bmax;
    ComputeBounds(md, bmin, bmax);
    reg.emplace<AABB>(e, AABB{bmin, bmax});

    // Matching collider so physics works on new prims (sphere -> sphere, else box).
    RigidBody rb;
    rb.shape = (std::string(prim) == "sphere") ? RigidBody::Shape::Sphere : RigidBody::Shape::Box;
    rb.halfExtents = glm::max((bmax - bmin) * 0.5f, glm::vec3(0.01f));
    rb.centerOffset = (bmin + bmax) * 0.5f;
    rb.radius = glm::max(rb.halfExtents.x, glm::max(rb.halfExtents.y, rb.halfExtents.z));
    rb.restitution = 0.3f;
    reg.emplace<RigidBody>(e, rb);
    return e;
}

void Editor::Reparent(Scene& scene, entt::entity child, entt::entity newParent) {
    auto& reg = scene.Registry();
    if (child == newParent || !reg.valid(child)) return;

    // Refuse cycles: newParent must not be a descendant of child.
    for (entt::entity a = newParent; a != entt::null;) {
        if (a == child) return;
        const Parent* p = reg.try_get<Parent>(a);
        a = (p && reg.valid(p->entity)) ? p->entity : entt::null;
    }

    PushUndo(scene);

    // Keep the world transform: newLocal = inverse(parentWorld) * world.
    const glm::mat4 world = scene.WorldMatrix(child);
    const glm::mat4 parentWorld =
        newParent != entt::null ? scene.WorldMatrix(newParent) : glm::mat4(1.0f);
    if (Transform* t = reg.try_get<Transform>(child)) {
        DecomposeTRS(glm::inverse(parentWorld) * world, *t);
    }

    if (newParent == entt::null) {
        reg.remove<Parent>(child);
    } else {
        reg.emplace_or_replace<Parent>(child, Parent{newParent});
        // A child belongs to its parent's scene (so it saves to the same file).
        const SceneSource* ps = reg.try_get<SceneSource>(newParent);
        MoveToScene(scene, child, ps ? ps->scene : std::string());
    }
}

// The level layer a scene file represents, from its "<name>.static|dynamic|ui"
// filename. Full when the path isn't a level member (a standalone scene).
static SceneKind KindFromScenePath(const std::string& p) {
    if (p.empty()) return SceneKind::Full;
    const std::filesystem::path fp(p);
    if (!scene::IsLevelMember(fp)) return SceneKind::Full;
    const std::string stem = fp.stem().string(); // "<name>.<kind>"
    return SceneKindFromString(stem.substr(stem.find_last_of('.') + 1));
}

void Editor::MoveToScene(Scene& scene, entt::entity root, const std::string& scenePath) {
    auto& reg = scene.Registry();
    if (!reg.valid(root)) return;
    const SceneKind kind = KindFromScenePath(scenePath);
    // Re-tag the entity and everything parented under it (a subtree moves whole).
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (scenePath.empty()) {
            reg.remove<SceneSource>(e); // active scene = untagged
            reg.remove<SceneLayer>(e);
        } else {
            reg.emplace_or_replace<SceneSource>(e, SceneSource{scenePath});
            if (kind != SceneKind::Full) reg.emplace_or_replace<SceneLayer>(e, SceneLayer{kind});
            else reg.remove<SceneLayer>(e);
        }
        for (const entt::entity c : reg.view<Parent>())
            if (reg.get<Parent>(c).entity == e) stack.push_back(c);
    }
}

void Editor::DestroyRecursive(Scene& scene, entt::entity e) {
    auto& reg = scene.Registry();
    if (!reg.valid(e)) return;
    // Children list is copied: destroying mutates the Parent storage we scan.
    std::vector<entt::entity> children;
    for (const entt::entity c : reg.view<Parent>()) {
        if (reg.get<Parent>(c).entity == e) children.push_back(c);
    }
    for (const entt::entity c : children) DestroyRecursive(scene, c);
    if (selected_ == e) selected_ = entt::null;
    reg.destroy(e);
}

void Editor::DrawEntityNode(Scene& scene, Renderer& renderer, entt::entity e) {
    auto& reg = scene.Registry();
    const char* label = "Entity";
    if (const Name* n = reg.try_get<Name>(e)) label = n->value.c_str();

    const auto kidsIt = childrenByParent_.find(static_cast<u32>(e));
    const bool hasKids = kidsIt != childrenByParent_.end() && !kidsIt->second.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf;
    if (e == selected_) flags |= ImGuiTreeNodeFlags_Selected;

    // Editor-only visibility: hidden entities (and their subtrees) are dimmed.
    const bool selfHidden = reg.all_of<EditorHidden>(e);
    if (selfHidden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.50f, 0.56f, 1.0f));
    const bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<u32>(e))), flags,
        "%s", label);
    if (selfHidden) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        selected_ = e;
        if (renderer.IsOrbitEnabled()) SyncFreecam(renderer);
        renderer.SetOrbitEnabled(false); // stop auto-orbit so the gizmo is usable
    }

    // Drag to reparent. (Bind drag-source / drop-target / context menu to the tree
    // node item BEFORE drawing the eye button, or they'd attach to the button.)
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("HBE_ENTITY", &e, sizeof(e));
        ImGui::TextUnformatted(label);
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HBE_ENTITY")) {
            entt::entity dropped;
            std::memcpy(&dropped, p->Data, sizeof(dropped));
            Reparent(scene, dropped, e);
        }
        // Assets dropped onto a row apply to that entity (material paint,
        // script attach, audio source, mesh spawns as a child, ...).
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HBE_ASSET_PATH")) {
            const std::filesystem::path src(static_cast<const char*>(p->Data));
            if (engine_) ApplyAssetDropToEntity(*engine_, e, src);
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem(selfHidden ? "Show in editor" : "Hide in editor")) {
            if (selfHidden) reg.remove<EditorHidden>(e);
            else reg.emplace<EditorHidden>(e);
        }
        if (ImGui::MenuItem("Save as Prefab...")) {
            // Routes through the asset-browser "New Asset" name modal (kind 5),
            // which calls CreatePrefabFromSelection on the entity selected here.
            selected_ = e;
            pendingCreateKind_ = 5;
            pendingCreateDir_ = Project::HasActive()
                                    ? Project::Active().AssetsDir() / "Prefabs"
                                    : std::filesystem::path();
            const Name* nm = reg.try_get<Name>(e);
            std::snprintf(newAssetNameBuf_, sizeof(newAssetNameBuf_), "%s",
                          nm && !nm->value.empty() ? nm->value.c_str() : "Prefab");
        }
        if (ImGui::MenuItem("Delete")) pendingDelete_ = e;
        if (reg.any_of<Parent>(e) && ImGui::MenuItem("Unparent")) {
            Reparent(scene, e, entt::null);
        }
        ImGui::EndPopup();
    }

    // Eye toggle at the right edge: hide/show this entity + its children in the
    // viewport (it stays loaded). Drawn AFTER the tree node's drag/drop/context so
    // those keep binding to the row, not this button. SameLine resumes on the row
    // (the behaviors above don't emit layout items).
    {
        ImGui::PushID(static_cast<int>(static_cast<u32>(e)));
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 22.0f);
        if (ImGui::SmallButton(selfHidden ? "-" : "o")) {
            if (selfHidden) reg.remove<EditorHidden>(e);
            else reg.emplace<EditorHidden>(e);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(selfHidden ? "Hidden in editor - click to show"
                                         : "Visible - click to hide (with children)");
        ImGui::PopID();
    }

    if (open) {
        if (hasKids) {
            for (const entt::entity c : kidsIt->second) {
                DrawEntityNode(scene, renderer, c);
            }
        }
        ImGui::TreePop();
    }
}

void Editor::DrawHierarchy(Scene& scene, Renderer& renderer) {
    if (!panelOpen_[Panel_Hierarchy]) return;
    ImGui::Begin("Hierarchy", &panelOpen_[Panel_Hierarchy]);
    auto& reg = scene.Registry();

    // Toolbar: create / delete entities.
    if (ImGui::Button("+ Create")) ImGui::OpenPopup("CreateEntity");
    if (ImGui::BeginPopup("CreateEntity")) {
        if (ImGui::MenuItem("Empty")) selected_ = CreateEntityPrim(scene, renderer, 0);
        if (ImGui::BeginMenu("3D Object")) {
            for (const char* p : mesh::kPrimitiveNames) {
                std::string label = p;
                label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
                if (ImGui::MenuItem(label.c_str())) {
                    selected_ = CreateMeshEntity(scene, renderer, p);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Terrain")) {
                PushUndo(scene);
                const entt::entity e = scene.CreateEntity("Terrain");
                reg.emplace<Transform>(e, Transform{});
                reg.emplace<TerrainComponent>(e);
                selected_ = e;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Point Light")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Point Light");
            reg.emplace<Transform>(e, Transform{{0.0f, 3.0f, 0.0f}});
            reg.emplace<PointLightComponent>(e);
            selected_ = e;
        }
        if (ImGui::MenuItem("Spot Light")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Spot Light");
            reg.emplace<Transform>(e, Transform{{0.0f, 5.0f, 0.0f}});
            reg.emplace<SpotLightComponent>(e);
            selected_ = e;
        }
        if (ImGui::MenuItem("Rect (Area) Light")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Rect Light");
            reg.emplace<Transform>(e, Transform{{0.0f, 4.0f, 0.0f}});
            reg.emplace<RectLightComponent>(e);
            selected_ = e;
        }
        if (ImGui::MenuItem("Directional Light")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Sun");
            reg.emplace<Transform>(e);
            reg.emplace<DirectionalLightComponent>(e);
            selected_ = e;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Camera")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Game Camera");
            Transform t;
            t.position = {0.0f, 2.0f, 8.0f};
            reg.emplace<Transform>(e, t);
            reg.emplace<CameraComponent>(e);
            selected_ = e;
        }
        if (ImGui::MenuItem("Camera Zone")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Camera Zone");
            reg.emplace<Transform>(e, Transform{});
            reg.emplace<CameraZone>(e);
            selected_ = e;
        }
        if (ImGui::MenuItem("Post Volume")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Post Volume");
            reg.emplace<Transform>(e, Transform{});
            reg.emplace<PostVolume>(e);
            selected_ = e;
        }
        if (ImGui::MenuItem("Reflection Probe")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Reflection Probe");
            reg.emplace<Transform>(e, Transform{});
            reg.emplace<ReflectionProbe>(e);
            selected_ = e;
        }
        if (ImGui::MenuItem("Bake GI Volume (whole level)")) {
            const std::filesystem::path assets = Project::Active().AssetsDir();
            SceneEnvironment& env = scene.Environment();
            if (env.giSource.empty()) env.giSource = "GI/volume.hbgi";
            const GiVolume vol = BakeGIVolume(renderer, scene, assets, {}, assets / env.giSource);
            if (vol.valid) {
                env.giSh = vol.sh;
                env.giDepth = vol.depth;
                env.giOrigin = vol.origin;
                env.giSpacing = vol.spacing;
                env.giDims = vol.dims;
            }
        }
        if (ImGui::MenuItem("Auto-Place + Bake Probes")) {
            PushUndo(scene);
            const std::filesystem::path assets = Project::Active().AssetsDir();
            const std::vector<ProbePlacement> places = AutoPlaceProbes(scene, assets, 4.0f);
            if (!places.empty()) {
                const entt::entity parent = scene.CreateEntity("Probes");
                reg.emplace<Transform>(parent, Transform{});
                for (const ProbePlacement& pl : places) {
                    const entt::entity e = scene.CreateEntity("Probe");
                    Transform t;
                    t.position = pl.position;
                    reg.emplace<Transform>(e, t);
                    reg.emplace<Parent>(e, Parent{parent});
                    ReflectionProbe rp;
                    rp.halfExtents = pl.halfExtents;
                    reg.emplace<ReflectionProbe>(e, rp);
                }
                for (const entt::entity e : reg.view<Transform, ReflectionProbe>()) {
                    ReflectionProbe& rp = reg.get<ReflectionProbe>(e);
                    if (rp.source.empty())
                        rp.source = "Probes/probe_" +
                                    std::to_string(static_cast<u32>(entt::to_integral(e))) +
                                    ".hbprobe";
                    const IBLMaps m = BakeLocalProbe(renderer, scene, assets,
                                                     glm::vec3(scene.WorldMatrix(e)[3]), rp.range,
                                                     rp.skyMix, {}, assets / rp.source);
                    if (m.valid) {
                        rp.irradiance = m.irradiance;
                        rp.prefiltered = m.prefiltered;
                        rp.prefilteredMaxLod = m.prefilteredMaxLod;
                        rp.baked = true;
                    }
                }
                selected_ = parent;
            } else {
                HBE_WARN("Auto-place: no enclosed (floor+ceiling) interior found to probe.");
            }
        }
        if (ImGui::MenuItem("Bake All Probes")) {
            const std::filesystem::path assets = Project::Active().AssetsDir();
            for (const entt::entity e : reg.view<Transform, ReflectionProbe>()) {
                ReflectionProbe& rp = reg.get<ReflectionProbe>(e);
                if (rp.source.empty())
                    rp.source = "Probes/probe_" +
                                std::to_string(static_cast<u32>(entt::to_integral(e))) + ".hbprobe";
                const IBLMaps m = BakeLocalProbe(renderer, scene, assets,
                                                 glm::vec3(scene.WorldMatrix(e)[3]), rp.range,
                                                 rp.skyMix, {}, assets / rp.source);
                if (m.valid) {
                    rp.irradiance = m.irradiance;
                    rp.prefiltered = m.prefiltered;
                    rp.prefilteredMaxLod = m.prefilteredMaxLod;
                    rp.baked = true;
                }
            }
        }
        if (ImGui::MenuItem("Camera Spline")) {
            PushUndo(scene);
            const entt::entity e = scene.CreateEntity("Camera Spline");
            reg.emplace<Transform>(e, Transform{});
            CameraSpline sp;
            sp.points = {{-8.0f, 3.0f, 8.0f}, {8.0f, 3.0f, 8.0f}, {8.0f, 3.0f, -8.0f},
                         {-8.0f, 3.0f, -8.0f}};
            reg.emplace<CameraSpline>(e, sp);
            selected_ = e;
        }
        ImGui::Separator();
        // Player: a capsule with a CharacterController + a third-person camera
        // that follows it. Instant playable movement (press Play, WASD/stick).
        if (ImGui::MenuItem("Player (Character)")) {
            PushUndo(scene);
            // A 1-unit capsule at scale 1 (same base size as every other primitive -
            // no weird x2 scale, and it reads smaller than a 2x-scaled wall). Bottom
            // sits on the floor: prim spans y[-0.5,0.5], +0.5 position -> [0,1].
            // Want a taller character? Scale it up (and match the controller below).
            MeshData md = mesh::GeneratePrimitive("capsule");
            const entt::entity e = scene.CreateEntity("Player");
            reg.emplace<Transform>(e, Transform{{0.0f, 0.5f, 0.0f}});
            if (!md.vertices.empty()) {
                MeshInstance mi;
                mi.mesh = renderer.UploadMesh(md);
                mi.baseColor = {0.30f, 0.55f, 0.95f, 1.0f};
                mi.roughness = 0.6f;
                reg.emplace<MeshInstance>(e, mi);
                reg.emplace<MeshRef>(e, MeshRef{"prim:capsule"});
                glm::vec3 bmin, bmax;
                ComputeBounds(md, bmin, bmax);
                reg.emplace<AABB>(e, AABB{bmin, bmax});
            }
            CharacterController cc;
            // Collision is in WORLD units (built directly from these, NOT transform-
            // scaled), so match the 1-unit mesh exactly - otherwise the visible
            // capsule floats above / sinks into its collision capsule.
            cc.radius = 0.25f;
            cc.height = 1.0f;
            cc.turnSpeed = 8.0f; // gentler body turn -> the follow-cam swings less
            reg.emplace<CharacterController>(e, cc);

            // A ground to stand on (thin static box, top at y=0). Delete it if
            // your scene already has a floor with a collider.
            {
                const entt::entity floor = scene.CreateEntity("Ground");
                Transform ft;
                ft.position = {0.0f, -0.5f, 0.0f};
                ft.scale = {30.0f, 1.0f, 30.0f};
                reg.emplace<Transform>(floor, ft);
                MeshData fmd = mesh::GeneratePrimitive("cube");
                if (!fmd.vertices.empty()) {
                    MeshInstance fmi;
                    fmi.mesh = renderer.UploadMesh(fmd);
                    fmi.baseColor = {0.40f, 0.42f, 0.45f, 1.0f};
                    fmi.roughness = 0.9f;
                    reg.emplace<MeshInstance>(floor, fmi);
                    reg.emplace<MeshRef>(floor, MeshRef{"prim:cube"});
                    glm::vec3 bmin, bmax;
                    ComputeBounds(fmd, bmin, bmax);
                    reg.emplace<AABB>(floor, AABB{bmin, bmax});
                }
                RigidBody frb;
                frb.shape = RigidBody::Shape::Box;
                frb.motion = RigidBody::Motion::Static;
                frb.halfExtents = {0.5f, 0.5f, 0.5f}; // scaled by the Transform above
                reg.emplace<RigidBody>(floor, frb);
            }

            // A third-person camera that follows the player.
            for (const entt::entity other : reg.view<CameraComponent>())
                reg.get<CameraComponent>(other).primary = false;
            const entt::entity cam = scene.CreateEntity("Player Camera");
            reg.emplace<Transform>(cam, Transform{{0.0f, 1.2f, 4.0f}});
            CameraComponent c;
            c.mode = CameraComponent::Mode::ThirdPerson;
            c.rotation = CameraComponent::RotationMode::SlowFollow;
            c.target = "Player";
            c.primary = true;
            // Sized + damped for a 1-unit player. The default offset (1.7m eye height)
            // and distance 6 are for a 2m human and would frame empty air above a
            // 1-unit capsule; look at its upper body from closer in. Lower damping =
            // a softer, less "whiplash" follow when the player turns to face movement.
            c.offset = {0.0f, 0.6f, 0.0f};
            c.distance = 4.0f;
            c.pitch = 12.0f;
            c.positionDamping = 5.0f;  // was 10 (snappy)
            c.rotationDamping = 3.5f;  // was 8  (snappy aim swing)
            reg.emplace<CameraComponent>(cam, c);
            selected_ = e;
        }
        if (ImGui::BeginMenu("UI")) {
            // Unity behavior: new UI elements land under a canvas - the
            // selected entity's canvas tree when there is one, else the first
            // canvas in the scene, else a freshly created one.
            const auto canvasFor = [&]() -> entt::entity {
                if (selected_ != entt::null && reg.valid(selected_)) {
                    int depth = 0;
                    for (entt::entity cur = selected_; cur != entt::null && depth < 64;
                         ++depth) {
                        if (reg.all_of<UICanvas>(cur) || reg.all_of<UIElement>(cur)) {
                            return cur; // nest under the selection's UI tree
                        }
                        const Parent* p = reg.try_get<Parent>(cur);
                        cur = (p && reg.valid(p->entity)) ? p->entity : entt::null;
                    }
                }
                for (const entt::entity e : reg.view<UICanvas>()) return e;
                const entt::entity e = scene.CreateEntity("Canvas");
                reg.emplace<UICanvas>(e);
                return e;
            };
            if (ImGui::MenuItem("Canvas")) {
                PushUndo(scene);
                const entt::entity e = scene.CreateEntity("Canvas");
                reg.emplace<UICanvas>(e);
                selected_ = e;
            }
            ImGui::Separator();
            const auto makeUI = [&](const char* name, UIElement::Type type) {
                PushUndo(scene);
                const entt::entity parent = canvasFor();
                const entt::entity e = scene.CreateEntity(name);
                reg.emplace<Parent>(e, Parent{parent});
                UIElement el;
                el.type = type;
                el.text = type == UIElement::Type::Label    ? "New Label"
                          : type == UIElement::Type::Button ? "Button"
                                                            : "";
                if (type == UIElement::Type::Panel) {
                    el.size = {500.0f, 300.0f};
                    el.color = {0.10f, 0.10f, 0.14f, 0.85f};
                } else if (type == UIElement::Type::Button) {
                    el.size = {320.0f, 90.0f};
                    el.color = {0.86f, 0.27f, 0.33f, 1.0f};
                } else if (type == UIElement::Type::Image) {
                    el.size = {256.0f, 256.0f};
                } else if (type == UIElement::Type::ProgressBar) {
                    el.size = {420.0f, 36.0f};
                    el.color = {0.12f, 0.12f, 0.16f, 0.9f};
                }
                reg.emplace<UIElement>(e, el);
                selected_ = e;
            };
            if (ImGui::MenuItem("Label")) makeUI("UI Label", UIElement::Type::Label);
            if (ImGui::MenuItem("Button")) makeUI("UI Button", UIElement::Type::Button);
            if (ImGui::MenuItem("Panel")) makeUI("UI Panel", UIElement::Type::Panel);
            if (ImGui::MenuItem("Image")) makeUI("UI Image", UIElement::Type::Image);
            if (ImGui::MenuItem("Progress Bar")) {
                makeUI("UI Progress Bar", UIElement::Type::ProgressBar);
            }
            if (ImGui::MenuItem("Progress Wheel")) {
                makeUI("UI Progress Wheel", UIElement::Type::ProgressBar);
                if (UIElement* el = reg.try_get<UIElement>(selected_)) {
                    el->radial = true;
                    el->size = {220.0f, 220.0f};
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool hasSel = (selected_ != entt::null) && reg.valid(selected_);
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button("Delete")) pendingDelete_ = selected_;
    ImGui::EndDisabled();
    ImGui::Separator();

    // Stable display order: sort by entity id (≈ creation order). entt's raw
    // storage iteration reshuffles as components/entities change, which made the
    // tree jump around mid-reparent; a fixed order keeps drag-reroute reliable.
    const auto byId = [](entt::entity a, entt::entity b) {
        return static_cast<u32>(a) < static_cast<u32>(b);
    };

    // While a level is open, make sure every entity belongs to a layer file so
    // the groups below (and Save) are correct without any per-object bookkeeping.
    EnsureLevelMembership(scene);

    // Parent -> children map for this frame's tree walk. Terrain chunks are
    // system-generated noise; keep them out of the hierarchy.
    childrenByParent_.clear();
    for (const entt::entity c : reg.view<Parent>()) {
        if (reg.try_get<TerrainChunk>(c)) continue;
        const entt::entity p = reg.get<Parent>(c).entity;
        if (reg.valid(p)) childrenByParent_[static_cast<u32>(p)].push_back(c);
    }
    for (auto& [p, kids] : childrenByParent_) std::sort(kids.begin(), kids.end(), byId);

    // Roots: every live entity without a (valid) parent. Gather then draw in the
    // fixed order (Transform-less entities - UI / lights / cameras - included).
    std::vector<entt::entity> roots;
    for (const entt::entity e : reg.view<entt::entity>()) {
        if (reg.try_get<TerrainChunk>(e)) continue; // generated terrain chunk
        const Parent* p = reg.try_get<Parent>(e);
        if (p && reg.valid(p->entity)) continue;
        roots.push_back(e);
    }
    std::sort(roots.begin(), roots.end(), byId);

    // Group roots by their source FILE (SceneSource). In a level every entity is
    // tagged with its layer file, so each composed level shows as its own Static /
    // Dynamic groups; untagged roots fall under the active scene (non-level edit).
    const std::string activeName =
        currentScenePath_.empty() ? std::string("Scene") : currentScenePath_.stem().string();
    std::vector<std::pair<std::string, std::vector<entt::entity>>> groups;
    const auto groupFor = [&](const std::string& name) -> std::vector<entt::entity>& {
        for (auto& g : groups)
            if (g.first == name) return g.second;
        groups.push_back({name, {}});
        return groups.back().second;
    };
    for (const entt::entity e : roots) {
        const SceneSource* ss = reg.try_get<SceneSource>(e);
        groupFor(ss && !ss->scene.empty() ? ss->scene : activeName).push_back(e);
    }
    // A friendly label + sort key for a group's file path.
    const auto groupLabel = [&](const std::string& key) -> std::string {
        if (key == activeName) return activeName;
        const std::filesystem::path p(key);
        if (scene::IsLevelMember(p)) {
            return scene::ResolveLevel(p).Name() +
                   (KindFromScenePath(key) == SceneKind::Static ? "  -  Static"
                                                                : "  -  Dynamic");
        }
        return p.stem().string();
    };
    const auto sortKey = [&](const std::string& key) -> std::string {
        if (key == activeName) return "0";
        const std::filesystem::path p(key);
        if (scene::IsLevelMember(p)) {
            return "1" + scene::ResolveLevel(p).Name() +
                   (KindFromScenePath(key) == SceneKind::Static ? "0" : "1");
        }
        return "2" + p.stem().string();
    };
    std::sort(groups.begin(), groups.end(),
              [&](const auto& a, const auto& b) { return sortKey(a.first) < sortKey(b.first); });

    if (groups.size() <= 1 && !levelOpen_) {
        for (const entt::entity e : roots) DrawEntityNode(scene, renderer, e);
    } else {
        for (auto& g : groups) {
            std::string disp = groupLabel(g.first);
            if (disp.empty()) disp = g.first;
            char hdr[256];
            std::snprintf(hdr, sizeof(hdr), "%s  (%zu)###grp_%s", disp.c_str(),
                          g.second.size(), g.first.c_str());
            const bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
            // Drop an entity onto a header to move it there. For a level layer file
            // this retags the whole subtree into that level + layer; for a scene it
            // re-tags the source scene (active group = untag).
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HBE_ENTITY")) {
                    entt::entity dropped;
                    std::memcpy(&dropped, p->Data, sizeof(dropped));
                    Reparent(scene, dropped, entt::null); // becomes a root
                    const std::filesystem::path gp(g.first);
                    if (scene::IsLevelMember(gp)) {
                        AssignToLevel(scene, dropped, scene::ResolveLevel(gp).base,
                                      KindFromScenePath(g.first));
                    } else {
                        MoveToScene(scene, dropped,
                                    g.first == activeName ? std::string() : g.first);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (open) {
                ImGui::Indent(8.0f);
                for (const entt::entity e : g.second) DrawEntityNode(scene, renderer, e);
                ImGui::Unindent(8.0f);
            }
        }
    }

    // Remaining empty space: drop target that unparents (move to scene root).
    ImVec2 rest = ImGui::GetContentRegionAvail();
    rest.y = rest.y < 24.0f ? 24.0f : rest.y;
    rest.x = rest.x < 1.0f ? 1.0f : rest.x;
    ImGui::Dummy(rest);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HBE_ENTITY")) {
            entt::entity dropped;
            std::memcpy(&dropped, p->Data, sizeof(dropped));
            Reparent(scene, dropped, entt::null);
        }
        // Assets dropped on empty space: meshes spawn at the scene root,
        // scenes stream in additively.
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HBE_ASSET_PATH")) {
            const std::filesystem::path src(static_cast<const char*>(p->Data));
            if (src.extension() == ".hbscene") {
                streamer_.BeginLoad(src, Project::Active().AssetsDir(),
                                    scene::LoadMode::Additive);
            } else if (src.extension() == ".uaf" &&
                       uaf::PeekType(src) == uaf::AssetType::Mesh) {
                selected_ = SpawnMeshAsset(scene, renderer, src, /*frameCamera=*/false);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Delete key removes the selection while the Hierarchy is focused.
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete) && hasSel) {
        pendingDelete_ = selected_;
    }
    // Deletions are deferred to after the tree walk (entity ids appear in
    // ImGui ids and the children map during the walk).
    if (pendingDelete_ != entt::null) {
        PushUndo(scene);
        DestroyRecursive(scene, pendingDelete_);
        pendingDelete_ = entt::null;
    }
    ImGui::End();
}

namespace {
// Collapsing component section with a right-click "Remove component" menu.
// Returns {open, removeRequested}.
struct SectionState {
    bool open = false;
    bool remove = false;
};
SectionState ComponentSection(const char* label, bool removable = true) {
    SectionState s;
    s.open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
    if (removable && ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Remove component")) s.remove = true;
        ImGui::EndPopup();
    }
    return s;
}

// Combo that picks an entity by Name (camera targets / zones / splines reference
// entities by name, which survives save/load). `requireKind`: 0 = any named
// entity, 1 = must carry a CameraComponent, 2 = must carry a CameraSpline. The
// selection is returned in `out` (the caller applies it so it can snapshot undo
// first). Returns true when the user picked a different value.
bool EntityNameCombo(const char* label, entt::registry& reg, const std::string& current,
                     int requireKind, std::string& out) {
    bool changed = false;
    const char* preview = current.empty() ? "(none)" : current.c_str();
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable("(none)", current.empty())) { out.clear(); changed = true; }
        for (const entt::entity e : reg.view<Name>()) {
            if (requireKind == 1 && !reg.all_of<CameraComponent>(e)) continue;
            if (requireKind == 2 && !reg.all_of<CameraSpline>(e)) continue;
            const std::string& n = reg.get<Name>(e).value;
            if (n.empty()) continue;
            ImGui::PushID(static_cast<int>(e));
            if (ImGui::Selectable(n.c_str(), n == current)) { out = n; changed = true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Behaviour (mode + target + distance/pitch/speed...) and rotation controls for
// a CameraComponent. Shared by the Camera inspector and a Camera Zone's inline
// override. `snap` is invoked right before a value changes so the caller can
// capture one undo step.
template <typename Snap>
void DrawCameraBehaviour(entt::registry& reg, CameraComponent& cam, Snap&& snap) {
    const auto undoOnActivate = [&] { if (ImGui::IsItemActivated()) snap(); };

    ImGui::SeparatorText("Behaviour");
    const char* modes[] = {"Static",   "First Person", "Third Person",
                           "Orbit",    "Distance",     "Spline"};
    int mode = static_cast<int>(cam.mode);
    if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes))) {
        snap();
        cam.mode = static_cast<CameraComponent::Mode>(mode);
    }
    using Mode = CameraComponent::Mode;
    const Mode m = cam.mode;
    const bool needsTarget = m != Mode::Static && m != Mode::Spline;
    if (needsTarget || m == Mode::Spline) {
        std::string pick;
        if (EntityNameCombo("Target", reg, cam.target, 0, pick)) { snap(); cam.target = pick; }
        if (m != Mode::Spline && cam.target.empty())
            ImGui::TextDisabled("No target -> behaves as Static.");
    }
    if (m == Mode::FirstPerson) {
        ImGui::DragFloat3("Eye Offset", glm::value_ptr(cam.offset), 0.02f);
        undoOnActivate();
    } else if (m == Mode::ThirdPerson || m == Mode::Orbit || m == Mode::Distance) {
        ImGui::DragFloat3("Pivot Offset", glm::value_ptr(cam.offset), 0.02f);
        undoOnActivate();
        ImGui::DragFloat("Distance", &cam.distance, 0.05f, 0.1f, 500.0f);
        undoOnActivate();
        if (m != Mode::Orbit) {
            ImGui::DragFloat("Yaw", &cam.yaw, 0.5f);
            undoOnActivate();
        }
        ImGui::DragFloat("Pitch", &cam.pitch, 0.5f, -89.0f, 89.0f);
        undoOnActivate();
        if (m == Mode::Orbit) {
            ImGui::DragFloat("Spin Speed", &cam.spinSpeed, 1.0f, -360.0f, 360.0f, "%.0f deg/s");
            undoOnActivate();
        }
        if (m != Mode::Distance) {
            ImGui::DragFloat("Follow Damping", &cam.positionDamping, 0.1f, 0.0f, 60.0f);
            undoOnActivate();
        }
        {
            bool col = cam.collide;
            if (ImGui::Checkbox("Camera collision", &col)) { snap(); cam.collide = col; }
            if (cam.collide) {
                ImGui::DragFloat("Min Distance", &cam.collisionMinDistance, 0.05f, 0.0f, 50.0f);
                undoOnActivate();
                ImGui::DragFloat("Wall Padding", &cam.collisionPadding, 0.01f, 0.0f, 5.0f);
                undoOnActivate();
            }
        }
        if (m == Mode::ThirdPerson) {
            bool pl = cam.playerLook;
            if (ImGui::Checkbox("Player look (mouse / right stick)", &pl)) {
                snap();
                cam.playerLook = pl;
            }
            if (cam.playerLook) {
                ImGui::DragFloat("Look Sensitivity", &cam.lookSensitivity, 0.005f, 0.01f, 2.0f);
                undoOnActivate();
                bool inv = cam.invertLookY;
                if (ImGui::Checkbox("Invert Y", &inv)) { snap(); cam.invertLookY = inv; }
                ImGui::TextDisabled("Orbit the camera; the character moves relative to it.");
            }
        }
    } else if (m == Mode::Spline) {
        std::string pick;
        if (EntityNameCombo("Spline", reg, cam.spline, 2, pick)) { snap(); cam.spline = pick; }
        ImGui::DragFloat("Spline Speed", &cam.splineSpeed, 0.005f, -5.0f, 5.0f);
        undoOnActivate();
        bool sl = cam.splineLoop;
        if (ImGui::Checkbox("Loop Spline", &sl)) { snap(); cam.splineLoop = sl; }
        ImGui::DragFloat("Follow Damping", &cam.positionDamping, 0.1f, 0.0f, 60.0f);
        undoOnActivate();
    }

    ImGui::SeparatorText("Rotation");
    const char* rots[] = {"Free", "Look At", "Slow Follow", "Spin", "Fixed"};
    int rot = static_cast<int>(cam.rotation);
    if (ImGui::Combo("Aim", &rot, rots, IM_ARRAYSIZE(rots))) {
        snap();
        cam.rotation = static_cast<CameraComponent::RotationMode>(rot);
    }
    using Rot = CameraComponent::RotationMode;
    if (cam.rotation == Rot::SlowFollow) {
        ImGui::DragFloat("Aim Damping", &cam.rotationDamping, 0.1f, 0.0f, 60.0f);
        undoOnActivate();
    } else if (cam.rotation == Rot::Spin) {
        ImGui::DragFloat("Spin Speed", &cam.spinSpeed, 1.0f, -360.0f, 360.0f, "%.0f deg/s");
        undoOnActivate();
        ImGui::DragFloat("Pitch", &cam.pitch, 0.5f, -89.0f, 89.0f);
        undoOnActivate();
    } else if (cam.rotation == Rot::Fixed) {
        ImGui::DragFloat3("Fixed (pitch,yaw,roll)", glm::value_ptr(cam.fixedEuler), 0.5f);
        undoOnActivate();
    }
}
} // namespace

void Editor::DrawInspector(Scene& scene, Renderer& renderer) {
    if (!panelOpen_[Panel_Inspector]) return;
    ImGui::Begin("Inspector", &panelOpen_[Panel_Inspector]);
    auto& reg = scene.Registry();

    if (selected_ == entt::null || !reg.valid(selected_)) {
        ImGui::TextDisabled("Select an entity in the Hierarchy.");
        ImGui::End();
        return;
    }
    const entt::entity sel = selected_;

    // Captures a pre-edit snapshot when a continuous widget (drag / slider /
    // color / text) is first activated; its value only changes afterwards.
    const auto undoOnActivate = [&] {
        if (ImGui::IsItemActivated()) PushUndo(scene);
    };

    // --- Name (editable) + Add Component ------------------------------------
    {
        char buf[128] = {};
        if (const Name* n = reg.try_get<Name>(sel)) {
            std::snprintf(buf, sizeof(buf), "%s", n->value.c_str());
        }
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::InputText("##name", buf, sizeof(buf))) {
            reg.emplace_or_replace<Name>(sel, Name{buf});
        }
        undoOnActivate();
        ImGui::SameLine();
        if (ImGui::Button("Add Component")) ImGui::OpenPopup("AddComponent");
    }

    // --- Level layer (Static / Dynamic) ------------------------------------
    // While a level is open, pick which layer this object (and its subtree) lives
    // in. Static = non-moving world geometry baked into the navmesh; Dynamic =
    // actors / physics. The choice tags the entity, so it sticks (the auto-router
    // only assigns untagged entities) and saves into the level's .static/.dynamic
    // file. UI lives in its own scenes, so only these two appear here.
    if (levelOpen_) {
        const SceneKind cur = EffectiveLayer(scene, sel);
        int idx = (cur == SceneKind::Dynamic) ? 1 : 0; // 0 = Static, 1 = Dynamic
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::Combo("Layer", &idx, "Static\0Dynamic\0")) {
            PushUndo(scene);
            AssignToLayer(scene, sel, idx == 1 ? SceneKind::Dynamic : SceneKind::Static);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Static = world geometry (navmesh source).\n"
                              "Dynamic = actors / physics / scripted.\n"
                              "Moves this object's subtree into that layer's file.");
        }
    }

    if (ImGui::BeginPopup("AddComponent")) {
        if (!reg.all_of<Transform>(sel) && ImGui::MenuItem("Transform")) {
            PushUndo(scene);
            reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<MeshInstance>(sel) && ImGui::BeginMenu("Mesh")) {
            const char* chosen = nullptr;
            for (const char* p : mesh::kPrimitiveNames) {
                std::string label = p;
                label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
                if (ImGui::MenuItem(label.c_str())) chosen = p;
            }
            if (chosen) {
                PushUndo(scene);
                MeshData md = mesh::GeneratePrimitive(chosen);
                MeshInstance mi;
                mi.mesh = renderer.UploadMesh(md);
                mi.baseColor = {0.8f, 0.8f, 0.82f, 1.0f};
                reg.emplace<MeshInstance>(sel, mi);
                reg.emplace_or_replace<MeshRef>(sel, MeshRef{std::string("prim:") + chosen});
                glm::vec3 bmin, bmax;
                ComputeBounds(md, bmin, bmax);
                reg.emplace_or_replace<AABB>(sel, AABB{bmin, bmax});
                if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
            }
            ImGui::EndMenu();
        }
        if (!reg.all_of<RigidBody>(sel) && ImGui::MenuItem("Rigid Body")) {
            PushUndo(scene);
            RigidBody rb;
            if (const AABB* box = reg.try_get<AABB>(sel)) {
                rb.halfExtents = glm::max((box->max - box->min) * 0.5f, glm::vec3(0.01f));
                rb.centerOffset = (box->min + box->max) * 0.5f;
                rb.radius = glm::max(rb.halfExtents.x,
                                     glm::max(rb.halfExtents.y, rb.halfExtents.z));
            }
            reg.emplace<RigidBody>(sel, rb);
        }
        if (!reg.all_of<DirectionalLightComponent>(sel) &&
            ImGui::MenuItem("Directional Light")) {
            PushUndo(scene);
            reg.emplace<DirectionalLightComponent>(sel);
        }
        if (!reg.all_of<PointLightComponent>(sel) && ImGui::MenuItem("Point Light")) {
            PushUndo(scene);
            reg.emplace<PointLightComponent>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<SpotLightComponent>(sel) && ImGui::MenuItem("Spot Light")) {
            PushUndo(scene);
            reg.emplace<SpotLightComponent>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<AnimationTrack>(sel) && ImGui::MenuItem("Animation")) {
            PushUndo(scene);
            reg.emplace<AnimationTrack>(sel);
        }
        if (!reg.all_of<AudioSource>(sel) && ImGui::MenuItem("Audio Source")) {
            PushUndo(scene);
            reg.emplace<AudioSource>(sel);
        }
        if (!reg.all_of<SchematicComponent>(sel) && ImGui::MenuItem("Schematic (Visual Script)")) {
            PushUndo(scene);
            reg.emplace<SchematicComponent>(sel);
        }
        if (!reg.all_of<Checkpoint>(sel) && ImGui::MenuItem("Checkpoint")) {
            PushUndo(scene);
            reg.emplace<Checkpoint>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<ParticleEmitter>(sel) && ImGui::MenuItem("Particle Emitter")) {
            PushUndo(scene);
            reg.emplace<ParticleEmitter>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<CameraComponent>(sel) && ImGui::MenuItem("Camera")) {
            PushUndo(scene);
            reg.emplace<CameraComponent>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<CameraZone>(sel) && ImGui::MenuItem("Camera Zone")) {
            PushUndo(scene);
            reg.emplace<CameraZone>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<CameraSpline>(sel) && ImGui::MenuItem("Camera Spline")) {
            PushUndo(scene);
            reg.emplace<CameraSpline>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<TerrainComponent>(sel) && ImGui::MenuItem("Terrain")) {
            PushUndo(scene);
            reg.emplace<TerrainComponent>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<MotionMatching>(sel) && ImGui::MenuItem("Motion Matching")) {
            PushUndo(scene);
            reg.emplace<MotionMatching>(sel);
            if (!reg.all_of<Animator>(sel)) reg.emplace<Animator>(sel); // MM drives an Animator
        }
        if (!reg.all_of<Rotator>(sel) && ImGui::MenuItem("Rotator")) {
            PushUndo(scene);
            reg.emplace<Rotator>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<CharacterController>(sel) && ImGui::MenuItem("Character Controller")) {
            PushUndo(scene);
            reg.emplace<CharacterController>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<IKConstraint>(sel) && ImGui::MenuItem("IK Constraint")) {
            PushUndo(scene);
            reg.emplace<IKConstraint>(sel);
            if (!reg.all_of<Animator>(sel)) reg.emplace<Animator>(sel); // IK poses an Animator
        }
        if (!reg.all_of<UIElement>(sel) && ImGui::MenuItem("UI Element")) {
            PushUndo(scene);
            reg.emplace<UIElement>(sel);
        }
        if (!reg.all_of<UICanvas>(sel) && ImGui::MenuItem("UI Canvas")) {
            PushUndo(scene);
            reg.emplace<UICanvas>(sel);
        }
        if (!reg.all_of<Animator>(sel) && ImGui::MenuItem("Animator")) {
            PushUndo(scene);
            reg.emplace<Animator>(sel);
        }
        if (!reg.all_of<NavigationAgent>(sel) && ImGui::MenuItem("Navigation Agent")) {
            PushUndo(scene);
            reg.emplace<NavigationAgent>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<NavigationObstacle>(sel) && ImGui::MenuItem("Navigation Obstacle")) {
            PushUndo(scene);
            reg.emplace<NavigationObstacle>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        if (!reg.all_of<NavmeshInput>(sel) && ImGui::MenuItem("Navmesh Input")) {
            PushUndo(scene);
            reg.emplace<NavmeshInput>(sel);
            if (!reg.all_of<Transform>(sel)) reg.emplace<Transform>(sel);
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();

    // --- Transform -----------------------------------------------------------
    if (Transform* t = reg.try_get<Transform>(sel)) {
        const SectionState s = ComponentSection("Transform");
        if (s.open) {
            ImGui::DragFloat3("Position", glm::value_ptr(t->position), 0.05f);
            undoOnActivate();
            // Edit rotation as Euler degrees for usability.
            glm::vec3 euler = glm::degrees(glm::eulerAngles(t->rotation));
            if (ImGui::DragFloat3("Rotation", glm::value_ptr(euler), 0.5f)) {
                t->rotation = glm::quat(glm::radians(euler));
            }
            undoOnActivate();
            ImGui::DragFloat3("Scale", glm::value_ptr(t->scale), 0.05f, 0.001f, 1000.0f);
            undoOnActivate();
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<Transform>(sel);
        }
    }

    // --- Navigation Agent ----------------------------------------------------
    if (NavigationAgent* na = reg.try_get<NavigationAgent>(sel)) {
        const SectionState s = ComponentSection("Navigation Agent");
        if (s.open) {
            ImGui::Checkbox("Has target", &na->hasTarget);
            undoOnActivate();
            ImGui::DragFloat3("Target", glm::value_ptr(na->target), 0.1f);
            undoOnActivate();
            ImGui::SliderFloat("Speed##nav", &na->speed, 0.1f, 20.0f, "%.1f");
            undoOnActivate();
            ImGui::SliderFloat("Acceleration##nav", &na->acceleration, 0.5f, 40.0f, "%.1f");
            undoOnActivate();
            ImGui::SliderFloat("Radius##nav", &na->radius, 0.1f, 3.0f, "%.2f");
            undoOnActivate();
            ImGui::SliderFloat("Stopping dist##nav", &na->stoppingDistance, 0.0f, 5.0f, "%.2f");
            undoOnActivate();
            ImGui::SliderFloat("Turn speed##nav", &na->turnSpeed, 0.0f, 30.0f, "%.1f");
            undoOnActivate();
            ImGui::Checkbox("Auto repath", &na->autoRepath);
            undoOnActivate();
            if (na->reached) {
                ImGui::TextDisabled("Reached the goal.");
            } else if (!na->path.empty()) {
                ImGui::TextDisabled("Following path (%d corners).",
                                    static_cast<int>(na->path.size()));
            }
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<NavigationAgent>(sel);
        }
    }

    // --- Navigation Obstacle -------------------------------------------------
    if (NavigationObstacle* no = reg.try_get<NavigationObstacle>(sel)) {
        const SectionState s = ComponentSection("Navigation Obstacle");
        if (s.open) {
            ImGui::Checkbox("Enabled##obs", &no->enabled);
            undoOnActivate();
            ImGui::SliderFloat("Radius##obs", &no->radius, 0.1f, 10.0f, "%.2f");
            undoOnActivate();
            ImGui::SliderFloat("Height##obs", &no->height, 0.1f, 10.0f, "%.2f");
            undoOnActivate();
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<NavigationObstacle>(sel);
        }
    }

    // --- Navmesh Input -------------------------------------------------------
    if (NavmeshInput* nin = reg.try_get<NavmeshInput>(sel)) {
        const SectionState s = ComponentSection("Navmesh Input");
        if (s.open) {
            ImGui::TextDisabled("Includes this mesh in the navmesh bake.");
            ImGui::TextDisabled("Tag floors AND blockers; untagged meshes are skipped.");
            ImGui::Checkbox("Enabled##navin", &nin->enabled);
            undoOnActivate();
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<NavmeshInput>(sel);
        }
    }

    // --- Post Volume ---------------------------------------------------------
    if (PostVolume* pv = reg.try_get<PostVolume>(sel)) {
        const SectionState s = ComponentSection("Post Volume");
        if (s.open) {
            ImGui::TextDisabled("Overrides the post LOOK while the camera is inside");
            ImGui::TextDisabled("this box. AA + GTAO stay project-global.");
            ImGui::Checkbox("Enabled##pv", &pv->enabled);
            undoOnActivate();
            ImGui::DragFloat3("Half Extents##pv", glm::value_ptr(pv->halfExtents), 0.1f, 0.1f,
                              1000.0f);
            undoOnActivate();
            ImGui::DragInt("Priority##pv", &pv->priority);
            undoOnActivate();
            ImGui::Separator();
            DrawPostLookControls(pv->settings, nullptr); // no per-volume manual exposure
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<PostVolume>(sel);
        }
    }

    if (ReflectionProbe* rp = reg.try_get<ReflectionProbe>(sel)) {
        const SectionState s = ComponentSection("Reflection Probe");
        if (s.open) {
            ImGui::TextDisabled("Bakes the room's LOCAL lighting (its walls + lamps)");
            ImGui::TextDisabled("so a sealed interior isn't lit by the sky. Applies");
            ImGui::TextDisabled("while the camera is inside the box.");
            ImGui::DragFloat3("Half Extents##rp", glm::value_ptr(rp->halfExtents), 0.1f, 0.1f,
                              1000.0f);
            undoOnActivate();
            ImGui::DragFloat("Ray range##rp", &rp->range, 0.5f, 1.0f, 500.0f, "%.0f");
            undoOnActivate();
            ImGui::SliderFloat("Sky mix##rp", &rp->skyMix, 0.0f, 1.0f, "%.2f");
            undoOnActivate();
            ImGui::DragInt("Priority##rp", &rp->priority);
            undoOnActivate();
            ImGui::Separator();
            if (ImGui::Button("Bake Probe##rp")) {
                const std::filesystem::path assets = Project::Active().AssetsDir();
                if (rp->source.empty())
                    rp->source = "Probes/probe_" +
                                 std::to_string(static_cast<u32>(entt::to_integral(sel))) +
                                 ".hbprobe";
                const glm::vec3 pos = glm::vec3(scene.WorldMatrix(sel)[3]);
                IBLMaps m = BakeLocalProbe(renderer, scene, assets, pos, rp->range, rp->skyMix, {},
                                           assets / rp->source);
                if (m.valid) {
                    rp->irradiance = m.irradiance;
                    rp->prefiltered = m.prefiltered;
                    rp->prefilteredMaxLod = m.prefilteredMaxLod;
                    rp->baked = true;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled(rp->baked ? "baked" : "not baked - press Bake");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<ReflectionProbe>(sel);
        }
    }

    // --- Mesh / material ------------------------------------------------------
    if (MeshInstance* mi = reg.try_get<MeshInstance>(sel)) {
        const SectionState s = ComponentSection("Mesh / Material");
        if (s.open) {
            // Assign / swap / clear the linked material asset (searchable picker).
            if (Project::HasActive()) {
                const MaterialRef* cur = reg.try_get<MaterialRef>(sel);
                std::string mpick;
                if (AssetPicker("Material (.hbmat)", cur ? cur->asset : std::string(),
                                ".hbmat", uaf::AssetType::Unknown, mpick)) {
                    PushUndo(scene);
                    if (mpick.empty()) {
                        reg.remove<MaterialRef>(sel);
                    } else if (engine_) {
                        ApplyMaterialToEntity(*engine_, sel,
                                              Project::Active().AssetsDir() / mpick);
                    }
                }
            }
            // Linked material asset (if any): re-apply / open / unlink.
            if (MaterialRef* mref = reg.try_get<MaterialRef>(sel)) {
                ImGui::Text("Material: %s", mref->asset.c_str());
                if (ImGui::SmallButton("Open in Asset Viewer")) {
                    SelectAsset(Project::Active().AssetsDir() / mref->asset);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Re-apply")) {
                    if (engine_) {
                        ApplyMaterialToEntity(*engine_, sel,
                                              Project::Active().AssetsDir() / mref->asset);
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Unlink")) {
                    PushUndo(scene);
                    reg.remove<MaterialRef>(sel);
                }
                ImGui::Separator();
            } else if (Project::HasActive()) {
                if (ImGui::SmallButton("Save as material asset...")) {
                    const std::filesystem::path created =
                        CreateMaterialAsset(Project::Active().AssetsDir() / "Materials");
                    if (!created.empty()) {
                        PushUndo(scene);
                        MaterialAsset mat;
                        mat.name = created.stem().string();
                        mat.baseColor = mi->baseColor;
                        mat.metallic = mi->metallic;
                        mat.roughness = mi->roughness;
                        mat.emissiveColor = mi->emissiveColor;
                        mat.emissiveIntensity = mi->emissiveIntensity;
                        mat.subsurfaceColor = mi->subsurfaceColor;
                        mat.subsurfaceRadius = mi->subsurfaceRadius;
                        mat.flags = mi->materialFlags;
                        assets::SaveMaterial(created, mat);
                        reg.emplace_or_replace<MaterialRef>(
                            sel, MaterialRef{Project::Active().RelativeAssetPath(created)});
                        RefreshAssets();
                        SelectAsset(created);
                    }
                }
                ImGui::Separator();
            }
            ImGui::ColorEdit4("Base Color", glm::value_ptr(mi->baseColor));
            undoOnActivate();
            ImGui::SliderFloat("Metallic", &mi->metallic, 0.0f, 1.0f);
            undoOnActivate();
            ImGui::SliderFloat("Roughness", &mi->roughness, 0.04f, 1.0f);
            undoOnActivate();

            ImGui::ColorEdit3("Emissive", glm::value_ptr(mi->emissiveColor));
            undoOnActivate();
            if (mi->emissiveColor != glm::vec3(0.0f)) {
                ImGui::DragFloat("Emissive Intensity", &mi->emissiveIntensity, 0.05f,
                                 0.0f, 100.0f);
                undoOnActivate();
            }

            bool sss = (mi->materialFlags & rhi::MaterialFlag_Subsurface) != 0u;
            if (ImGui::Checkbox("Subsurface (skin)", &sss)) {
                PushUndo(scene); // flags are written below, after the snapshot
                if (sss) mi->materialFlags |= rhi::MaterialFlag_Subsurface;
                else     mi->materialFlags &= ~static_cast<u32>(rhi::MaterialFlag_Subsurface);
            }
            if (sss) {
                ImGui::ColorEdit3("Subsurface Color", glm::value_ptr(mi->subsurfaceColor));
                undoOnActivate();
                ImGui::SliderFloat("Scatter radius", &mi->subsurfaceRadius, 0.1f, 4.0f, "%.2f");
                undoOnActivate();
            }
            const auto flagToggle = [&](const char* label, u32 bit) {
                bool on = (mi->materialFlags & bit) != 0u;
                if (ImGui::Checkbox(label, &on)) {
                    PushUndo(scene);
                    if (on) mi->materialFlags |= bit;
                    else    mi->materialFlags &= ~bit;
                }
            };
            flagToggle("Cloth (fabric sheen)", rhi::MaterialFlag_Cloth);
            flagToggle("Eye (parallax iris)", rhi::MaterialFlag_Eye);
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<MeshInstance>(sel);
            reg.remove<AABB>(sel); // picking bounds belong to the mesh
        }
    }

    // --- Rigid body ------------------------------------------------------------
    if (RigidBody* rb = reg.try_get<RigidBody>(sel)) {
        const SectionState s = ComponentSection("Rigid Body");
        if (s.open) {
            bool changed = false;
            // Order matches RigidBody::Shape (Box,Sphere,Capsule,Mesh,ConvexHull).
            int shape = static_cast<int>(rb->shape);
            if (ImGui::Combo("Shape", &shape, "Box\0Sphere\0Capsule\0Mesh\0Convex Hull\0")) {
                PushUndo(scene);
                rb->shape = static_cast<RigidBody::Shape>(shape);
                // Mesh / Convex Hull: pull the exact geometry from this entity's mesh.
                if (rb->shape == RigidBody::Shape::Mesh ||
                    rb->shape == RigidBody::Shape::ConvexHull) {
                    if (const MeshData* md = GetCpuMesh(scene, sel)) {
                        rb->collisionVertices.clear();
                        rb->collisionVertices.reserve(md->vertices.size());
                        for (const auto& v : md->vertices)
                            rb->collisionVertices.push_back(v.position);
                        rb->collisionIndices = md->indices;
                        // Mesh verts are already local-space positioned; the box
                        // AABB-centre offset must not shift them (would misalign).
                        rb->centerOffset = glm::vec3(0.0f);
                    }
                }
                changed = true;
            }
            int motion = rb->motion == RigidBody::Motion::Dynamic ? 1 : 0;
            if (ImGui::Combo("Motion", &motion, "Static\0Dynamic\0")) {
                PushUndo(scene);
                rb->motion = motion == 1 ? RigidBody::Motion::Dynamic
                                         : RigidBody::Motion::Static;
                changed = true;
            }
            if (rb->shape == RigidBody::Shape::Sphere) {
                changed |= ImGui::DragFloat("Radius", &rb->radius, 0.02f, 0.01f, 1000.0f);
                undoOnActivate();
            } else if (rb->shape == RigidBody::Shape::Capsule) {
                changed |= ImGui::DragFloat("Radius", &rb->radius, 0.02f, 0.01f, 1000.0f);
                undoOnActivate();
                changed |= ImGui::DragFloat("Half Height", &rb->halfHeight, 0.02f, 0.0f, 1000.0f);
                undoOnActivate();
            } else if (rb->shape == RigidBody::Shape::Mesh ||
                       rb->shape == RigidBody::Shape::ConvexHull) {
                ImGui::TextDisabled("Collider built from the mesh (%zu verts).",
                                    rb->collisionVertices.size());
                if (rb->shape == RigidBody::Shape::Mesh &&
                    rb->motion == RigidBody::Motion::Dynamic) {
                    ImGui::TextDisabled("Dynamic uses a convex hull (triangle mesh = static).");
                }
                if (ImGui::Button("Rebuild From Mesh")) {
                    if (const MeshData* md = GetCpuMesh(scene, sel)) {
                        PushUndo(scene);
                        rb->collisionVertices.clear();
                        rb->collisionVertices.reserve(md->vertices.size());
                        for (const auto& v : md->vertices)
                            rb->collisionVertices.push_back(v.position);
                        rb->collisionIndices = md->indices;
                        rb->centerOffset = glm::vec3(0.0f);
                        changed = true;
                    }
                }
            } else {
                changed |= ImGui::DragFloat3("Half Extents", glm::value_ptr(rb->halfExtents),
                                             0.02f, 0.01f, 1000.0f);
                undoOnActivate();
            }
            // Mesh/ConvexHull colliders carry their position in the vertices, so
            // the offset is inert for them and only shown for primitive shapes.
            if (rb->shape != RigidBody::Shape::Mesh &&
                rb->shape != RigidBody::Shape::ConvexHull) {
                changed |= ImGui::DragFloat3("Center Offset", glm::value_ptr(rb->centerOffset),
                                             0.02f);
                undoOnActivate();
            }
            changed |= ImGui::SliderFloat("Friction", &rb->friction, 0.0f, 1.0f);
            undoOnActivate();
            changed |= ImGui::SliderFloat("Restitution", &rb->restitution, 0.0f, 1.0f);
            undoOnActivate();
            // Property edits rebuild the body (PhysicsWorld reaps the old one).
            if (changed) rb->bodyId = RigidBody::kInvalidBody;
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<RigidBody>(sel);
        }
    }

    // --- Audio source -----------------------------------------------------------
    if (AudioSource* src = reg.try_get<AudioSource>(sel)) {
        const SectionState s = ComponentSection("Audio Source");
        if (s.open) {
            std::string apick;
            if (AssetPicker("Audio (.uaf)", src->asset, ".uaf", uaf::AssetType::Audio, apick)) {
                PushUndo(scene);
                src->asset = apick;
                src->voiceId = AudioSource::kNoVoice; // re-create with the new asset
            }
            AssetDropTarget(".uaf", uaf::AssetType::Audio,
                            [&](const std::filesystem::path& dropped) {
                                PushUndo(scene);
                                src->asset = Project::Active().RelativeAssetPath(dropped);
                                src->voiceId = AudioSource::kNoVoice;
                            });
            // Mixer bus routing (bus list = Master + the project's buses).
            if (ImGui::BeginCombo("Bus", src->bus.empty() ? "Master" : src->bus.c_str())) {
                if (ImGui::Selectable("Master", src->bus.empty() || src->bus == "Master")) {
                    PushUndo(scene);
                    src->bus = "Master";
                    src->voiceId = AudioSource::kNoVoice;
                }
                if (Project::HasActive()) {
                    for (const AudioBusSetting& b : Project::Active().Settings().audioBuses) {
                        if (ImGui::Selectable(b.name.c_str(), b.name == src->bus)) {
                            PushUndo(scene);
                            src->bus = b.name;
                            src->voiceId = AudioSource::kNoVoice; // re-route
                        }
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SliderFloat("Volume", &src->volume, 0.0f, 2.0f);
            undoOnActivate();
            ImGui::DragFloat("Min Distance", &src->minDistance, 0.1f, 0.01f, 1000.0f);
            undoOnActivate();
            ImGui::DragFloat("Max Distance", &src->maxDistance, 0.5f, 0.1f, 5000.0f);
            undoOnActivate();
            bool loop = src->loop;
            if (ImGui::Checkbox("Loop", &loop)) {
                PushUndo(scene);
                src->loop = loop;
            }
            ImGui::SameLine();
            bool autoplay = src->autoplay;
            if (ImGui::Checkbox("Autoplay", &autoplay)) {
                PushUndo(scene);
                src->autoplay = autoplay;
            }
            ImGui::SameLine();
            if (ImGui::Button(src->playing ? "Stop" : "Play")) {
                src->playing = !src->playing; // runtime state; not undoable
            }
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<AudioSource>(sel);
        }
    }

    // --- Particle Emitter ----------------------------------------------------
    if (ParticleEmitter* pe = reg.try_get<ParticleEmitter>(sel)) {
        const SectionState s = ComponentSection("Particle Emitter");
        if (s.open) {
            ImGui::TextDisabled("Live: %d / %u", static_cast<int>(pe->pool.size()),
                                pe->maxParticles);
            bool emit = pe->emitting;
            if (ImGui::Checkbox("Emitting", &emit)) { PushUndo(scene); pe->emitting = emit; }
            ImGui::SameLine();
            bool add = pe->additive;
            if (ImGui::Checkbox("Additive", &add)) { PushUndo(scene); pe->additive = add; }
            ImGui::DragFloat("Rate (/s)", &pe->rate, 0.5f, 0.0f, 4000.0f); undoOnActivate();
            int maxp = static_cast<int>(pe->maxParticles);
            if (ImGui::DragInt("Max", &maxp, 8, 1, 200000)) {
                PushUndo(scene);
                pe->maxParticles = static_cast<u32>(glm::max(1, maxp));
            }
            ImGui::DragFloat("Lifetime", &pe->lifetime, 0.05f, 0.05f, 60.0f); undoOnActivate();
            ImGui::SliderFloat("Life variance", &pe->lifetimeVariance, 0.0f, 1.0f); undoOnActivate();
            ImGui::SeparatorText("Spawn");
            ImGui::DragFloat3("Direction", glm::value_ptr(pe->direction), 0.01f); undoOnActivate();
            ImGui::DragFloat("Speed", &pe->startSpeed, 0.05f, 0.0f, 200.0f); undoOnActivate();
            ImGui::SliderFloat("Speed variance", &pe->speedVariance, 0.0f, 1.0f); undoOnActivate();
            ImGui::SliderFloat("Spread", &pe->spread, 0.0f, 1.0f); undoOnActivate();
            ImGui::DragFloat("Emit radius", &pe->emitRadius, 0.02f, 0.0f, 50.0f); undoOnActivate();
            ImGui::SeparatorText("Motion");
            ImGui::DragFloat3("Gravity", glm::value_ptr(pe->gravity), 0.05f); undoOnActivate();
            ImGui::SliderFloat("Drag", &pe->drag, 0.0f, 8.0f); undoOnActivate();
            ImGui::SeparatorText("Look (over life)");
            ImGui::ColorEdit4("Start color", glm::value_ptr(pe->startColor),
                              ImGuiColorEditFlags_AlphaBar);
            ImGui::ColorEdit4("End color", glm::value_ptr(pe->endColor),
                              ImGuiColorEditFlags_AlphaBar);
            ImGui::DragFloat("Start size", &pe->startSize, 0.01f, 0.0f, 50.0f); undoOnActivate();
            ImGui::DragFloat("End size", &pe->endSize, 0.01f, 0.0f, 50.0f); undoOnActivate();
            ImGui::DragFloat("Spin (rad/s)", &pe->spin, 0.05f, -20.0f, 20.0f); undoOnActivate();
            std::string tpick;
            if (AssetPicker("Sprite (.uaf)", pe->texture, ".uaf", uaf::AssetType::Texture,
                            tpick, "(soft dot)")) {
                PushUndo(scene);
                pe->texture = tpick;
                pe->textureResolved = false;
            }
            AssetDropTarget(".uaf", uaf::AssetType::Texture,
                            [&](const std::filesystem::path& dropped) {
                                PushUndo(scene);
                                pe->texture = Project::Active().RelativeAssetPath(dropped);
                                pe->textureResolved = false;
                            });
        }
        if (s.remove) { PushUndo(scene); reg.remove<ParticleEmitter>(sel); }
    }

    // --- Camera --------------------------------------------------------------
    if (CameraComponent* cam = reg.try_get<CameraComponent>(sel)) {
        const SectionState s = ComponentSection("Camera");
        if (s.open) {
            ImGui::SliderFloat("Field of View", &cam->fovY, 10.0f, 140.0f, "%.0f deg");
            undoOnActivate();
            ImGui::DragFloat("Near", &cam->nearZ, 0.01f, 0.001f, 100.0f);
            undoOnActivate();
            ImGui::DragFloat("Far", &cam->farZ, 1.0f, 1.0f, 100000.0f);
            undoOnActivate();
            bool primary = cam->primary;
            if (ImGui::Checkbox("Primary (renders in play mode)", &primary)) {
                PushUndo(scene);
                cam->primary = primary;
                if (primary) {
                    // One primary at a time: demote the others.
                    for (const entt::entity other : reg.view<CameraComponent>()) {
                        if (other != sel) reg.get<CameraComponent>(other).primary = false;
                    }
                }
            }

            // Behaviour preset + rotation (shared with Camera Zone overrides).
            DrawCameraBehaviour(reg, *cam, [&] { PushUndo(scene); });
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<CameraComponent>(sel);
        }
    }

    // --- Camera Zone (trigger volume that switches the active camera) ---------
    if (CameraZone* z = reg.try_get<CameraZone>(sel)) {
        const SectionState s = ComponentSection("Camera Zone");
        if (s.open) {
            ImGui::DragFloat3("Half Extents", glm::value_ptr(z->halfExtents), 0.1f, 0.01f, 1000.0f);
            undoOnActivate();
            std::string pick;
            if (EntityNameCombo("Track Entity", reg, z->track, 0, pick)) {
                PushUndo(scene);
                z->track = pick;
            }
            ImGui::DragInt("Priority", &z->priority, 0.1f);
            undoOnActivate();
            bool en = z->enabled;
            if (ImGui::Checkbox("Enabled", &en)) { PushUndo(scene); z->enabled = en; }

            // Source of the camera this zone switches to: an inline override
            // (mode/speed/distance/fov set right here) or a referenced camera.
            ImGui::SeparatorText("Switch To");
            bool useSettings = z->useSettings;
            if (ImGui::Checkbox("Override camera (set mode + speed here)", &useSettings)) {
                PushUndo(scene);
                z->useSettings = useSettings;
            }
            if (z->useSettings) {
                ImGui::SliderFloat("Field of View", &z->settings.fovY, 10.0f, 140.0f,
                                   "%.0f deg");
                undoOnActivate();
                DrawCameraBehaviour(reg, z->settings, [&] { PushUndo(scene); });
                if (z->settings.target.empty()) {
                    ImGui::TextDisabled("Target empty -> follows the base camera's target.");
                }
            } else {
                if (EntityNameCombo("Camera", reg, z->camera, 1, pick)) {
                    PushUndo(scene);
                    z->camera = pick;
                }
                ImGui::TextDisabled("Switches to this camera entity when entered.");
            }

            ImGui::Separator();
            ImGui::TextDisabled(z->active ? "Status: ACTIVE (tracked entity inside)"
                                          : "Status: inactive");
            ImGui::TextDisabled("Box uses this entity's Transform (move/rotate/scale).");
            ImGui::TextDisabled("Track empty -> uses the camera's own target.");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<CameraZone>(sel);
        }
    }

    // --- Camera Spline (path for Spline-mode cameras) ------------------------
    if (CameraSpline* sp = reg.try_get<CameraSpline>(sel)) {
        const SectionState s = ComponentSection("Camera Spline");
        if (s.open) {
            bool lp = sp->loop;
            if (ImGui::Checkbox("Loop", &lp)) { PushUndo(scene); sp->loop = lp; }
            ImGui::TextDisabled("Pick a point, move it with the gizmo in the viewport.");
            ImGui::TextDisabled("Tab extends the active end; click a point to select it.");
            ImGui::Text("Control points: %zu", sp->points.size());
            int removeAt = -1;
            for (usize i = 0; i < sp->points.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                // Radio = the active point (the gizmo target / Tab-extend anchor).
                if (ImGui::RadioButton("##sel", splinePoint_ == static_cast<int>(i)))
                    splinePoint_ = static_cast<int>(i);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-30.0f);
                ImGui::DragFloat3("##pt", glm::value_ptr(sp->points[i]), 0.05f);
                undoOnActivate();
                if (ImGui::IsItemActive()) splinePoint_ = static_cast<int>(i);
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) removeAt = static_cast<int>(i);
                ImGui::PopID();
            }
            if (removeAt >= 0) {
                PushUndo(scene);
                sp->points.erase(sp->points.begin() + removeAt);
                if (splinePoint_ >= static_cast<int>(sp->points.size()))
                    splinePoint_ = static_cast<int>(sp->points.size()) - 1;
            }
            if (ImGui::Button("Extend Start")) ExtendSpline(scene, *sp, true);
            ImGui::SameLine();
            if (ImGui::Button("Extend End")) ExtendSpline(scene, *sp, false);
            ImGui::SameLine();
            if (ImGui::Button("Add at Camera")) {
                PushUndo(scene);
                sp->points.push_back(renderer.GetCamera().Position());
                splinePoint_ = static_cast<int>(sp->points.size()) - 1;
            }
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<CameraSpline>(sel);
        }
    }

    // --- Terrain (chunked heightfield) ---------------------------------------
    if (TerrainComponent* tr = reg.try_get<TerrainComponent>(sel)) {
        const SectionState s = ComponentSection("Terrain");
        if (s.open) {
            bool rebuild = false;
            const auto edited = [&] { if (ImGui::IsItemDeactivatedAfterEdit()) rebuild = true; };
            int chunks = static_cast<int>(tr->chunks);
            if (ImGui::SliderInt("Chunks / side", &chunks, 1, 16)) tr->chunks = static_cast<u32>(chunks);
            undoOnActivate(); edited();
            int res = static_cast<int>(tr->resolution);
            if (ImGui::SliderInt("Resolution", &res, 2, 96)) tr->resolution = static_cast<u32>(res);
            undoOnActivate(); edited();
            ImGui::DragFloat("Chunk Size", &tr->chunkSize, 0.5f, 1.0f, 256.0f);
            undoOnActivate(); edited();
            ImGui::DragFloat("Height", &tr->height, 0.1f, 0.0f, 200.0f);
            undoOnActivate(); edited();
            ImGui::DragFloat("Frequency", &tr->frequency, 0.001f, 0.001f, 1.0f, "%.4f");
            undoOnActivate(); edited();
            int oct = static_cast<int>(tr->octaves);
            if (ImGui::SliderInt("Octaves", &oct, 1, 8)) tr->octaves = static_cast<u32>(oct);
            undoOnActivate(); edited();
            ImGui::DragInt("Seed", &tr->seed);
            undoOnActivate(); edited();
            ImGui::ColorEdit4("Color", glm::value_ptr(tr->color));
            undoOnActivate(); edited();
            ImGui::SliderFloat("Roughness", &tr->roughness, 0.0f, 1.0f);
            undoOnActivate(); edited();
            if (ImGui::Button("Rebuild Terrain")) {
                tr->heights.clear(); // re-seed procedurally from the params
                rebuild = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%u x %u chunks", glm::clamp(tr->chunks, 1u, 16u),
                                glm::clamp(tr->chunks, 1u, 16u));
            if (rebuild) tr->dirty = true; // the terrain system regenerates chunks

            // --- Sculpt tool ---------------------------------------------------
            ImGui::SeparatorText("Sculpt");
            ImGui::Checkbox("Sculpt Mode", &terrainSculpt_);
            if (terrainSculpt_) {
                ImGui::TextDisabled("Drag LMB on the terrain in the Scene view.");
                const char* brushes[] = {"Raise", "Lower", "Smooth", "Flatten"};
                ImGui::Combo("Brush", &terrainBrush_, brushes, IM_ARRAYSIZE(brushes));
                ImGui::SliderFloat("Radius", &terrainRadius_, 0.5f, 50.0f, "%.1f");
                ImGui::SliderFloat("Strength", &terrainStrength_, 0.1f, 30.0f, "%.1f");
            }
        }
        if (s.remove) {
            PushUndo(scene);
            // Destroy this terrain's generated chunk children too.
            std::vector<entt::entity> kill;
            for (const entt::entity c : reg.view<TerrainChunk, Parent>()) {
                if (reg.get<Parent>(c).entity == sel) kill.push_back(c);
            }
            for (const entt::entity c : kill) reg.destroy(c);
            reg.remove<TerrainComponent>(sel);
        }
    }

    // --- Motion Matching (data-driven locomotion) ----------------------------
    if (MotionMatching* mm = reg.try_get<MotionMatching>(sel)) {
        const SectionState s = ComponentSection("Motion Matching");
        if (s.open) {
            if (!reg.all_of<Animator>(sel)) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 80, 255));
                ImGui::TextWrapped("Requires an Animator component to drive.");
                ImGui::PopStyleColor();
            }
            // Source asset: a mesh .uaf with locomotion clips ("" = the
            // Animator's own source).
            std::string mmpick;
            if (AssetPicker("Clips Source", mm->sourceAsset, ".uaf", uaf::AssetType::Mesh,
                            mmpick, "(Animator's own)")) {
                PushUndo(scene);
                mm->sourceAsset = mmpick;
            }
            ImGui::DragFloat("Search Interval", &mm->searchInterval, 0.01f, 0.02f, 1.0f);
            undoOnActivate();
            ImGui::DragFloat("Speed Scale", &mm->speedScale, 0.01f, 0.01f, 20.0f);
            undoOnActivate();
            bool nav = mm->useNavVelocity;
            if (ImGui::Checkbox("Use Nav Agent velocity", &nav)) {
                PushUndo(scene);
                mm->useNavVelocity = nav;
            }
            if (!mm->useNavVelocity) {
                ImGui::DragFloat3("Desired Velocity", glm::value_ptr(mm->desiredVelocity), 0.05f);
                undoOnActivate();
            }
            bool en = mm->enabled;
            if (ImGui::Checkbox("Enabled", &en)) { PushUndo(scene); mm->enabled = en; }
            ImGui::TextDisabled("Auto-selects idle/walk/run by speed (play mode).");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<MotionMatching>(sel);
        }
    }

    // --- Rotator -------------------------------------------------------------
    if (Rotator* ro = reg.try_get<Rotator>(sel)) {
        const SectionState s = ComponentSection("Rotator");
        if (s.open) {
            ImGui::DragFloat3("Axis", glm::value_ptr(ro->axis), 0.05f);
            undoOnActivate();
            ImGui::DragFloat("Speed (deg/s)", &ro->speed, 1.0f);
            undoOnActivate();
            bool en = ro->enabled;
            if (ImGui::Checkbox("Enabled", &en)) { PushUndo(scene); ro->enabled = en; }
            ImGui::TextDisabled("Spins the entity while the simulation runs.");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<Rotator>(sel);
        }
    }

    // --- Character Controller (player movement) ------------------------------
    if (CharacterController* cc = reg.try_get<CharacterController>(sel)) {
        const SectionState s = ComponentSection("Character Controller");
        if (s.open) {
            ImGui::DragFloat("Capsule Radius", &cc->radius, 0.02f, 0.05f, 5.0f, "%.2f m");
            undoOnActivate();
            ImGui::DragFloat("Capsule Height", &cc->height, 0.05f, 0.1f, 10.0f, "%.2f m");
            undoOnActivate();
            ImGui::DragFloat("Move Speed", &cc->moveSpeed, 0.1f, 0.0f, 100.0f, "%.1f m/s");
            undoOnActivate();
            ImGui::DragFloat("Sprint x", &cc->sprintMultiplier, 0.05f, 1.0f, 8.0f);
            undoOnActivate();
            ImGui::DragFloat("Jump Height", &cc->jumpHeight, 0.05f, 0.0f, 20.0f, "%.2f m");
            undoOnActivate();
            ImGui::DragFloat("Gravity", &cc->gravity, 0.5f, 0.0f, 100.0f);
            undoOnActivate();
            ImGui::DragFloat("Turn Speed", &cc->turnSpeed, 0.5f, 0.0f, 60.0f);
            undoOnActivate();
            bool cr = cc->cameraRelative;
            if (ImGui::Checkbox("Camera-relative", &cr)) { PushUndo(scene); cc->cameraRelative = cr; }
            bool fm = cc->faceMoveDir;
            if (ImGui::Checkbox("Face move direction", &fm)) { PushUndo(scene); cc->faceMoveDir = fm; }
            bool kb = cc->useKeyboard;
            if (ImGui::Checkbox("Keyboard (WASD)", &kb)) { PushUndo(scene); cc->useKeyboard = kb; }
            ImGui::SameLine();
            bool gp = cc->useGamepad;
            if (ImGui::Checkbox("Gamepad", &gp)) { PushUndo(scene); cc->useGamepad = gp; }
            bool en = cc->enabled;
            if (ImGui::Checkbox("Enabled", &en)) { PushUndo(scene); cc->enabled = en; }
            ImGui::TextDisabled("Moves while playing. Point a camera's Target at this\n"
                                "entity (Third/First Person) to control the view.");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<CharacterController>(sel);
        }
    }

    // --- IK Constraint (two-bone inverse kinematics) -------------------------
    if (IKConstraint* ik = reg.try_get<IKConstraint>(sel)) {
        const SectionState s = ComponentSection("IK Constraint");
        if (s.open) {
            if (!reg.all_of<Animator>(sel)) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 80, 255));
                ImGui::TextWrapped("Needs an Animator + skinned mesh to pose.");
                ImGui::PopStyleColor();
            }
            ImGui::TextDisabled("Each chain bends end joint + parent + grandparent.");
            int removeAt = -1;
            for (usize i = 0; i < ik->chains.size(); ++i) {
                IKChain& ch = ik->chains[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Separator();
                char nameBuf[96];
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", ch.endJoint.c_str());
                if (ImGui::InputText("End Joint", nameBuf, sizeof(nameBuf))) ch.endJoint = nameBuf;
                undoOnActivate();
                std::string pick;
                if (EntityNameCombo("Target Entity", reg, ch.targetEntity, 0, pick)) {
                    PushUndo(scene);
                    ch.targetEntity = pick;
                }
                if (ch.targetEntity.empty()) {
                    ImGui::DragFloat3("Target", glm::value_ptr(ch.target), 0.05f);
                    undoOnActivate();
                }
                bool hp = ch.hasPole;
                if (ImGui::Checkbox("Pole hint", &hp)) { PushUndo(scene); ch.hasPole = hp; }
                if (ch.hasPole) {
                    ImGui::DragFloat3("Pole", glm::value_ptr(ch.pole), 0.05f);
                    undoOnActivate();
                }
                ImGui::SliderFloat("Weight", &ch.weight, 0.0f, 1.0f, "%.2f");
                undoOnActivate();
                bool en = ch.enabled;
                if (ImGui::Checkbox("Enabled##ik", &en)) { PushUndo(scene); ch.enabled = en; }
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove Chain")) removeAt = static_cast<int>(i);
                ImGui::PopID();
            }
            if (removeAt >= 0) {
                PushUndo(scene);
                ik->chains.erase(ik->chains.begin() + removeAt);
            }
            if (ImGui::Button("Add Chain")) {
                PushUndo(scene);
                ik->chains.push_back(IKChain{});
            }
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<IKConstraint>(sel);
        }
    }

    // --- Animator (skeletal animation) ----------------------------------------
    if (Animator* an = reg.try_get<Animator>(sel)) {
        const SectionState s = ComponentSection("Animator");
        if (s.open) {
            // Resolve the rigs involved (cached loads; cheap per frame).
            const std::filesystem::path assetsDir =
                Project::HasActive() ? Project::Active().AssetsDir()
                                     : std::filesystem::path();
            std::string ownRel;
            if (const MeshRef* mr = reg.try_get<MeshRef>(sel);
                mr && mr->source.rfind("uaf:", 0) == 0) {
                const std::string rest = mr->source.substr(4);
                const auto hash = rest.find_last_of('#');
                ownRel = hash == std::string::npos ? rest : rest.substr(0, hash);
            }
            const auto targetRig = anim::LoadRigCached(assetsDir, ownRel);
            auto sourceRig = an->sourceAsset.empty()
                                 ? targetRig
                                 : anim::LoadRigCached(assetsDir, an->sourceAsset);
            if (!sourceRig || sourceRig->clips.empty()) sourceRig = targetRig;

            if (!targetRig || !targetRig->Valid()) {
                ImGui::TextDisabled("This entity's mesh asset has no skeleton.");
            } else {
                ImGui::TextDisabled("%zu joints", targetRig->skeleton.joints.size());

                // Retarget source: clips can come from any rigged mesh asset.
                std::string anpick;
                if (AssetPicker("Clip Source", an->sourceAsset, ".uaf", uaf::AssetType::Mesh,
                                anpick, "(own clips)")) {
                    PushUndo(scene);
                    an->sourceAsset = anpick;
                    an->clip = 0;
                    an->time = 0.0f;
                }
                if (!an->sourceAsset.empty()) {
                    ImGui::TextDisabled("Retargeting by joint name (scale %.2f).",
                                        an->translationScale);
                }

                if (sourceRig && !sourceRig->clips.empty()) {
                    an->clip = glm::clamp(
                        an->clip, 0, static_cast<i32>(sourceRig->clips.size()) - 1);
                    const AnimationClip& clip =
                        sourceRig->clips[static_cast<usize>(an->clip)];
                    if (ImGui::BeginCombo("Clip", clip.name.c_str())) {
                        for (i32 c = 0; c < static_cast<i32>(sourceRig->clips.size());
                             ++c) {
                            if (ImGui::Selectable(
                                    sourceRig->clips[static_cast<usize>(c)].name.c_str(),
                                    c == an->clip)) {
                                PushUndo(scene);
                                an->clip = c;
                                an->time = 0.0f;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::Button(an->playing ? "Pause" : "Play")) {
                        an->playing = !an->playing; // runtime state; not undoable
                    }
                    ImGui::SameLine();
                    bool loop = an->loop;
                    if (ImGui::Checkbox("Loop", &loop)) {
                        PushUndo(scene);
                        an->loop = loop;
                    }
                    ImGui::SameLine();
                    bool rootMotion = an->rootMotion;
                    if (ImGui::Checkbox("Root Motion", &rootMotion)) {
                        PushUndo(scene);
                        an->rootMotion = rootMotion;
                        an->rootTrackValid = false;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "The clip's horizontal root travel moves this entity's\n"
                            "Transform; the character animates in place.");
                    }
                    ImGui::SliderFloat("Speed", &an->speed, -2.0f, 3.0f);
                    undoOnActivate();
                    ImGui::SliderFloat("Time", &an->time, 0.0f,
                                       glm::max(clip.duration, 0.001f), "%.2f s");
                } else {
                    ImGui::TextDisabled("No clips in the source asset.");
                }
            }
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<Animator>(sel);
        }
    }

    // --- UI element ------------------------------------------------------------
    if (UIElement* el = reg.try_get<UIElement>(sel)) {
        const SectionState s = ComponentSection("UI Element");
        if (s.open) {
            int type = static_cast<int>(el->type);
            if (ImGui::Combo("Type", &type,
                             "Panel\0Label\0Button\0Image\0Progress Bar\0")) {
                PushUndo(scene);
                el->type = static_cast<UIElement::Type>(type);
            }
            if (el->type != UIElement::Type::Image) {
                char textBuf[256];
                std::snprintf(textBuf, sizeof(textBuf), "%s", el->text.c_str());
                if (ImGui::InputText("Text", textBuf, sizeof(textBuf))) {
                    el->text = textBuf;
                }
                undoOnActivate();
                ImGui::DragFloat("Text Size", &el->textSize, 0.5f, 6.0f, 300.0f,
                                 "%.0f px");
                undoOnActivate();
                // Caption alignment within the element rect.
                int hAlign = static_cast<int>(el->hAlign);
                if (ImGui::Combo("H Align", &hAlign, "Left\0Center\0Right\0")) {
                    PushUndo(scene);
                    el->hAlign = static_cast<UIElement::HAlign>(hAlign);
                }
                int vAlign = static_cast<int>(el->vAlign);
                if (ImGui::Combo("V Align", &vAlign, "Top\0Center\0Bottom\0")) {
                    PushUndo(scene);
                    el->vAlign = static_cast<UIElement::VAlign>(vAlign);
                }
                // Font asset picker (imported .ttf/.otf; default = system font).
                std::string fpick;
                if (AssetPicker("Font", el->font, ".uaf", uaf::AssetType::Font, fpick,
                                "(default)")) {
                    PushUndo(scene);
                    el->font = fpick;
                }
                AssetDropTarget(".uaf", uaf::AssetType::Font,
                                [&](const std::filesystem::path& src) {
                                    PushUndo(scene);
                                    el->font = Project::Active().RelativeAssetPath(src);
                                });
            }
            // Fit to parent: the element always covers its parent rect.
            bool fullscreen = el->fullscreen;
            if (ImGui::Checkbox("Fit to Parent", &fullscreen)) {
                PushUndo(scene);
                el->fullscreen = fullscreen;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Covers its entire parent rect (the whole canvas\n"
                                  "for top-level elements); the RectTransform below\n"
                                  "is ignored.");
            }

            if (!el->fullscreen) {
                // Anchor presets (Unity-style): a 3x3 point grid plus stretch
                // presets. Point presets keep the size; stretch presets zero
                // the affected axis' sizeDelta and center the offset.
                ImGui::TextDisabled("Anchor presets");
                static constexpr const char* kPreset[3][3] = {
                    {"TL", "T", "TR"}, {"L", "C", "R"}, {"BL", "B", "BR"}};
                for (int row = 0; row < 3; ++row) {
                    for (int col = 0; col < 3; ++col) {
                        if (col > 0) ImGui::SameLine();
                        ImGui::PushID(row * 3 + col);
                        if (ImGui::SmallButton(kPreset[row][col])) {
                            PushUndo(scene);
                            el->anchorMin = el->anchorMax = {col * 0.5f, row * 0.5f};
                            el->pivot = {col * 0.5f, row * 0.5f};
                            el->offset = {0.0f, 0.0f};
                        }
                        ImGui::PopID();
                    }
                }
                if (ImGui::SmallButton("Stretch H")) {
                    PushUndo(scene);
                    el->anchorMin.x = 0.0f;
                    el->anchorMax.x = 1.0f;
                    el->size.x = 0.0f;
                    el->offset.x = 0.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Stretch V")) {
                    PushUndo(scene);
                    el->anchorMin.y = 0.0f;
                    el->anchorMax.y = 1.0f;
                    el->size.y = 0.0f;
                    el->offset.y = 0.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Stretch All")) {
                    PushUndo(scene);
                    el->anchorMin = {0.0f, 0.0f};
                    el->anchorMax = {1.0f, 1.0f};
                    el->size = {0.0f, 0.0f};
                    el->offset = {0.0f, 0.0f};
                }
                ImGui::SliderFloat2("Anchor Min", glm::value_ptr(el->anchorMin), 0.0f, 1.0f);
                undoOnActivate();
                ImGui::SliderFloat2("Anchor Max", glm::value_ptr(el->anchorMax), 0.0f, 1.0f);
                undoOnActivate();
                ImGui::SliderFloat2("Pivot", glm::value_ptr(el->pivot), 0.0f, 1.0f);
                undoOnActivate();
                ImGui::DragFloat2("Offset", glm::value_ptr(el->offset), 1.0f);
                undoOnActivate();
                if (el->type != UIElement::Type::Label) {
                    ImGui::DragFloat2("Size", glm::value_ptr(el->size), 1.0f, -4000.0f,
                                      4000.0f);
                    undoOnActivate();
                }
                ImGui::TextDisabled("Anchors/pivot are relative to the PARENT rect\n"
                                    "(canvas, or the parent element). Tip: drag\n"
                                    "elements in the Scene viewport.");
            }
            ImGui::ColorEdit4(el->type == UIElement::Type::ProgressBar ? "Background"
                                                                       : "Color",
                              glm::value_ptr(el->color));
            undoOnActivate();

            // Texture picker (Image always; Panel/Button optionally).
            if (el->type == UIElement::Type::Image ||
                el->type == UIElement::Type::Panel ||
                el->type == UIElement::Type::Button) {
                std::string tpick;
                if (AssetPicker("Texture", el->texture, ".uaf", uaf::AssetType::Texture, tpick)) {
                    PushUndo(scene);
                    el->texture = tpick;
                    el->textureResolved = false; // re-resolve next frame
                }
                AssetDropTarget(".uaf", uaf::AssetType::Texture,
                                [&](const std::filesystem::path& src) {
                                    PushUndo(scene);
                                    el->texture = Project::Active().RelativeAssetPath(src);
                                    el->textureResolved = false;
                                });
            }

            if (el->type == UIElement::Type::ProgressBar) {
                ImGui::SliderFloat("Fill", &el->fill, 0.0f, 1.0f);
                undoOnActivate();
                ImGui::ColorEdit4("Fill Color", glm::value_ptr(el->fillColor));
                undoOnActivate();
                bool radial = el->radial;
                if (ImGui::Checkbox("Radial (wheel)", &radial)) {
                    PushUndo(scene);
                    el->radial = radial;
                }
            }

            // Button game-flow action: the engine handles the click (no script).
            if (el->type == UIElement::Type::Button) {
                const char* kActions[] = {"(none)", "play", "menu", "restart", "quit"};
                constexpr int kActionCount = 5;
                int cur = 0;
                for (int i = 1; i < kActionCount; ++i)
                    if (el->action == kActions[i]) cur = i;
                if (ImGui::Combo("Action", &cur, kActions, kActionCount)) {
                    PushUndo(scene);
                    el->action = (cur == 0) ? std::string() : kActions[cur];
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("play: menu->loading->gameplay; menu: back to main "
                                      "menu; restart: reload via the game loading screen; "
                                      "quit: exit. Handled by the engine.");
            }

            bool visible = el->visible;
            if (ImGui::Checkbox("Visible", &visible)) {
                PushUndo(scene);
                el->visible = visible;
            }
            if (Project::HasActive()) {
                const BuildSettings& build = Project::Active().Settings().build;
                static const char* kModes[] = {"stretch", "match height", "pixel-perfect"};
                ImGui::TextDisabled("Canvas: %ux%u (%s) - see Build Settings.",
                                    build.uiRefWidth, build.uiRefHeight,
                                    kModes[glm::clamp(build.uiScaleMode, 0u, 2u)]);
            }
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<UIElement>(sel);
        }
    }

    // --- UI canvas ---------------------------------------------------------------
    if (UICanvas* canvas = reg.try_get<UICanvas>(sel)) {
        const SectionState s = ComponentSection("UI Canvas");
        if (s.open) {
            int mode = static_cast<int>(glm::clamp(canvas->scaleMode, 0u, 2u));
            if (ImGui::Combo("Scale Mode", &mode,
                             "Stretch\0Match Height\0Pixel Perfect\0")) {
                PushUndo(scene);
                canvas->scaleMode = static_cast<u32>(mode);
            }
            ImGui::DragFloat("Ref Width", &canvas->refWidth, 1.0f, 64.0f, 8192.0f, "%.0f");
            undoOnActivate();
            ImGui::DragFloat("Ref Height", &canvas->refHeight, 1.0f, 64.0f, 8192.0f, "%.0f");
            undoOnActivate();
            ImGui::DragInt("Sort Order", &canvas->sortOrder, 0.1f, -100, 100);
            undoOnActivate();
            bool visible = canvas->visible;
            if (ImGui::Checkbox("Visible", &visible)) {
                PushUndo(scene);
                canvas->visible = visible;
            }
            ImGui::TextDisabled("UI elements parented under this entity lay out\n"
                                "inside this canvas (higher sort orders draw on top).");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<UICanvas>(sel);
        }
    }

    // --- Schematic (visual scripting) -------------------------------------------
    if (SchematicComponent* sc = reg.try_get<SchematicComponent>(sel)) {
        const SectionState s = ComponentSection("Schematic");
        if (s.open) {
            std::string pick;
            if (AssetPicker("Graph (.hbschem)", sc->asset, ".hbschem",
                            uaf::AssetType::Unknown, pick, "(none)")) {
                PushUndo(scene);
                sc->asset = pick;
                sc->started = false; // re-run On Start for the new graph
            }
            ImGui::BeginDisabled(sc->asset.empty());
            if (ImGui::Button("Edit")) {
                OpenSchematic(Project::Active().AssetsDir() / sc->asset);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("New Graph...")) {
                const std::filesystem::path p = CreateSchematicAsset();
                if (!p.empty()) {
                    PushUndo(scene);
                    std::error_code ec;
                    sc->asset = std::filesystem::relative(
                                    p, Project::Active().AssetsDir(), ec)
                                    .generic_string();
                    sc->started = false;
                    OpenSchematic(p);
                }
            }
            if (sc->asset.empty()) {
                ImGui::TextDisabled("Pick or create a .hbschem graph to drive this entity.");
            }
            ImGui::TextDisabled("Runs in play mode (Game tab) and in the runtime.");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<SchematicComponent>(sel);
        }
    }

    // --- Checkpoint (task goal / save point) ------------------------------------
    if (Checkpoint* cp = reg.try_get<Checkpoint>(sel)) {
        const SectionState s = ComponentSection("Checkpoint");
        if (s.open) {
            char idb[64], ob[128], cb[64];
            std::snprintf(idb, sizeof(idb), "%s", cp->id.c_str());
            if (ImGui::InputText("Id", idb, sizeof(idb))) cp->id = idb;
            std::snprintf(ob, sizeof(ob), "%s", cp->setObjective.c_str());
            if (ImGui::InputText("Set objective (HUD text)", ob, sizeof(ob))) cp->setObjective = ob;
            std::snprintf(cb, sizeof(cb), "%s", cp->completesObjective.c_str());
            if (ImGui::InputText("Completes objective (id)", cb, sizeof(cb)))
                cp->completesObjective = cb;
            ImGui::DragFloat3("Trigger half-extents", glm::value_ptr(cp->halfExtents), 0.1f, 0.0f,
                              1000.0f);
            ImGui::Checkbox("Trigger on player enter", &cp->triggerOnEnter);
            ImGui::Checkbox("Save on reach", &cp->saveOnReach);
            ImGui::SameLine();
            ImGui::Checkbox("Once", &cp->once);
            ImGui::TextDisabled("Also reachable from a Schematic (Reach Checkpoint node) -\n"
                                "e.g. on enemy death. Saves to Saves/checkpoint.hbsave.");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<Checkpoint>(sel);
        }
    }

    // --- Directional light -----------------------------------------------------
    if (DirectionalLightComponent* light = reg.try_get<DirectionalLightComponent>(sel)) {
        const SectionState s = ComponentSection("Directional Light");
        if (s.open) {
            if (ImGui::DragFloat3("Direction", glm::value_ptr(light->direction), 0.01f)) {
                if (glm::dot(light->direction, light->direction) > 1e-6f) {
                    light->direction = glm::normalize(light->direction);
                }
            }
            undoOnActivate();
            ImGui::ColorEdit3("Color", glm::value_ptr(light->color));
            undoOnActivate();
            ImGui::DragFloat("Intensity", &light->intensity, 0.05f, 0.0f, 100.0f);
            undoOnActivate();
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<DirectionalLightComponent>(sel);
        }
    }

    // --- Point light -------------------------------------------------------------
    if (PointLightComponent* light = reg.try_get<PointLightComponent>(sel)) {
        const SectionState s = ComponentSection("Point Light");
        if (s.open) {
            ImGui::ColorEdit3("Color", glm::value_ptr(light->color));
            undoOnActivate();
            ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 1000.0f);
            undoOnActivate();
            ImGui::DragFloat("Range", &light->range, 0.1f, 0.1f, 500.0f);
            undoOnActivate();
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<PointLightComponent>(sel);
        }
    }

    // --- Spot light --------------------------------------------------------------
    if (SpotLightComponent* light = reg.try_get<SpotLightComponent>(sel)) {
        const SectionState s = ComponentSection("Spot Light");
        if (s.open) {
            ImGui::ColorEdit3("Color", glm::value_ptr(light->color));
            undoOnActivate();
            ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 1000.0f);
            undoOnActivate();
            ImGui::DragFloat("Range", &light->range, 0.1f, 0.1f, 500.0f);
            undoOnActivate();
            if (ImGui::DragFloat("Inner Angle", &light->innerAngle, 0.25f, 0.0f, 89.0f)) {
                light->outerAngle = glm::max(light->outerAngle, light->innerAngle);
            }
            undoOnActivate();
            if (ImGui::DragFloat("Outer Angle", &light->outerAngle, 0.25f, 1.0f, 89.0f)) {
                light->innerAngle = glm::min(light->innerAngle, light->outerAngle);
            }
            undoOnActivate();
            ImGui::TextDisabled("Cone points along the entity's local -Y.");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<SpotLightComponent>(sel);
        }
    }

    if (RectLightComponent* light = reg.try_get<RectLightComponent>(sel)) {
        const SectionState s = ComponentSection("Rect (Area) Light");
        if (s.open) {
            ImGui::ColorEdit3("Color##rl", glm::value_ptr(light->color));
            undoOnActivate();
            ImGui::DragFloat("Intensity##rl", &light->intensity, 0.1f, 0.0f, 1000.0f);
            undoOnActivate();
            ImGui::DragFloat("Width##rl", &light->width, 0.05f, 0.05f, 100.0f);
            undoOnActivate();
            ImGui::DragFloat("Height##rl", &light->height, 0.05f, 0.05f, 100.0f);
            undoOnActivate();
            ImGui::DragFloat("Range##rl", &light->range, 0.1f, 0.1f, 500.0f);
            undoOnActivate();
            ImGui::Checkbox("Two-sided##rl", &light->twoSided);
            undoOnActivate();
            ImGui::TextDisabled("Panel emits along the entity's local -Z.");
        }
        if (s.remove) {
            PushUndo(scene);
            reg.remove<RectLightComponent>(sel);
        }
    }

    ImGui::End();
}

void Editor::DrawStats(Engine& engine) {
    if (!panelOpen_[Panel_Stats]) return;
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    ImGui::Begin("Stats", &panelOpen_[Panel_Stats]);
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Backend : %s", rhi::ToString(renderer.API()));
    ImGui::Text("Adapter : %s", renderer.AdapterName());
    ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
    ImGui::Text("Entities: %zu", scene.EntityCount());
    ImGui::Text("Jobs: %u worker threads (fiber system)", jobs::WorkerCount());
    ImGui::Separator();

    bool orbit = renderer.IsOrbitEnabled();
    if (ImGui::Checkbox("Orbit camera", &orbit)) {
        renderer.SetOrbitEnabled(orbit);
    }
    ImGui::TextDisabled("Uncheck to use the gizmo / freecam.");
    ImGui::TextDisabled("Hold RMB to fly: WASD/QE, Shift = fast.");
    ImGui::Separator();

    ImGui::TextDisabled(playMode_ ? (playPaused_ ? "Play mode: paused"
                                                 : "Play mode: playing")
                                  : "Press Play in the Game tab to run the game.");
    ImGui::Separator();

    ImGui::RadioButton("Translate", &gizmoMode_, 0); ImGui::SameLine();
    ImGui::RadioButton("Rotate", &gizmoMode_, 1);    ImGui::SameLine();
    ImGui::RadioButton("Scale", &gizmoMode_, 2);
    // Grid snapping for the gizmo (hold Ctrl while dragging to snap regardless).
    ImGui::Checkbox("Grid snap", &gizmoSnap_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap gizmo drags to the grid. Hold Ctrl to snap even when off.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("Move", &gizmoSnapStep_, 0.05f, 0.01f, 100.0f, "%.2f m");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("Rot", &gizmoSnapAngle_, 0.5f, 1.0f, 90.0f, "%.0f\xc2\xb0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("Scale##snap", &gizmoSnapScale_, 0.01f, 0.01f, 10.0f, "%.2f");
    ImGui::Checkbox("Editor icons", &showIcons_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show clickable billboard icons in the viewport for\n"
                          "lights, cameras, zones, splines, audio and empties.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Camera preview", &showCameraPreview_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Picture-in-picture of what the selected camera sees.");
    }
    ImGui::Separator();

    // UI layout editing (drag/resize UI elements in the Scene viewport).
    ImGui::Checkbox("UI snap", &uiSnap_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("##uisnapstep", &uiSnapStep_, 1.0f, 1.0f, 200.0f, "%.0f px");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Grid step for UI snapping (canvas pixels).\n"
                          "Elements also snap to the canvas center and edges.");
    }

    ImGui::Checkbox("ImGui demo window", &showDemo_);
    if (!buildResult_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Build: %s", buildResult_.c_str());
    }
    if (streamer_.Busy()) {
        ImGui::TextDisabled("Streaming scene: %s",
                            streamer_.CurrentPath().filename().string().c_str());
    }
    ImGui::End();
}

void Editor::DrawPostProcess(Engine& engine) {
    if (!panelOpen_[Panel_PostProcess]) return;
    SceneEnvironment& env = engine.GetScene().Environment();
    ImGui::Begin("Post Process", &panelOpen_[Panel_PostProcess]);
    ImGui::TextDisabled("Scene default LOOK (saved with the scene). Post Volumes");
    ImGui::TextDisabled("override this per-region; AA + GTAO live in Project Settings.");
    DrawPostLookControls(env.post, &env.exposure);

    // Make this look persist EVERY boot without per-scene saving: copy it into the
    // project's environment default (.hbproj). SetupEnvironment applies it at boot,
    // so it shows even before a scene sets its own; scenes/volumes still override.
    ImGui::Separator();
    if (Project::HasActive() && ImGui::Button("Save as project default")) {
        Project::Active().Settings().environment.post = env.post;
        Project::Active().Settings().environment.exposure = env.exposure;
        Project::Active().Save();
        buildResult_ = "Post look saved as the project default (.hbproj).";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes this look into the project so it loads every boot.\n"
                          "Per-scene saves and Post Volumes still override it.");
    ImGui::End();
}

// The LOOK widgets (volume-overridable + scene default). Exposure is optional
// (post volumes have no manual exposure of their own).
void Editor::DrawPostLookControls(rhi::PostSettings& p, f32* exposure) {
    // The enable flags are u32 (GPU-ready); bridge to bool for the checkbox.
    const auto toggle = [](const char* label, u32& v) {
        bool b = v != 0;
        if (ImGui::Checkbox(label, &b)) v = b ? 1u : 0u;
    };

    ImGui::SeparatorText("Painterly (oil on canvas)");
    toggle("Painterly finish", p.painterlyEnabled);
    if (p.painterlyEnabled) {
        ImGui::TextDisabled("Scene-driven brush strokes: they follow forms, stop at\n"
                            "silhouettes (G-buffer edges), and pick up the light's colour.");
        ImGui::SliderFloat("Strength##paint", &p.painterlyStrength, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Stroke size##paint", &p.painterlyRadius, 1.0f, 7.0f, "%.0f");
        ImGui::SliderFloat("Stroke flow##paint", &p.painterlyStrokeFlow, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Edge keep##paint", &p.painterlyEdge, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Light tint##paint", &p.painterlyLightTint, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Warm/cool##paint", &p.painterlyWarmCool, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Stroke texture##paint", &p.painterlyStrokeDetail, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Canvas weave##paint", &p.painterlyCanvasStrength, 0.0f, 0.5f, "%.2f");
        ImGui::SliderFloat("Canvas scale##paint", &p.painterlyCanvasScale, 2.0f, 32.0f, "%.0f px");
        ImGui::SliderFloat("Posterize steps##paint", &p.painterlyPosterize, 0.0f, 16.0f, "%.0f");
        ImGui::Spacing();
        toggle("Real brush strokes", p.painterlyStrokes);
        if (p.painterlyStrokes) {
            ImGui::SliderFloat("Stroke length##paint", &p.painterlyStrokeLength, 0.3f, 3.0f, "%.2f");
            ImGui::SliderFloat("Stroke density##paint", &p.painterlyStrokeDensity, 0.3f, 3.0f, "%.2f");
            ImGui::SliderFloat("Stroke sharpness##paint", &p.painterlyStrokeSharp, 0.0f, 1.0f, "%.2f");
        }
        ImGui::TextDisabled("Edge keep = how hard strokes stop at object silhouettes.\n"
                            "Light tint bleeds coloured lights into the paint. 0 posterize = off.\n"
                            "Real brush strokes splats actual paint marks over the base.");
    }

    ImGui::SeparatorText("Exposure");
    if (exposure) ImGui::SliderFloat("Manual", exposure, 0.05f, 8.0f, "%.2f");
    toggle("Auto exposure", p.autoExposureEnabled);
    if (p.autoExposureEnabled) {
        ImGui::SliderFloat("Key (mid-grey)", &p.autoExposureKey, 0.02f, 0.6f, "%.3f");
        ImGui::SliderFloat("Adapt speed", &p.autoExposureSpeed, 0.1f, 10.0f, "%.2f");
        ImGui::SliderFloat("Min", &p.autoExposureMin, 0.01f, 1.0f, "%.2f");
        ImGui::SliderFloat("Max", &p.autoExposureMax, 1.0f, 32.0f, "%.1f");
    }

    ImGui::SeparatorText("Bloom");
    toggle("Bloom", p.bloomEnabled);
    if (p.bloomEnabled) {
        ImGui::SliderFloat("Intensity##bloom", &p.bloomIntensity, 0.0f, 0.5f, "%.3f");
        ImGui::SliderFloat("Threshold##bloom", &p.bloomThreshold, 0.0f, 4.0f, "%.2f");
    }

    ImGui::SeparatorText("Global illumination (SSGI)");
    toggle("Screen-space GI", p.ssgiEnabled);
    if (p.ssgiEnabled) {
        ImGui::SliderFloat("Intensity##ssgi", &p.ssgiIntensity, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Radius##ssgi", &p.ssgiRadius, 0.5f, 16.0f, "%.1f");
        int samples = static_cast<int>(p.ssgiSamples);
        if (ImGui::SliderInt("Rays##ssgi", &samples, 2, 16)) p.ssgiSamples = static_cast<u32>(samples);
    }

    ImGui::SeparatorText("Reflections (SSR)");
    toggle("Screen-space reflections", p.ssrEnabled);
    if (p.ssrEnabled) {
        ImGui::SliderFloat("Intensity##ssr", &p.ssrIntensity, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Max distance##ssr", &p.ssrMaxDistance, 1.0f, 100.0f, "%.0f");
    }

    ImGui::SeparatorText("Volumetric fog");
    toggle("Volumetric fog", p.fogEnabled);
    if (p.fogEnabled) {
        ImGui::SliderFloat("Density##fog", &p.fogDensity, 0.0f, 0.1f, "%.4f");
        ImGui::SliderFloat("Height falloff##fog", &p.fogHeightFalloff, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Fog height##fog", &p.fogHeight, -50.0f, 50.0f, "%.1f");
        ImGui::SliderFloat("Anisotropy##fog", &p.fogAnisotropy, -0.95f, 0.95f, "%.2f");
        ImGui::SliderFloat("Sun scatter##fog", &p.fogSunIntensity, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("God rays##fog", &p.fogGodRays, 0.0f, 6.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Boosts the shadowed sun shafts (light through gaps).\n"
                              "Needs fog + shadows; 1 = neutral.");
        ImGui::ColorEdit3("Fog color##fog", glm::value_ptr(p.fogColor));
        ImGui::SliderFloat("Ambient##fog", &p.fogAmbient, 0.0f, 0.5f, "%.3f");
        ImGui::SliderFloat("Max distance##fog", &p.fogMaxDistance, 10.0f, 1000.0f, "%.0f");
        int steps = static_cast<int>(p.fogStepCount);
        if (ImGui::SliderInt("Steps##fog", &steps, 4, 64)) p.fogStepCount = static_cast<u32>(steps);
    }

    ImGui::SeparatorText("Depth of field");
    toggle("Depth of field", p.dofEnabled);
    if (p.dofEnabled) {
        ImGui::SliderFloat("Focus distance", &p.dofFocusDistance, 0.5f, 100.0f, "%.1f");
        ImGui::SliderFloat("Focus range", &p.dofFocusRange, 0.5f, 50.0f, "%.1f");
        ImGui::SliderFloat("Max blur##dof", &p.dofMaxBlur, 1.0f, 48.0f, "%.0f px");
    }

    ImGui::SeparatorText("Motion blur");
    toggle("Motion blur (camera)", p.motionBlurEnabled);
    if (p.motionBlurEnabled) {
        ImGui::SliderFloat("Intensity##mb", &p.motionBlurIntensity, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("Max radius##mb", &p.motionBlurMaxRadius, 1.0f, 64.0f, "%.0f px");
    }

    ImGui::SeparatorText("Color grade");
    ImGui::SliderFloat("Vignette", &p.vignette, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Saturation", &p.saturation, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Contrast", &p.contrast, 0.5f, 2.0f, "%.2f");
}

// Project-global rendering quality: anti-aliasing + ambient occlusion. These are
// NOT volume/scene overridable - one setting for the whole game (Project Settings).
bool Editor::DrawPostQualityControls(rhi::PostSettings& p) {
    bool changed = false;
    const auto toggle = [&changed](const char* label, u32& v) {
        bool b = v != 0;
        if (ImGui::Checkbox(label, &b)) { v = b ? 1u : 0u; changed = true; }
    };

    ImGui::SeparatorText("Anti-aliasing");
    toggle("TAA (temporal)", p.taaEnabled);
    ImGui::SameLine();
    toggle("FXAA", p.fxaaEnabled);

    ImGui::SeparatorText("Ambient occlusion (GTAO)");
    toggle("GTAO", p.ssaoEnabled);
    if (p.ssaoEnabled) {
        changed |= ImGui::SliderFloat("Radius##ssao", &p.ssaoRadius, 0.1f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Intensity##ssao", &p.ssaoIntensity, 0.0f, 3.0f, "%.2f");
    }
    return changed;
}

void Editor::DrawTimeline(Engine& engine) {
    if (!panelOpen_[Panel_Timeline]) return;
    Scene& scene = engine.GetScene();
    ImGui::Begin("Timeline", &panelOpen_[Panel_Timeline]);
    auto& reg = scene.Registry();

    if (selected_ == entt::null || !reg.valid(selected_)) {
        ImGui::TextDisabled("Select an entity to animate.");
        ImGui::End();
        return;
    }
    Transform* transform = reg.try_get<Transform>(selected_);
    AnimationTrack* track = reg.try_get<AnimationTrack>(selected_);
    if (!track) {
        if (ImGui::Button("Add Animation Track") && transform) {
            reg.emplace<AnimationTrack>(selected_);
            selectedKey_ = -1;
        }
        if (!transform) ImGui::TextDisabled("(needs a Transform component)");
        ImGui::End();
        return;
    }

    // --- Transport / track controls -----------------------------------------
    if (ImGui::Button(track->playing ? "Pause" : "Play")) {
        track->playing = !track->playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        track->playing = false;
        track->time = 0.0f;
        if (transform) anim::Sample(*track, *transform);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &track->loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("Speed", &track->speed, 0.05f, -4.0f, 4.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::DragFloat("Length", &track->duration, 0.1f, 0.1f, 600.0f)) {
        track->time = glm::min(track->time, track->duration);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Key") && transform) {
        PushUndo(scene);
        // Capture the entity's current local pose at the playhead.
        AnimationTrack::Key k;
        k.time = track->time;
        k.position = transform->position;
        k.rotation = transform->rotation;
        k.scale = transform->scale;
        auto same = std::find_if(track->keys.begin(), track->keys.end(),
                                 [&](const AnimationTrack::Key& a) {
                                     return std::abs(a.time - k.time) < 0.005f;
                                 });
        if (same != track->keys.end()) {
            *same = k;
        } else {
            track->keys.insert(std::upper_bound(track->keys.begin(), track->keys.end(),
                                                k.time,
                                                [](f32 t, const AnimationTrack::Key& a) {
                                                    return t < a.time;
                                                }),
                               k);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedKey_ < 0 ||
                         selectedKey_ >= static_cast<int>(track->keys.size()));
    if (ImGui::Button("Delete Key")) {
        PushUndo(scene);
        track->keys.erase(track->keys.begin() + selectedKey_);
        selectedKey_ = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%.2fs / %.2fs  (%d keys)", track->time, track->duration,
                        static_cast<int>(track->keys.size()));

    // --- Track strip: ruler + keys + playhead --------------------------------
    const f32 stripH = 44.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 stripW = glm::max(avail.x, 50.0f);
    ImGui::InvisibleButton("##strip", ImVec2(stripW, stripH));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(p0, p1, IM_COL32(36, 36, 42, 255), 4.0f);
    // Second ticks (subdivided when zoomed in enough).
    const f32 pxPerSec = stripW / glm::max(track->duration, 1e-3f);
    for (f32 t = 0.0f; t <= track->duration + 1e-3f; t += 1.0f) {
        const f32 x = p0.x + t * pxPerSec;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + 8.0f), IM_COL32(120, 120, 130, 255));
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f", t);
        dl->AddText(ImVec2(x + 2.0f, p0.y + 2.0f), IM_COL32(120, 120, 130, 255), label);
    }

    // Key diamonds.
    const f32 keyY = (p0.y + p1.y) * 0.5f + 6.0f;
    for (int i = 0; i < static_cast<int>(track->keys.size()); ++i) {
        const f32 x = p0.x + track->keys[i].time * pxPerSec;
        const f32 r = 5.0f;
        const ImU32 col = (i == selectedKey_) ? IM_COL32(255, 200, 60, 255)
                                              : IM_COL32(120, 180, 255, 255);
        dl->AddQuadFilled(ImVec2(x, keyY - r), ImVec2(x + r, keyY), ImVec2(x, keyY + r),
                          ImVec2(x - r, keyY), col);
    }

    // Playhead.
    {
        const f32 x = p0.x + track->time * pxPerSec;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 90, 90, 255), 2.0f);
    }

    // Interaction: click near a key selects it; click/drag elsewhere scrubs.
    if (ImGui::IsItemActivated()) {
        const f32 mx = ImGui::GetMousePos().x;
        selectedKey_ = -1;
        for (int i = 0; i < static_cast<int>(track->keys.size()); ++i) {
            const f32 kx = p0.x + track->keys[i].time * pxPerSec;
            if (std::abs(mx - kx) <= 6.0f) {
                selectedKey_ = i;
                break;
            }
        }
    }
    if (ImGui::IsItemActive() && selectedKey_ < 0) {
        const f32 t = glm::clamp((ImGui::GetMousePos().x - p0.x) / pxPerSec, 0.0f,
                                 track->duration);
        track->time = t;
        if (!track->playing && transform && !track->keys.empty()) {
            anim::Sample(*track, *transform); // live preview while scrubbing
        }
    }

    ImGui::End();
}

namespace {
// Slab ray/AABB test in world space. Returns the entry distance in `tHit`.
bool RayAABB(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin,
             const glm::vec3& bmax, float& tHit) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(rd[i]) < 1e-8f) {
            if (ro[i] < bmin[i] || ro[i] > bmax[i]) return false;
        } else {
            const float inv = 1.0f / rd[i];
            float t1 = (bmin[i] - ro[i]) * inv;
            float t2 = (bmax[i] - ro[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    tHit = tmin;
    return true;
}
} // namespace

void Editor::PickEntity(Scene& scene, Renderer& renderer) {
    const ImVec2 mouse = ImGui::GetMousePos();
    const float mx = (mouse.x - vpX_) / vpW_;
    const float my = (mouse.y - vpY_) / vpH_;
    if (mx < 0.0f || mx > 1.0f || my < 0.0f || my > 1.0f) return;
    selected_ = EntityUnderPixel(scene, renderer, mx, my); // entt::null deselects
}

entt::entity Editor::EntityUnderPixel(Scene& scene, Renderer& renderer, f32 mx, f32 my) {
    // Build a world-space ray from the mouse through the camera.
    const Camera& cam = renderer.GetCamera();
    const glm::mat4 invVP = glm::inverse(cam.ViewProjection());
    const glm::vec2 ndc(mx * 2.0f - 1.0f, 1.0f - my * 2.0f);
    glm::vec4 pNear = invVP * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 pFar = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    pNear /= pNear.w;
    pFar /= pFar.w;
    const glm::vec3 ro(pNear);
    const glm::vec3 rd = glm::normalize(glm::vec3(pFar) - glm::vec3(pNear));

    float best = 1e30f;
    entt::entity hit = entt::null;
    auto& reg = scene.Registry();
    auto view = reg.view<Transform, AABB>();
    for (const entt::entity e : view) {
        const glm::mat4 m = scene.WorldMatrix(e);
        const AABB& box = view.get<AABB>(e);
        // World AABB from the 8 transformed corners.
        glm::vec3 wmin(1e30f), wmax(-1e30f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? box.max.x : box.min.x,
                                   (c & 2) ? box.max.y : box.min.y,
                                   (c & 4) ? box.max.z : box.min.z);
            const glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
            wmin = glm::min(wmin, w);
            wmax = glm::max(wmax, w);
        }
        float t0;
        if (RayAABB(ro, rd, wmin, wmax, t0) && t0 < best) {
            best = t0;
            hit = e;
        }
    }
    return hit;
}

glm::vec3 Editor::DropPointInWorld(Scene& scene, Renderer& renderer, f32 mx, f32 my) {
    const Camera& cam = renderer.GetCamera();
    const glm::mat4 invVP = glm::inverse(cam.ViewProjection());
    const glm::vec2 ndc(mx * 2.0f - 1.0f, 1.0f - my * 2.0f);
    glm::vec4 pNear = invVP * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 pFar = invVP * glm::vec4(ndc, 1.0f, 1.0f);
    pNear /= pNear.w;
    pFar /= pFar.w;
    const glm::vec3 ro(pNear);
    const glm::vec3 rd = glm::normalize(glm::vec3(pFar) - glm::vec3(pNear));

    // Nearest AABB hit (e.g. the ground plane); fall back to a point ahead.
    f32 best = 1e30f;
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<Transform, AABB>()) {
        const glm::mat4 m = scene.WorldMatrix(e);
        const AABB& box = reg.get<AABB>(e);
        glm::vec3 wmin(1e30f), wmax(-1e30f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? box.max.x : box.min.x,
                                   (c & 2) ? box.max.y : box.min.y,
                                   (c & 4) ? box.max.z : box.min.z);
            const glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
            wmin = glm::min(wmin, w);
            wmax = glm::max(wmax, w);
        }
        float t0;
        if (RayAABB(ro, rd, wmin, wmax, t0) && t0 < best) best = t0;
    }
    return ro + rd * (best < 1e30f ? best : 10.0f);
}

// --- Editor asset-name helpers ----------------------------------------------------

namespace {

// Strips filesystem-illegal characters and trims surrounding spaces so a typed
// asset name is safe to use as a file stem (empty if nothing usable remains).
std::string SanitizeFileStem(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            continue;
        out.push_back(c);
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
} // namespace

// --- Schematic (visual scripting) editor ------------------------------------------

void Editor::OpenSchematic(const std::filesystem::path& path) {
    schematic::Graph g;
    if (!schematic::LoadGraph(path, g) || g.nodes.empty()) {
        // Fresh / unreadable: seed with an On Update event so the canvas isn't blank.
        g = schematic::Graph{};
        g.AddNode(schematic::NodeType::EventUpdate, {48.0f, 48.0f});
    }
    schematicGraph_ = std::move(g);
    editedSchematic_ = path;
    schematicDirty_ = false;
    schematicFocus_ = true;
    schemPan_ = glm::vec2(0.0f);
    schemSelected_ = 0;
    schemDragging_ = false;
    panelOpen_[Panel_SchematicEditor] = true;
}

void Editor::SaveSchematic() {
    if (editedSchematic_.empty()) return;
    if (schematic::SaveGraph(editedSchematic_, schematicGraph_)) {
        schematicDirty_ = false;
        schematic::ClearCache(); // running entities reload the edited graph next play
        assetsDirty_ = true;     // surface the file in the asset browser
    }
}

std::filesystem::path Editor::CreateSchematicAsset(const std::filesystem::path& dirIn,
                                                   const std::string& name) {
    namespace fs = std::filesystem;
    if (!Project::HasActive()) return {};
    const fs::path dir = dirIn.empty() ? Project::Active().AssetsDir() / "Schematics" : dirIn;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string base = name.empty() ? "NewSchematic" : name;
    fs::path p = dir / (base + ".hbschem");
    for (int i = 1; fs::exists(p); ++i) p = dir / (base + std::to_string(i) + ".hbschem");
    schematic::Graph g;
    g.AddNode(schematic::NodeType::EventUpdate, {48.0f, 48.0f});
    if (!schematic::SaveGraph(p, g)) return {};
    assetsDirty_ = true;
    return p;
}

void Editor::DrawSchematicEditor(Engine& engine) {
    (void)engine;
    if (!panelOpen_[Panel_SchematicEditor]) return;
    if (schematicFocus_) {
        ImGui::SetNextWindowFocus();
        schematicFocus_ = false;
    }
    if (!ImGui::Begin("Schematic Editor", &panelOpen_[Panel_SchematicEditor])) {
        ImGui::End();
        return;
    }

    // Toolbar: New / Open / Save + the open file name.
    if (ImGui::Button("New")) {
        const std::filesystem::path p = CreateSchematicAsset();
        if (!p.empty()) OpenSchematic(p);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) ImGui::OpenPopup("##schemopen");
    ImGui::SameLine();
    ImGui::BeginDisabled(editedSchematic_.empty());
    if (ImGui::Button(schematicDirty_ ? "Save*" : "Save")) SaveSchematic();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (editedSchematic_.empty()) {
        ImGui::TextDisabled("(no graph open)");
    } else {
        ImGui::Text("%s", editedSchematic_.filename().string().c_str());
        if (schematicDirty_) {
            ImGui::SameLine();
            ImGui::TextDisabled("(unsaved - Ctrl+S)");
        }
    }

    if (ImGui::BeginPopup("##schemopen")) {
        bool any = false;
        for (const std::string& rel : ListAssetsByExt(".hbschem")) {
            any = true;
            if (ImGui::Selectable(rel.c_str())) {
                OpenSchematic(Project::Active().AssetsDir() / rel);
                ImGui::CloseCurrentPopup();
            }
        }
        if (!any) ImGui::TextDisabled("No .hbschem assets yet - use New.");
        ImGui::EndPopup();
    }

    if (editedSchematic_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Create or open a schematic graph, wire up nodes, then add a "
            "Schematic component to an entity (Add Component > Schematic) and "
            "point it at this file. It runs in play mode and in the runtime.");
        ImGui::TextDisabled("Right-click the canvas to add a node. Drag from a pin to wire.");
        ImGui::End();
        return;
    }

    ImGui::Separator();
    DrawSchematicCanvas();

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        SaveSchematic();
    }
    ImGui::End();
}

void Editor::DrawSchematicCanvas() {
    namespace sk = schematic;
    using sk::NodeType;
    using sk::PinType;

    // Node geometry (canvas-space pixels).
    const float NW = 230.0f, TITLEH = 26.0f, ROWH = 24.0f, PAD = 6.0f, PINR = 5.0f;

    auto pinColor = [](PinType t) -> ImU32 {
        switch (t) {
            case PinType::Exec:   return IM_COL32(235, 235, 235, 255);
            case PinType::Float:  return IM_COL32(140, 220, 120, 255);
            case PinType::Bool:   return IM_COL32(220, 110, 100, 255);
            case PinType::Vec3:   return IM_COL32(232, 200, 90, 255);
            case PinType::String: return IM_COL32(220, 130, 210, 255);
            case PinType::Entity: return IM_COL32(110, 200, 220, 255);
        }
        return IM_COL32(200, 200, 200, 255);
    };
    auto catColor = [](const char* cat) -> ImU32 {
        const std::string c = cat;
        if (c == "Events")    return IM_COL32(150, 52, 60, 255);
        if (c == "Flow")      return IM_COL32(58, 70, 104, 255);
        if (c == "Variables") return IM_COL32(92, 70, 112, 255);
        if (c == "Constants") return IM_COL32(70, 92, 70, 255);
        if (c == "Math")      return IM_COL32(56, 96, 82, 255);
        if (c == "Compare")   return IM_COL32(98, 84, 56, 255);
        if (c == "Logic")     return IM_COL32(98, 60, 76, 255);
        if (c == "Vec3")      return IM_COL32(70, 86, 102, 255);
        if (c == "Entity")    return IM_COL32(54, 86, 96, 255);
        if (c == "Input")     return IM_COL32(98, 76, 50, 255);
        if (c == "Transform") return IM_COL32(54, 82, 70, 255);
        return IM_COL32(72, 72, 84, 255);
    };

    // Catalog grouped by category for the add-node menu (built once).
    struct Cat { const char* name; std::vector<NodeType> types; };
    static std::vector<Cat> cats;
    if (cats.empty()) {
        for (int i = 0; i < static_cast<int>(NodeType::Count); ++i) {
            const NodeType t = static_cast<NodeType>(i);
            const char* cat = sk::Describe(t).category;
            auto it = std::find_if(cats.begin(), cats.end(),
                                   [&](const Cat& c) { return std::strcmp(c.name, cat) == 0; });
            if (it == cats.end()) cats.push_back({cat, {t}});
            else it->types.push_back(t);
        }
    }

    ImGui::BeginChild("##schemcanvas", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoMove);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cP0 = ImGui::GetCursorScreenPos();
    const ImVec2 cSz = ImGui::GetContentRegionAvail();
    const ImVec2 cP1(cP0.x + cSz.x, cP0.y + cSz.y);
    const ImVec2 mp = io.MousePos;
    const bool canvasHovered = ImGui::IsWindowHovered();

    // Background + grid.
    dl->AddRectFilled(cP0, cP1, IM_COL32(26, 27, 32, 255));
    const float grid = 24.0f;
    const ImU32 gridCol = IM_COL32(40, 42, 48, 255);
    for (float x = std::fmod(schemPan_.x, grid); x < cSz.x; x += grid)
        dl->AddLine(ImVec2(cP0.x + x, cP0.y), ImVec2(cP0.x + x, cP1.y), gridCol);
    for (float y = std::fmod(schemPan_.y, grid); y < cSz.y; y += grid)
        dl->AddLine(ImVec2(cP0.x, cP0.y + y), ImVec2(cP1.x, cP0.y + y), gridCol);
    dl->AddRect(cP0, cP1, IM_COL32(12, 12, 16, 255));

    const ImVec2 origin(cP0.x + schemPan_.x, cP0.y + schemPan_.y);
    auto toScreen = [&](glm::vec2 p) { return ImVec2(origin.x + p.x, origin.y + p.y); };
    auto rowCenterY = [&](const sk::Node& n, int i) {
        return toScreen(n.pos).y + TITLEH + ROWH * i + ROWH * 0.5f;
    };
    auto inPinPos = [&](const sk::Node& n, int i) {
        return ImVec2(toScreen(n.pos).x, rowCenterY(n, i));
    };
    auto outPinPos = [&](const sk::Node& n, int i) {
        return ImVec2(toScreen(n.pos).x + NW, rowCenterY(n, i));
    };
    auto hasInLink = [&](u32 node, u32 pin) {
        for (const sk::Link& l : schematicGraph_.links)
            if (l.toNode == node && l.toPin == pin) return true;
        return false;
    };
    auto dist2 = [](ImVec2 a, ImVec2 b) {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy;
    };

    // --- Hovered pin (nearest within a small radius) -------------------------------
    struct PinHover { u32 node = 0, pin = 0; bool isOutput = false; bool valid = false; };
    PinHover hov;
    {
        float best = (PINR + 4.0f) * (PINR + 4.0f);
        for (const sk::Node& n : schematicGraph_.nodes) {
            const sk::NodeDesc& d = sk::Describe(n.type);
            for (u32 i = 0; i < d.inputs.size(); ++i) {
                const float dd = dist2(mp, inPinPos(n, static_cast<int>(i)));
                if (dd < best) { best = dd; hov = {n.id, i, false, true}; }
            }
            for (u32 i = 0; i < d.outputs.size(); ++i) {
                const float dd = dist2(mp, outPinPos(n, static_cast<int>(i)));
                if (dd < best) { best = dd; hov = {n.id, i, true, true}; }
            }
        }
    }

    // --- Links (bezier) + link hover ----------------------------------------------
    auto bez = [](ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float t) {
        const float u = 1.0f - t;
        const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        return ImVec2(w0 * p0.x + w1 * c0.x + w2 * c1.x + w3 * p1.x,
                      w0 * p0.y + w1 * c0.y + w2 * c1.y + w3 * p1.y);
    };
    int hoveredLink = -1;
    for (u32 li = 0; li < schematicGraph_.links.size(); ++li) {
        const sk::Link& l = schematicGraph_.links[li];
        const sk::Node* a = schematicGraph_.Find(l.fromNode);
        const sk::Node* b = schematicGraph_.Find(l.toNode);
        if (!a || !b) continue;
        const sk::NodeDesc& da = sk::Describe(a->type);
        if (l.fromPin >= da.outputs.size() || l.toPin >= sk::Describe(b->type).inputs.size())
            continue;
        const ImVec2 p0 = outPinPos(*a, static_cast<int>(l.fromPin));
        const ImVec2 p1 = inPinPos(*b, static_cast<int>(l.toPin));
        const float dx = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.5f);
        const ImVec2 c0(p0.x + dx, p0.y), c1(p1.x - dx, p1.y);
        // Cheap hover test: sample the curve.
        float md = 1e9f;
        for (int s = 0; s <= 16; ++s) md = std::min(md, dist2(mp, bez(p0, c0, c1, p1, s / 16.0f)));
        const bool lh = canvasHovered && md < 36.0f && !hov.valid;
        if (lh) hoveredLink = static_cast<int>(li);
        const ImU32 col = lh ? IM_COL32(255, 200, 90, 255) : pinColor(da.outputs[l.fromPin].type);
        dl->AddBezierCubic(p0, c0, c1, p1, col, lh ? 3.5f : 2.2f);
    }

    // --- Nodes ---------------------------------------------------------------------
    auto editLiteral = [&](sk::Node& nd, int pin, PinType pt, float x, float cy, float w) {
        ImGui::PushID(pin);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - ImGui::GetFrameHeight() * 0.5f));
        ImGui::SetNextItemWidth(w);
        sk::Value& lit = nd.literals[pin];
        switch (pt) {
            case PinType::Float:
                if (ImGui::DragFloat("##f", &lit.f, 0.05f, 0, 0, "%.3f")) schematicDirty_ = true;
                break;
            case PinType::Bool:
                if (ImGui::Checkbox("##b", &lit.b)) schematicDirty_ = true;
                break;
            case PinType::Vec3: {
                float v[3] = {lit.v3.x, lit.v3.y, lit.v3.z};
                if (ImGui::DragFloat3("##v", v, 0.05f, 0, 0, "%.2f")) {
                    lit.v3 = {v[0], v[1], v[2]};
                    schematicDirty_ = true;
                }
                break;
            }
            case PinType::String: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s", lit.s.c_str());
                if (ImGui::InputText("##s", buf, sizeof(buf))) {
                    lit.s = buf;
                    schematicDirty_ = true;
                }
                break;
            }
            default: break; // Exec / Entity have no inline editor
        }
        ImGui::PopID();
    };

    for (sk::Node& n : schematicGraph_.nodes) {
        const sk::NodeDesc& d = sk::Describe(n.type);
        const int rows = static_cast<int>(std::max(d.inputs.size(), d.outputs.size()));
        const ImVec2 nMin = toScreen(n.pos);
        const ImVec2 nMax(nMin.x + NW, nMin.y + TITLEH + ROWH * rows + PAD);
        const bool selected = (schemSelected_ == n.id);

        // Body + title bar + border.
        dl->AddRectFilled(nMin, nMax, IM_COL32(42, 44, 52, 240), 5.0f);
        dl->AddRectFilled(nMin, ImVec2(nMax.x, nMin.y + TITLEH), catColor(d.category), 5.0f,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRect(nMin, nMax, selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(18, 18, 22, 255),
                    5.0f, 0, selected ? 2.5f : 1.2f);
        dl->AddText(ImVec2(nMin.x + 10, nMin.y + 5), IM_COL32(240, 240, 245, 255), d.name);

        ImGui::PushID(static_cast<int>(n.id));

        // Title bar: click selects, drag moves.
        ImGui::SetCursorScreenPos(nMin);
        ImGui::InvisibleButton("title", ImVec2(NW, TITLEH));
        if (ImGui::IsItemActivated()) schemSelected_ = n.id;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            n.pos.x += io.MouseDelta.x;
            n.pos.y += io.MouseDelta.y;
            schematicDirty_ = true;
        }

        // Input pins (left) + labels + inline literal editors.
        for (u32 i = 0; i < d.inputs.size(); ++i) {
            const PinType pt = d.inputs[i].type;
            const ImVec2 pp = inPinPos(n, static_cast<int>(i));
            const ImU32 col = pinColor(pt);
            if (pt == PinType::Exec) {
                dl->AddTriangleFilled(ImVec2(pp.x - 4, pp.y - 5), ImVec2(pp.x - 4, pp.y + 5),
                                      ImVec2(pp.x + 5, pp.y), col);
            } else {
                dl->AddCircleFilled(pp, PINR, col);
            }
            const bool linked = hasInLink(n.id, i);
            const char* nm = d.inputs[i].name;
            if (nm && nm[0]) dl->AddText(ImVec2(pp.x + 10, pp.y - 7), IM_COL32(200, 202, 210, 255), nm);
            if (!linked && pt != PinType::Exec && pt != PinType::Entity) {
                const float ex = nMin.x + 72.0f;
                const float ew = (pt == PinType::Vec3) ? 130.0f : (pt == PinType::String ? 96.0f : 78.0f);
                editLiteral(n, static_cast<int>(i), pt, ex, pp.y, ew);
            } else if (pt == PinType::Entity && !linked) {
                dl->AddText(ImVec2(pp.x + 70, pp.y - 7), IM_COL32(120, 130, 140, 255), "(self)");
            }
        }
        // Output pins (right) + right-aligned labels.
        for (u32 i = 0; i < d.outputs.size(); ++i) {
            const PinType pt = d.outputs[i].type;
            const ImVec2 pp = outPinPos(n, static_cast<int>(i));
            const ImU32 col = pinColor(pt);
            if (pt == PinType::Exec) {
                dl->AddTriangleFilled(ImVec2(pp.x - 5, pp.y - 5), ImVec2(pp.x - 5, pp.y + 5),
                                      ImVec2(pp.x + 4, pp.y), col);
            } else {
                dl->AddCircleFilled(pp, PINR, col);
            }
            const char* nm = d.outputs[i].name;
            if (nm && nm[0]) {
                const float tw = ImGui::CalcTextSize(nm).x;
                dl->AddText(ImVec2(pp.x - 10 - tw, pp.y - 7), IM_COL32(200, 202, 210, 255), nm);
            }
        }
        ImGui::PopID();
    }

    // --- In-progress wire ----------------------------------------------------------
    if (schemDragging_) {
        if (const sk::Node* s = schematicGraph_.Find(schemDragNode_)) {
            const ImVec2 sp = schemDragFromOutput_ ? outPinPos(*s, static_cast<int>(schemDragPin_))
                                                   : inPinPos(*s, static_cast<int>(schemDragPin_));
            const float dx = std::max(40.0f, std::fabs(mp.x - sp.x) * 0.5f);
            const float dir = schemDragFromOutput_ ? 1.0f : -1.0f;
            dl->AddBezierCubic(sp, ImVec2(sp.x + dx * dir, sp.y),
                               ImVec2(mp.x - dx * dir, mp.y), mp, IM_COL32(255, 235, 150, 255), 2.4f);
        } else {
            schemDragging_ = false; // source node vanished
        }
    }

    // --- Interaction ---------------------------------------------------------------
    // Start (or re-pick) a wire by pressing a pin.
    if (canvasHovered && hov.valid && !schemDragging_ &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool started = false;
        if (!hov.isOutput) {
            // Pressing a connected input "picks up" the existing wire (Unreal-style).
            for (u32 i = 0; i < schematicGraph_.links.size(); ++i) {
                const sk::Link& l = schematicGraph_.links[i];
                if (l.toNode == hov.node && l.toPin == hov.pin) {
                    schemDragNode_ = l.fromNode;
                    schemDragPin_ = l.fromPin;
                    schemDragFromOutput_ = true;
                    schematicGraph_.RemoveLink(i);
                    schematicDirty_ = true;
                    started = true;
                    break;
                }
            }
        }
        if (!started) {
            schemDragNode_ = hov.node;
            schemDragPin_ = hov.pin;
            schemDragFromOutput_ = hov.isOutput;
        }
        schemDragging_ = true;
    }
    // Release: connect if over an opposite-orientation pin.
    if (schemDragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (hov.valid && hov.isOutput != schemDragFromOutput_) {
            u32 fn, fp, tn, tp;
            if (schemDragFromOutput_) { fn = schemDragNode_; fp = schemDragPin_; tn = hov.node; tp = hov.pin; }
            else { fn = hov.node; fp = hov.pin; tn = schemDragNode_; tp = schemDragPin_; }
            if (schematicGraph_.Connect(fn, fp, tn, tp)) schematicDirty_ = true;
        }
        schemDragging_ = false;
    }

    // Empty-canvas left click deselects.
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hov.valid &&
        !ImGui::IsAnyItemHovered()) {
        schemSelected_ = 0;
    }

    // Pan: middle-drag, or left-drag on empty space.
    if (canvasHovered && !schemDragging_ && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
            (!hov.valid && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))) {
            schemPan_.x += io.MouseDelta.x;
            schemPan_.y += io.MouseDelta.y;
        }
    }

    // Delete the selected node.
    if (schemSelected_ && ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        schematicGraph_.RemoveNode(schemSelected_);
        schemSelected_ = 0;
        schematicDirty_ = true;
    }

    // Right click: delete a hovered link, else open the add-node menu.
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hoveredLink >= 0) {
            schematicGraph_.RemoveLink(static_cast<u32>(hoveredLink));
            schematicDirty_ = true;
        } else if (!hov.valid) {
            schemAddPos_ = glm::vec2(mp.x - origin.x, mp.y - origin.y);
            ImGui::OpenPopup("##addnode");
        }
    }
    if (ImGui::BeginPopup("##addnode")) {
        for (const Cat& c : cats) {
            if (ImGui::BeginMenu(c.name)) {
                for (const NodeType t : c.types) {
                    if (ImGui::MenuItem(sk::Describe(t).name)) {
                        schemSelected_ = schematicGraph_.AddNode(t, schemAddPos_);
                        schematicDirty_ = true;
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

// --- Undo / redo ------------------------------------------------------------------

void Editor::PushUndo(Scene& scene) {
    undoStack_.push_back(scene::SaveSceneToString(scene));
    if (undoStack_.size() > kMaxUndoSteps) {
        undoStack_.erase(undoStack_.begin());
    }
    redoStack_.clear(); // a new edit invalidates the redo branch
}

void Editor::Undo(Engine& engine) {
    if (undoStack_.empty()) return;
    redoStack_.push_back(scene::SaveSceneToString(engine.GetScene()));
    const std::string snapshot = std::move(undoStack_.back());
    undoStack_.pop_back();
    RestoreSnapshot(engine, snapshot);
}

void Editor::Redo(Engine& engine) {
    if (redoStack_.empty()) return;
    undoStack_.push_back(scene::SaveSceneToString(engine.GetScene()));
    const std::string snapshot = std::move(redoStack_.back());
    redoStack_.pop_back();
    RestoreSnapshot(engine, snapshot);
}

void Editor::RestoreSnapshot(Engine& engine, const std::string& snapshot) {
    scene::SceneData data;
    if (!scene::ParseSceneString(snapshot, data)) return;
    scene::StagedAssets staged;
    const std::filesystem::path assetsDir =
        Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
    scene::StageAssets(data, assetsDir, staged);
    selected_ = entt::null; // entities are recreated; old ids are invalid
    engine.GetPhysics().SetEditedEntity(entt::null);
    scene::Instantiate(engine.GetScene(), engine.GetRenderer(), data, staged,
                       scene::LoadMode::Replace);
}

// --- Copy / paste / duplicate ----------------------------------------------------

void Editor::CopySelection(Scene& scene) {
    if (selected_ == entt::null || !scene.Registry().valid(selected_)) return;
    clipboard_ = scene::SaveSubtreeToString(scene, selected_);
}

void Editor::PasteSubtree(Engine& engine, const std::string& fragment,
                          const glm::vec3* placeAt) {
    if (fragment.empty()) return;
    scene::SceneData data;
    if (!scene::ParseSceneString(fragment, data) || data.entities.empty()) return;

    Scene& scene = engine.GetScene();
    PushUndo(scene); // a paste is one undo step

    scene::StagedAssets staged;
    const std::filesystem::path assetsDir =
        Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
    scene::StageAssets(data, assetsDir, staged);

    std::vector<entt::entity> created;
    scene::Instantiate(scene, engine.GetRenderer(), data, staged,
                       scene::LoadMode::Additive, &created);
    if (created.empty()) return;

    // A cloned PaintComponent inherits the original's .hbpaint path; clear it so
    // a later Save writes each clone its own file. The pixels are already copied
    // into the clone's layers in memory, so the canvas still renders.
    for (const entt::entity e : created) {
        if (PaintComponent* pc = scene.Registry().try_get<PaintComponent>(e))
            pc->source.clear();
    }

    // created[0] is the subtree root (serialized first). A prefab drop places it at
    // `placeAt`; copy/paste/duplicate keeps the original's exact position (clone sits
    // on top - move it with the gizmo). Either way, select the new root.
    const entt::entity root = created.front();
    if (placeAt) {
        if (Transform* t = scene.Registry().try_get<Transform>(root)) t->position = *placeAt;
    }
    selected_ = root;
}

void Editor::PasteClipboard(Engine& engine) { PasteSubtree(engine, clipboard_); }

std::filesystem::path Editor::CreatePrefabFromSelection(Scene& scene,
                                                        const std::filesystem::path& dirIn,
                                                        const std::string& name) {
    namespace fs = std::filesystem;
    if (!Project::HasActive() || selected_ == entt::null || !scene.Registry().valid(selected_))
        return {};
    const std::string frag = scene::SaveSubtreeToString(scene, selected_);
    if (frag.empty()) return {};
    const fs::path dir = dirIn.empty() ? Project::Active().AssetsDir() / "Prefabs" : dirIn;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string base = name.empty() ? "Prefab" : name;
    fs::path p = dir / (base + ".hbprefab");
    for (int i = 1; fs::exists(p); ++i) p = dir / (base + std::to_string(i) + ".hbprefab");
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    out.write(frag.data(), static_cast<std::streamsize>(frag.size()));
    assetsDirty_ = true;
    return p;
}

void Editor::InstantiatePrefab(Engine& engine, const std::filesystem::path& path,
                               const glm::vec3* at) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    const std::string frag((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PasteSubtree(engine, frag, at); // additive; selects + places the new root
}

void Editor::DuplicateSelection(Engine& engine) {
    Scene& scene = engine.GetScene();
    if (selected_ == entt::null || !scene.Registry().valid(selected_)) return;
    PasteSubtree(engine, scene::SaveSubtreeToString(scene, selected_));
}

// --- Asset viewer ---------------------------------------------------------------

void Editor::SelectAsset(const std::filesystem::path& path) {
    if (viewedAsset_ == path) return;
    viewedAsset_ = path;
    viewerDirty_ = true;
    editedMatValid_ = false;
    editedMatDirty_ = false;
}

std::filesystem::path Editor::CreateMaterialAsset(const std::filesystem::path& dir,
                                                  const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string base = SanitizeFileStem(name);
    if (base.empty()) base = "NewMaterial";
    std::string stem = base;
    for (int i = 1; std::filesystem::exists(dir / (stem + assets::kMaterialExtension), ec); ++i)
        stem = base + std::to_string(i);
    const std::filesystem::path p = dir / (stem + assets::kMaterialExtension);
    MaterialAsset m;
    m.name = stem;
    return assets::SaveMaterial(p, m) ? p : std::filesystem::path{};
}

std::filesystem::path Editor::CreateAudioEventAsset(const std::filesystem::path& dir,
                                                    const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string base = SanitizeFileStem(name);
    if (base.empty()) base = "NewAudioEvent";
    std::string stem = base;
    for (int i = 1; std::filesystem::exists(dir / (stem + assets::kAudioEventExtension), ec); ++i)
        stem = base + std::to_string(i);
    const std::filesystem::path p = dir / (stem + assets::kAudioEventExtension);
    return assets::SaveAudioEvent(p, AudioEvent{}) ? p : std::filesystem::path{};
}

void Editor::DrawMusicEditor(Engine& engine) {
    if (!panelOpen_[Panel_Music]) return;
    AudioSystem& audio = engine.GetAudio();
    ImGui::Begin("Music", &panelOpen_[Panel_Music]);
    if (!Project::HasActive()) {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }
    ProjectSettings& settings = Project::Active().Settings();
    const std::filesystem::path assets = Project::Active().AssetsDir();

    // Sync the working graph from the project once per session.
    if (!musicLoaded_) {
        musicLoaded_ = true;
        musicEditPath_ = settings.musicGraph;
        musicEdit_ = MusicGraph{};
        if (!settings.musicGraph.empty()) {
            if (auto g = assets::LoadMusicGraph(assets / settings.musicGraph)) musicEdit_ = *g;
        }
        if (musicEdit_.parameters.empty()) musicEdit_.parameters.push_back(MusicParameter{});
    }

    // Small char-buffer InputText helper (the project avoids the std::string backend).
    const auto editStr = [](const char* label, std::string& s, float width = 0.0f) {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", s.c_str());
        if (width > 0.0f) ImGui::SetNextItemWidth(width);
        if (ImGui::InputText(label, buf, sizeof(buf))) { s = buf; return true; }
        return false;
    };

    // --- Asset + save ---------------------------------------------------------
    editStr("Asset (.hbmusic)", musicEditPath_);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (musicEditPath_.empty()) musicEditPath_ = "Music/score.hbmusic";
        std::error_code ec;
        std::filesystem::create_directories((assets / musicEditPath_).parent_path(), ec);
        if (assets::SaveMusicGraph(assets / musicEditPath_, musicEdit_)) {
            settings.musicGraph = musicEditPath_;
            settings.musicStartState = musicEdit_.initialState;
            Project::Active().Save();
            buildResult_ = "Saved music graph '" + musicEditPath_ + "'.";
        }
    }
    ImGui::SliderFloat("Default fade (s)", &musicEdit_.defaultFade, 0.0f, 10.0f, "%.1f");
    // Initial state (played when the game starts).
    if (ImGui::BeginCombo("Start state", musicEdit_.initialState.c_str())) {
        for (const MusicState& s : musicEdit_.states)
            if (ImGui::Selectable(s.name.c_str(), s.name == musicEdit_.initialState))
                musicEdit_.initialState = s.name;
        ImGui::EndCombo();
    }

    // --- Parameters -----------------------------------------------------------
    ImGui::SeparatorText("Parameters (gameplay drives these 0..1 knobs)");
    int paramRemove = -1;
    for (int i = 0; i < static_cast<int>(musicEdit_.parameters.size()); ++i) {
        MusicParameter& p = musicEdit_.parameters[static_cast<usize>(i)];
        ImGui::PushID(i);
        editStr("##pname", p.name, 130.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat("min##p", &p.min, 0.01f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat("max##p", &p.max, 0.01f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("default##p", &p.defaultValue, p.min, p.max, "%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("X##p")) paramRemove = i;
        ImGui::PopID();
    }
    if (paramRemove >= 0) musicEdit_.parameters.erase(musicEdit_.parameters.begin() + paramRemove);
    if (ImGui::Button("Add Parameter")) musicEdit_.parameters.push_back(MusicParameter{});

    // --- States + layers ------------------------------------------------------
    ImGui::SeparatorText("States (sections)");
    if (ImGui::Button("Add State")) {
        MusicState s;
        s.name = "State" + std::to_string(musicEdit_.states.size() + 1);
        musicEdit_.states.push_back(std::move(s));
        musicStateSel_ = static_cast<int>(musicEdit_.states.size()) - 1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("a state's layers loop together; switching crossfades");

    // State tabs.
    musicStateSel_ = glm::clamp(musicStateSel_, 0, glm::max(0, static_cast<int>(musicEdit_.states.size()) - 1));
    for (int i = 0; i < static_cast<int>(musicEdit_.states.size()); ++i) {
        if (i > 0) ImGui::SameLine();
        const bool sel = musicStateSel_ == i;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button((musicEdit_.states[i].name + "##st" + std::to_string(i)).c_str()))
            musicStateSel_ = i;
        if (sel) ImGui::PopStyleColor();
    }

    if (musicStateSel_ < static_cast<int>(musicEdit_.states.size())) {
        MusicState& st = musicEdit_.states[static_cast<usize>(musicStateSel_)];
        ImGui::PushID(musicStateSel_ + 1000);
        editStr("Name##st", st.name, 160.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete State")) {
            musicEdit_.states.erase(musicEdit_.states.begin() + musicStateSel_);
            ImGui::PopID();
            ImGui::End();
            return; // indices shifted; redraw next frame
        }
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("BPM", &st.bpm, 0.5f, 1.0f, 400.0f, "%.0f");

        ImGui::TextDisabled("Layers (looping stems):");
        int layerRemove = -1;
        for (int li = 0; li < static_cast<int>(st.layers.size()); ++li) {
            MusicLayer& L = st.layers[static_cast<usize>(li)];
            ImGui::PushID(li);
            editStr("##lname", L.name, 110.0f);
            ImGui::SameLine();
            // Asset: type the path, or drag a .uaf from the Assets panel onto it.
            editStr("##lasset", L.asset, 180.0f);
            AssetDropTarget(".uaf", uaf::AssetType::Unknown,
                            [&](const std::filesystem::path& src) { L.asset = src.generic_string(); });
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::SliderFloat("vol##l", &L.volume, 0.0f, 1.5f, "%.2f");
            ImGui::SameLine();
            if (ImGui::SmallButton("X##l")) layerRemove = li;
            // Parameter binding: fades this layer in over [lo, hi] of the parameter.
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::BeginCombo("param##l", L.parameter.empty() ? "(always on)" : L.parameter.c_str())) {
                if (ImGui::Selectable("(always on)", L.parameter.empty())) L.parameter.clear();
                for (const MusicParameter& p : musicEdit_.parameters)
                    if (ImGui::Selectable(p.name.c_str(), p.name == L.parameter)) L.parameter = p.name;
                ImGui::EndCombo();
            }
            if (!L.parameter.empty()) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                ImGui::DragFloat("lo##l", &L.paramLo, 0.01f);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                ImGui::DragFloat("hi##l", &L.paramHi, 0.01f);
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (layerRemove >= 0) st.layers.erase(st.layers.begin() + layerRemove);
        if (ImGui::Button("Add Layer")) st.layers.push_back(MusicLayer{});
        ImGui::PopID();
    }

    // --- Live preview ---------------------------------------------------------
    ImGui::SeparatorText("Preview");
    if (!audio.IsAvailable()) {
        ImGui::TextDisabled("No audio device.");
    } else {
        if (ImGui::Button("Play state") && musicStateSel_ < static_cast<int>(musicEdit_.states.size())) {
            audio.SetMusicGraph(musicEdit_, assets); // install the working graph
            audio.PlayMusicState(musicEdit_.states[static_cast<usize>(musicStateSel_)].name);
            musicPreviewing_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            audio.StopMusic(0.5f);
            musicPreviewing_ = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(musicPreviewing_ ? "(now: %s)" : "(stopped)",
                            audio.CurrentMusicState().c_str());
        // Live parameter knobs - drag and hear the layers fade in/out.
        for (const MusicParameter& p : musicEdit_.parameters) {
            f32 v = audio.MusicParameterValue(p.name);
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat(p.name.c_str(), &v, p.min, p.max, "%.2f"))
                audio.SetMusicParameter(p.name, v);
        }
    }
    ImGui::End();
}

void Editor::DrawAudioMixer(Engine& engine) {
    if (!panelOpen_[Panel_AudioMixer]) return;
    AudioSystem& audio = engine.GetAudio();
    ImGui::Begin("Audio Mixer", &panelOpen_[Panel_AudioMixer]);
    if (!Project::HasActive()) {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }
    if (!audio.IsAvailable()) {
        ImGui::TextDisabled("No audio device.");
        ImGui::End();
        return;
    }

    ProjectSettings& settings = Project::Active().Settings();

    // First draw for this project: make sure the project lists its buses (so
    // they are editable + persistable) and push them to the audio system.
    if (!mixerSynced_) {
        mixerSynced_ = true;
        if (settings.audioBuses.empty()) {
            for (const AudioBusDesc& d : DefaultAudioBuses()) {
                settings.audioBuses.push_back({d.name, d.parent, d.volume, d.muted});
            }
        }
        std::vector<AudioBusDesc> descs;
        for (const AudioBusSetting& b : settings.audioBuses) {
            descs.push_back({b.name, b.parent, b.volume, b.muted});
        }
        audio.ConfigureBuses(descs);
    }

    const auto rebuild = [&]() {
        std::vector<AudioBusDesc> descs;
        for (const AudioBusSetting& b : settings.audioBuses) {
            descs.push_back({b.name, b.parent, b.volume, b.muted});
        }
        audio.ConfigureBuses(descs);
        Project::Active().Save();
    };

    // Master strip (implicit root; volume applies live, persisted with the rest).
    {
        f32 master = audio.BusVolume("Master");
        ImGui::TextUnformatted("Master");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::SliderFloat("##mastervol", &master, 0.0f, 2.0f, "%.2f")) {
            audio.SetBusVolume("Master", master);
        }
        ImGui::SameLine();
        bool muted = audio.BusMuted("Master");
        if (ImGui::Checkbox("Mute##master", &muted)) audio.SetBusMuted("Master", muted);
    }
    ImGui::Separator();

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(settings.audioBuses.size()); ++i) {
        AudioBusSetting& bus = settings.audioBuses[static_cast<usize>(i)];
        ImGui::PushID(i);
        ImGui::Text("%s", bus.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("-> %s", bus.parent.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::SliderFloat("##vol", &bus.volume, 0.0f, 2.0f, "%.2f")) {
            audio.SetBusVolume(bus.name, bus.volume);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) Project::Active().Save();
        ImGui::SameLine();
        if (ImGui::Checkbox("Mute", &bus.muted)) {
            audio.SetBusMuted(bus.name, bus.muted);
            Project::Active().Save();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) removeIndex = i;
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        settings.audioBuses.erase(settings.audioBuses.begin() + removeIndex);
        rebuild();
    }

    ImGui::Separator();
    static char newBusName[64] = "";
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##newbus", "new bus name", newBusName, sizeof(newBusName));
    ImGui::SameLine();
    static int parentIndex = 0;
    std::vector<std::string> parents = audio.BusNames();
    if (parents.empty()) parents.push_back("Master");
    parentIndex = glm::clamp(parentIndex, 0, static_cast<int>(parents.size()) - 1);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##newbusparent", parents[static_cast<usize>(parentIndex)].c_str())) {
        for (int i = 0; i < static_cast<int>(parents.size()); ++i) {
            if (ImGui::Selectable(parents[static_cast<usize>(i)].c_str(), i == parentIndex)) {
                parentIndex = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Bus") && newBusName[0] != '\0') {
        const std::string name = newBusName;
        const bool exists =
            name == "Master" ||
            std::any_of(settings.audioBuses.begin(), settings.audioBuses.end(),
                        [&](const AudioBusSetting& b) { return b.name == name; });
        if (!exists) {
            settings.audioBuses.push_back(
                {name, parents[static_cast<usize>(parentIndex)], 1.0f, false});
            newBusName[0] = '\0';
            rebuild();
        }
    }
    ImGui::End();
}

bool Editor::ApplyMaterialToEntity(Engine& engine, entt::entity e,
                                   const std::filesystem::path& hbmat) {
    auto& reg = engine.GetScene().Registry();
    if (!reg.valid(e)) return false;
    MeshInstance* mi = reg.try_get<MeshInstance>(e);
    if (!mi) return false;
    const std::optional<MaterialAsset> mat = assets::LoadMaterial(hbmat);
    if (!mat) return false;
    PushUndo(engine.GetScene());
    assets::ApplyMaterial(engine.GetRenderer(), Project::Active().AssetsDir(), *mat,
                          *mi, textureCache_);
    reg.emplace_or_replace<MaterialRef>(
        e, MaterialRef{Project::Active().RelativeAssetPath(hbmat)});
    return true;
}

namespace {
// Case-insensitive "ends with" / "contains" for texture naming conventions.
bool EndsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

std::filesystem::path Editor::MaterialFromTexture(const std::filesystem::path& texture) {
    if (!Project::HasActive()) return {};
    const std::filesystem::path dir = texture.parent_path();
    const std::string stem = texture.stem().string();

    // Strip a known albedo suffix so sibling maps share the prefix
    // ("brick_BaseColor" -> "brick" pairs with "brick_Normal" etc).
    static constexpr const char* kAlbedoSuffixes[] = {
        "_BaseColor", "_Basecolor", "_Albedo", "_Diffuse", "_Color", "_Diff", "_D"};
    std::string prefix = stem;
    for (const char* suffix : kAlbedoSuffixes) {
        if (EndsWithNoCase(stem, suffix)) {
            prefix = stem.substr(0, stem.size() - std::strlen(suffix));
            break;
        }
    }

    const std::filesystem::path matPath = dir / (prefix + "_Mat.hbmat");
    std::error_code ec;
    if (std::filesystem::exists(matPath, ec)) return matPath; // reuse

    MaterialAsset mat;
    mat.name = prefix;
    mat.albedoTex = Project::Active().RelativeAssetPath(texture);
    mat.roughness = 0.8f; // sensible non-shiny default until maps say otherwise

    // Sibling maps in the same folder, matched by prefix + role keywords.
    const std::string prefixLower = Lower(prefix);
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".uaf") continue;
        if (entry.path() == texture) continue;
        const std::string nameLower = Lower(entry.path().stem().string());
        if (nameLower.rfind(prefixLower, 0) != 0) continue; // shared prefix only
        const std::string rel = Project::Active().RelativeAssetPath(entry.path());
        if (mat.normalTex.empty() && nameLower.find("normal") != std::string::npos) {
            mat.normalTex = rel;
        } else if (mat.mrTex.empty() && (nameLower.find("roughness") != std::string::npos ||
                                         nameLower.find("metal") != std::string::npos)) {
            mat.mrTex = rel;
            mat.metallic = 1.0f; // factors multiply the map (glTF convention)
            mat.roughness = 1.0f;
        } else if (mat.aoTex.empty() && (nameLower.find("occlusion") != std::string::npos ||
                                         EndsWithNoCase(nameLower, "_ao"))) {
            mat.aoTex = rel;
        } else if (mat.emissiveTex.empty() &&
                   nameLower.find("emiss") != std::string::npos) {
            mat.emissiveTex = rel;
            mat.emissiveColor = glm::vec3(1.0f);
        }
    }

    if (!assets::SaveMaterial(matPath, mat)) return {};
    assetsDirty_ = true; // show the new .hbmat in the browser
    HBE_INFO("Editor: auto-created material '{}' from texture.", matPath.string());
    return matPath;
}

bool Editor::ApplyAssetDropToEntity(Engine& engine, entt::entity target,
                                    const std::filesystem::path& asset) {
    Scene& scene = engine.GetScene();
    auto& reg = scene.Registry();
    if (target == entt::null || !reg.valid(target) || !Project::HasActive()) return false;
    const std::string ext = asset.extension().string();
    const std::string rel = Project::Active().RelativeAssetPath(asset);

    if (ext == ".hbmat") {
        if (ApplyMaterialToEntity(engine, target, asset)) {
            selected_ = target;
            return true;
        }
        return false;
    }
    if (ext == ".hbevent") {
        // Events aren't components; audition them at the entity's position.
        if (const auto ev = assets::LoadAudioEvent(asset)) {
            const glm::vec3 pos = glm::vec3(scene.WorldMatrix(target)[3]);
            engine.GetAudio().PostEvent(*ev, Project::Active().AssetsDir(), &pos);
        }
        return true;
    }
    if (ext != ".uaf") return false;

    switch (uaf::PeekType(asset)) {
        case uaf::AssetType::Texture: {
            // UI elements take the image directly; meshes get an auto material.
            if (UIElement* el = reg.try_get<UIElement>(target)) {
                PushUndo(scene);
                el->texture = rel;
                el->textureResolved = false;
                selected_ = target;
                return true;
            }
            if (reg.all_of<MeshInstance>(target)) {
                const std::filesystem::path mat = MaterialFromTexture(asset);
                if (!mat.empty() && ApplyMaterialToEntity(engine, target, mat)) {
                    selected_ = target;
                    return true;
                }
            }
            return false;
        }
        case uaf::AssetType::Audio: {
            PushUndo(scene);
            AudioSource src;
            if (const AudioSource* old = reg.try_get<AudioSource>(target)) {
                src = *old; // keep tuning; swap the clip
                src.voiceId = AudioSource::kNoVoice;
                src.playing = false;
            }
            src.asset = rel;
            reg.emplace_or_replace<AudioSource>(target, src);
            selected_ = target;
            return true;
        }
        case uaf::AssetType::Font: {
            if (UIElement* el = reg.try_get<UIElement>(target)) {
                PushUndo(scene);
                el->font = rel;
                selected_ = target;
                return true;
            }
            return false;
        }
        case uaf::AssetType::Mesh: {
            // Spawn the model as a child of the drop target.
            const entt::entity spawned =
                SpawnMeshAsset(scene, engine.GetRenderer(), asset, /*frameCamera=*/false);
            if (spawned != entt::null) {
                Reparent(scene, spawned, target);
                selected_ = spawned;
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

template <typename Fn>
void Editor::AssetDropTarget(const char* extension, uaf::AssetType uafType, Fn&& apply) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HBE_ASSET_PATH")) {
        const std::filesystem::path src(static_cast<const char*>(p->Data));
        const bool extMatch = src.extension() == extension;
        const bool typeMatch = uafType == uaf::AssetType::Unknown ||
                               (extMatch && extension == std::string(".uaf") &&
                                uaf::PeekType(src) == uafType);
        if (extMatch && (uafType == uaf::AssetType::Unknown || typeMatch)) {
            apply(src);
        }
    }
    ImGui::EndDragDropTarget();
}

std::vector<std::string> Editor::ListAssetsByExt(const char* extension,
                                                 uaf::AssetType typeFilter) const {
    std::vector<std::string> out;
    if (!Project::HasActive()) return out;
    const std::filesystem::path root = Project::Active().AssetsDir();
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file() || it->path().extension() != extension) continue;
        if (typeFilter != uaf::AssetType::Unknown &&
            uaf::PeekType(it->path()) != typeFilter) {
            continue;
        }
        out.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool Editor::AssetPicker(const char* label, const std::string& current, const char* extension,
                         uaf::AssetType uafType, std::string& out, const char* noneLabel) {
    bool changed = false;
    ImGui::PushID(label);
    const std::string preview =
        current.empty() ? (noneLabel ? noneLabel : "(none)") : current;
    if (ImGui::Button(preview.c_str())) {
        assetPickerSearch_[0] = '\0';
        ImGui::OpenPopup("##assetpicker");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);

    if (ImGui::BeginPopup("##assetpicker")) {
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##search", "search...", assetPickerSearch_,
                                 sizeof(assetPickerSearch_));
        ImGui::Separator();
        if (ImGui::BeginChild("##list", ImVec2(280.0f, 260.0f))) {
            if (noneLabel && ImGui::Selectable(noneLabel, current.empty())) {
                out.clear();
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            const auto lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return s;
            };
            const std::string needle = lower(assetPickerSearch_);
            for (const std::string& rel : ListAssetsByExt(extension, uafType)) {
                if (!needle.empty() && lower(rel).find(needle) == std::string::npos) continue;
                if (ImGui::Selectable(rel.c_str(), rel == current)) {
                    out = rel;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

namespace {
const char* FormatName(u32 format) {
    switch (static_cast<rhi::Format>(format)) {
        case rhi::Format::R8G8B8A8_UNORM: return "RGBA8 (linear)";
        case rhi::Format::R8G8B8A8_SRGB:  return "RGBA8 (sRGB)";
        case rhi::Format::B8G8R8A8_UNORM: return "BGRA8 (linear)";
        case rhi::Format::B8G8R8A8_SRGB:  return "BGRA8 (sRGB)";
        case rhi::Format::R16G16B16A16_FLOAT: return "RGBA16F";
        case rhi::Format::R32G32B32A32_FLOAT: return "RGBA32F";
        default: return "unknown";
    }
}

std::string HumanSize(u64 bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    }
    return buf;
}

// Uploads RGBA8 pixels and returns the ImGui id (0 on failure).
u64 UploadPreview(Renderer& renderer, const std::vector<u32>& px, u32 w, u32 h,
                  rhi::Format format = rhi::Format::R8G8B8A8_UNORM) {
    rhi::TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = format;
    d.pixels = px.data();
    d.debugName = "asset_preview";
    return renderer.TextureUIId(renderer.UploadTexture(d));
}

// Nearest-downsamples 4-byte texel data to fit maxDim (no-op when smaller).
std::vector<u32> DownsamplePixels(const u8* src, u32 sw, u32 sh, u32 maxDim,
                                  u32& dw, u32& dh) {
    dw = sw;
    dh = sh;
    if (dw > maxDim || dh > maxDim) {
        const f32 scale = static_cast<f32>(glm::max(dw, dh)) / maxDim;
        dw = glm::max(1u, static_cast<u32>(dw / scale));
        dh = glm::max(1u, static_cast<u32>(dh / scale));
    }
    std::vector<u32> px(static_cast<usize>(dw) * dh);
    const u32* s = reinterpret_cast<const u32*>(src);
    for (u32 y = 0; y < dh; ++y) {
        const u32 sy = y * sh / dh;
        for (u32 x = 0; x < dw; ++x) {
            px[y * dw + x] = s[static_cast<usize>(sy) * sw + (x * sw / dw)];
        }
    }
    return px;
}
} // namespace

void Editor::EnsureMeshPreview(Engine& engine) {
    if (previewPath_ == viewedAsset_) return;
    previewPath_ = viewedAsset_;
    previewMeshes_.clear();
    previewInstances_.clear();
    previewModel_.clear();
    previewDraw_.clear();
    previewMeshDirty_ = false;

    const std::optional<Model> model = assets::LoadMesh(viewedAsset_);
    if (!model) return;
    previewModel_ = *model;

    Renderer& renderer = engine.GetRenderer();
    const std::filesystem::path assetsDir = Project::Active().AssetsDir();
    const auto loadTex = [&](const std::string& name) -> rhi::TextureHandle {
        if (name.empty()) return {};
        if (auto it = textureCache_.find(name); it != textureCache_.end()) return it->second;
        const rhi::TextureHandle h = assets::LoadTexture(renderer, assetsDir / name);
        textureCache_[name] = h;
        return h;
    };

    glm::vec3 mn(1e30f), mx(-1e30f);
    for (MeshData& md : previewModel_) {
        previewMeshes_.push_back(renderer.UploadMesh(md));

        MeshInstance mi;
        if (!md.material.materialAsset.empty()) {
            if (const auto mat = assets::LoadMaterial(assetsDir / md.material.materialAsset)) {
                assets::ApplyMaterial(renderer, assetsDir, *mat, mi, textureCache_);
            }
        } else {
            mi.baseColor = md.material.baseColor;
            mi.metallic = md.material.metallic;
            mi.roughness = md.material.roughness;
            mi.emissiveColor = md.material.emissive;
            mi.albedoTexture = loadTex(md.material.baseColorTex);
            mi.normalTexture = loadTex(md.material.normalTex);
            mi.mrTexture = loadTex(md.material.mrTex);
            mi.aoTexture = loadTex(md.material.aoTex);
            mi.emissiveTexture = loadTex(md.material.emissiveTex);
        }
        previewInstances_.push_back(mi);

        glm::vec3 a, b;
        ComputeBounds(md, a, b);
        mn = glm::min(mn, a);
        mx = glm::max(mx, b);
    }
    previewCenter_ = (mn + mx) * 0.5f;
    previewRadius_ = glm::max(glm::length(mx - mn) * 0.5f, 0.01f);
    previewYaw_ = 0.8f;
    previewPitch_ = 0.35f;
    previewZoom_ = 2.4f;
}

void Editor::DrawAssetViewer(Engine& engine) {
    if (!panelOpen_[Panel_AssetViewer]) return;
    Renderer& renderer = engine.GetRenderer();
    Scene& scene = engine.GetScene();
    ImGui::Begin("Asset Viewer", &panelOpen_[Panel_AssetViewer]);

    std::error_code ec;
    if (!Project::HasActive() || viewedAsset_.empty() ||
        !std::filesystem::exists(viewedAsset_, ec)) {
        ImGui::TextDisabled("Click an asset in the Assets panel to inspect it.");
        ImGui::End();
        return;
    }

    const std::string ext = viewedAsset_.extension().string();
    const bool isMat = ext == ".hbmat";
    const bool isScene = ext == ".hbscene";
    const bool isEvent = ext == ".hbevent";
    const bool isUaf = ext == ".uaf";
    static std::vector<std::string> texChoices;   // rebuilt when the viewer re-targets
    static std::vector<std::string> audioChoices; // .uaf audio assets for events

    // (Re)build the cached preview + info lines.
    if (viewerDirty_) {
        viewerDirty_ = false;
        viewerInfo_.clear();
        viewerPreviewId_ = 0;
        const u64 fileSize = std::filesystem::file_size(viewedAsset_, ec);
        const std::string cacheKey = viewedAsset_.string();

        if (isUaf) {
            const uaf::AssetType type = uaf::PeekType(viewedAsset_);
            viewedTypeName_ = uaf::ToString(type);
            if (type == uaf::AssetType::Texture) {
                if (const auto tex = uaf::ReadTexture(viewedAsset_)) {
                    viewerInfo_.push_back(std::to_string(tex->width) + " x " +
                                          std::to_string(tex->height) + "  " +
                                          FormatName(tex->format));
                    viewerInfo_.push_back(std::to_string(tex->mipCount) + " mip level(s), " +
                                          HumanSize(fileSize));
                    if (auto it = previewCache_.find(cacheKey); it != previewCache_.end()) {
                        viewerPreviewId_ = it->second;
                    } else if (tex->pixels.size() >=
                               static_cast<usize>(tex->width) * tex->height * 4) {
                        u32 dw = 0, dh = 0;
                        const auto px = DownsamplePixels(tex->pixels.data(), tex->width,
                                                         tex->height, 384, dw, dh);
                        // UNORM pass-through: ImGui does no gamma encode.
                        const auto fmt = static_cast<rhi::Format>(tex->format);
                        const bool bgra = fmt == rhi::Format::B8G8R8A8_UNORM ||
                                          fmt == rhi::Format::B8G8R8A8_SRGB;
                        viewerPreviewId_ = UploadPreview(
                            renderer, px, dw, dh,
                            bgra ? rhi::Format::B8G8R8A8_UNORM
                                 : rhi::Format::R8G8B8A8_UNORM);
                        previewCache_[cacheKey] = viewerPreviewId_;
                    }
                }
            } else if (type == uaf::AssetType::Mesh) {
                if (const auto model = assets::LoadMesh(viewedAsset_)) {
                    u64 verts = 0, inds = 0;
                    for (const MeshData& md : *model) {
                        verts += md.vertices.size();
                        inds += md.indices.size();
                    }
                    viewerInfo_.push_back(std::to_string(model->size()) + " submesh(es), " +
                                          std::to_string(verts) + " verts, " +
                                          std::to_string(inds / 3) + " tris, " +
                                          HumanSize(fileSize));
                }
                // The interactive 3D preview below replaces the old raster image.
            } else if (type == uaf::AssetType::Audio) {
                if (const auto audio = uaf::ReadAudio(viewedAsset_)) {
                    const u64 bytesPerSec = static_cast<u64>(audio->sampleRate) *
                                            audio->channels * (audio->bitsPerSample / 8);
                    const f64 seconds =
                        bytesPerSec ? static_cast<f64>(audio->pcm.size()) / bytesPerSec : 0.0;
                    char line[128];
                    std::snprintf(line, sizeof(line), "%u ch, %u Hz, %u-bit, %.2f s",
                                  audio->channels, audio->sampleRate,
                                  audio->bitsPerSample, seconds);
                    viewerInfo_.push_back(line);
                    viewerInfo_.push_back(HumanSize(fileSize));
                    // Cache the editable tags for the inspector below.
                    viewedAudioKind_ = static_cast<int>(audio->kind);
                    std::snprintf(viewedAudioCaption_, sizeof(viewedAudioCaption_), "%s",
                                  audio->caption.c_str());
                }
            }
        } else if (isMat) {
            viewedTypeName_ = "Material";
            if (const auto mat = assets::LoadMaterial(viewedAsset_)) {
                editedMat_ = *mat;
                editedMatValid_ = true;
                editedMatDirty_ = false;
            }
            texChoices = ListAssetsByExt(".uaf", uaf::AssetType::Texture);
        } else if (isScene) {
            viewedTypeName_ = "Scene";
            scene::SceneData data;
            if (scene::ParseSceneFile(viewedAsset_, data)) {
                viewerInfo_.push_back(std::to_string(data.entities.size()) + " entities, " +
                                      HumanSize(fileSize));
            }
        } else if (isEvent) {
            viewedTypeName_ = "Audio Event";
            editedEventValid_ = false;
            if (const auto ev = assets::LoadAudioEvent(viewedAsset_)) {
                editedEvent_ = *ev;
                editedEventValid_ = true;
                editedEventDirty_ = false;
            }
            audioChoices = ListAssetsByExt(".uaf", uaf::AssetType::Audio);
        }
    }

    ImGui::Text("%s", viewedAsset_.filename().string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", viewedTypeName_.c_str());
    ImGui::Separator();

    // --- Material editor (full PBR) -----------------------------------------
    if (isMat && editedMatValid_) {
        // Live sphere preview, re-rasterized after edits.
        static u64 matPreviewId = 0;
        static std::string matPreviewKey;
        static bool matPreviewStale = false;
        const std::string key = viewedAsset_.string();
        if (matPreviewKey != key || matPreviewStale) {
            matPreviewKey = key;
            matPreviewStale = false;
            const glm::vec3 tint =
                glm::clamp(glm::vec3(editedMat_.baseColor) +
                               editedMat_.emissiveColor * editedMat_.emissiveIntensity * 0.25f,
                           glm::vec3(0.02f), glm::vec3(1.0f));
            Model sphere{mesh::GenerateSphere(0.5f, 32, 16)};
            const auto px = editor::RasterizeMeshThumbnail(sphere, 192, tint);
            matPreviewId = UploadPreview(renderer, px, 192, 192);
        }
        if (matPreviewId != 0) {
            const f32 indent = glm::max((ImGui::GetContentRegionAvail().x - 192.0f) * 0.5f, 0.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            ImGui::Image(static_cast<ImTextureID>(matPreviewId), ImVec2(192, 192));
        }

        bool edited = false;
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", editedMat_.name.c_str());
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            editedMat_.name = nameBuf;
            edited = true;
        }
        edited |= ImGui::ColorEdit4("Base Color", glm::value_ptr(editedMat_.baseColor));
        edited |= ImGui::SliderFloat("Metallic", &editedMat_.metallic, 0.0f, 1.0f);
        edited |= ImGui::SliderFloat("Roughness", &editedMat_.roughness, 0.04f, 1.0f);
        edited |= ImGui::ColorEdit3("Emissive", glm::value_ptr(editedMat_.emissiveColor));
        edited |= ImGui::DragFloat("Emissive Intensity", &editedMat_.emissiveIntensity,
                                   0.05f, 0.0f, 100.0f);
        bool sss = (editedMat_.flags & rhi::MaterialFlag_Subsurface) != 0u;
        if (ImGui::Checkbox("Subsurface (skin)", &sss)) {
            if (sss) editedMat_.flags |= rhi::MaterialFlag_Subsurface;
            else     editedMat_.flags &= ~static_cast<u32>(rhi::MaterialFlag_Subsurface);
            edited = true;
        }
        if (sss) {
            edited |= ImGui::ColorEdit3("Subsurface Color",
                                        glm::value_ptr(editedMat_.subsurfaceColor));
            edited |= ImGui::SliderFloat("Scatter radius", &editedMat_.subsurfaceRadius,
                                         0.1f, 4.0f, "%.2f");
        }
        const auto matFlag = [&](const char* label, u32 bit) {
            bool on = (editedMat_.flags & bit) != 0u;
            if (ImGui::Checkbox(label, &on)) {
                if (on) editedMat_.flags |= bit;
                else    editedMat_.flags &= ~bit;
                edited = true;
            }
        };
        matFlag("Cloth (fabric sheen)", rhi::MaterialFlag_Cloth);
        matFlag("Eye (parallax iris)", rhi::MaterialFlag_Eye);
        matFlag("Transparent (alpha blend)", rhi::MaterialFlag_Transparent);
        // Cast shadow is the inverse of the NoShadow flag (free-standing strokes
        // turn this off so they don't shadow the surface they float over).
        bool castShadow = (editedMat_.flags & rhi::MaterialFlag_NoShadow) == 0u;
        if (ImGui::Checkbox("Cast shadow", &castShadow)) {
            if (castShadow) editedMat_.flags &= ~static_cast<u32>(rhi::MaterialFlag_NoShadow);
            else            editedMat_.flags |= rhi::MaterialFlag_NoShadow;
            edited = true;
        }

        ImGui::SeparatorText("Texture maps");
        // One filter buffer is enough: only a single combo popup is ever open at once.
        static char texFilter[64] = "";
        const auto texMatches = [](const std::string& choice, const char* filter) {
            if (!filter[0]) return true;
            std::string h = choice, n = filter;
            std::transform(h.begin(), h.end(), h.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(n.begin(), n.end(), n.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return h.find(n) != std::string::npos;
        };
        const auto texPicker = [&](const char* label, std::string& ref) {
            const std::string current = ref.empty() ? "(none)" : ref;
            if (ImGui::BeginCombo(label, current.c_str())) {
                // A search box at the top of the list (texture libraries get large).
                // Reset + focus it each time this picker opens so you can just type.
                if (ImGui::IsWindowAppearing()) {
                    texFilter[0] = '\0';
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                ImGui::InputTextWithHint("##texfilter", "Search...", texFilter, sizeof(texFilter));
                if (ImGui::Selectable("(none)", ref.empty())) {
                    ref.clear();
                    edited = true;
                }
                for (const std::string& choice : texChoices) {
                    if (!texMatches(choice, texFilter)) continue;
                    if (ImGui::Selectable(choice.c_str(), choice == ref)) {
                        ref = choice;
                        edited = true;
                    }
                }
                ImGui::EndCombo();
            }
            AssetDropTarget(".uaf", uaf::AssetType::Texture,
                            [&](const std::filesystem::path& src) {
                                ref = Project::Active().RelativeAssetPath(src);
                                edited = true;
                            });
        };
        texPicker("Albedo", editedMat_.albedoTex);
        texPicker("Normal", editedMat_.normalTex);
        texPicker("Metal/Rough", editedMat_.mrTex);
        texPicker("AO", editedMat_.aoTex);
        texPicker("Emissive Map", editedMat_.emissiveTex);

        if (edited) {
            editedMatDirty_ = true;
            matPreviewStale = true;
        }

        ImGui::Separator();
        if (ImGui::Button(editedMatDirty_ ? "Save*" : "Save")) {
            if (assets::SaveMaterial(viewedAsset_, editedMat_)) {
                editedMatDirty_ = false;
                // Refresh every entity wearing this material.
                const std::string rel = Project::Active().RelativeAssetPath(viewedAsset_);
                auto& reg = scene.Registry();
                for (const entt::entity e : reg.view<MaterialRef, MeshInstance>()) {
                    if (reg.get<MaterialRef>(e).asset == rel) {
                        assets::ApplyMaterial(renderer, Project::Active().AssetsDir(),
                                              editedMat_, reg.get<MeshInstance>(e),
                                              textureCache_);
                    }
                }
            }
        }
        ImGui::SameLine();
        const bool hasMeshSel = selected_ != entt::null && scene.Registry().valid(selected_) &&
                                scene.Registry().all_of<MeshInstance>(selected_);
        ImGui::BeginDisabled(!hasMeshSel);
        if (ImGui::Button("Apply to selected")) {
            // Apply the WORKING copy (saved or not) and link the asset.
            assets::ApplyMaterial(renderer, Project::Active().AssetsDir(), editedMat_,
                                  scene.Registry().get<MeshInstance>(selected_),
                                  textureCache_);
            scene.Registry().emplace_or_replace<MaterialRef>(
                selected_, MaterialRef{Project::Active().RelativeAssetPath(viewedAsset_)});
        }
        ImGui::EndDisabled();
        if (editedMatDirty_) {
            ImGui::SameLine();
            ImGui::TextDisabled("(unsaved changes)");
        }
        ImGui::End();
        return;
    }

    // --- Audio event editor (FMOD-style weighted pool + bus routing) -----------
    if (isEvent && editedEventValid_) {
        AudioSystem& audio = engine.GetAudio();
        bool edited = false;

        // Mixer bus routing.
        {
            const std::vector<std::string> buses = audio.BusNames();
            if (ImGui::BeginCombo("Bus", editedEvent_.bus.c_str())) {
                for (const std::string& b : buses) {
                    if (ImGui::Selectable(b.c_str(), b == editedEvent_.bus)) {
                        editedEvent_.bus = b;
                        edited = true;
                    }
                }
                ImGui::EndCombo();
            }
        }

        ImGui::SeparatorText("Sounds (weighted random pool)");
        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(editedEvent_.sounds.size()); ++i) {
            AudioEventSound& s = editedEvent_.sounds[static_cast<usize>(i)];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
            const std::string current = s.asset.empty() ? "(none)" : s.asset;
            if (ImGui::BeginCombo("##snd", current.c_str())) {
                for (const std::string& choice : audioChoices) {
                    if (ImGui::Selectable(choice.c_str(), choice == s.asset)) {
                        s.asset = choice;
                        edited = true;
                    }
                }
                ImGui::EndCombo();
            }
            AssetDropTarget(".uaf", uaf::AssetType::Audio,
                            [&](const std::filesystem::path& src) {
                                s.asset = Project::Active().RelativeAssetPath(src);
                                edited = true;
                            });
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            edited |= ImGui::DragFloat("##wt", &s.weight, 0.05f, 0.0f, 100.0f, "w %.2f");
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) removeIndex = i;
            ImGui::PopID();
        }
        if (removeIndex >= 0) {
            editedEvent_.sounds.erase(editedEvent_.sounds.begin() + removeIndex);
            edited = true;
        }
        if (ImGui::SmallButton("+ Add sound")) {
            editedEvent_.sounds.push_back({});
            edited = true;
        }

        ImGui::SeparatorText("Playback");
        edited |= ImGui::SliderFloat("Volume", &editedEvent_.volume, 0.0f, 2.0f);
        edited |= ImGui::SliderFloat("Volume Variance", &editedEvent_.volumeVariance, 0.0f, 1.0f);
        edited |= ImGui::SliderFloat("Pitch", &editedEvent_.pitch, 0.25f, 4.0f);
        edited |= ImGui::SliderFloat("Pitch Variance", &editedEvent_.pitchVariance, 0.0f, 1.0f);
        edited |= ImGui::Checkbox("Loop", &editedEvent_.loop);
        edited |= ImGui::Checkbox("Spatial (3D)", &editedEvent_.spatial);
        if (editedEvent_.spatial) {
            edited |= ImGui::DragFloat("Min Distance", &editedEvent_.minDistance, 0.1f,
                                       0.01f, 1000.0f);
            edited |= ImGui::DragFloat("Max Distance", &editedEvent_.maxDistance, 0.1f,
                                       0.01f, 1000.0f);
        }
        if (edited) editedEventDirty_ = true;

        ImGui::Separator();
        if (ImGui::Button("Post (test)")) {
            audio.StopEvent(lastPostedVoice_); // re-trigger replaces the test voice
            lastPostedVoice_ =
                audio.PostEvent(editedEvent_, Project::Active().AssetsDir());
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            audio.StopEvent(lastPostedVoice_);
            lastPostedVoice_ = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(editedEventDirty_ ? "Save*" : "Save")) {
            if (assets::SaveAudioEvent(viewedAsset_, editedEvent_)) {
                editedEventDirty_ = false;
            }
        }
        if (editedEventDirty_) {
            ImGui::SameLine();
            ImGui::TextDisabled("(unsaved changes)");
        }
        ImGui::End();
        return;
    }

    // --- Mesh editor: interactive 3D preview + material slots (Unreal-style) --
    if (isUaf && viewedTypeName_ == "Mesh") {
        EnsureMeshPreview(engine);
        for (const std::string& line : viewerInfo_) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (!previewMeshes_.empty()) {
            // The preview image: LMB-drag orbits, mouse wheel zooms.
            const f32 avail = glm::max(ImGui::GetContentRegionAvail().x, 64.0f);
            const ImVec2 imgSize(avail, glm::max(avail * 0.75f, 64.0f));
            renderer.SetPreviewSize(static_cast<u32>(imgSize.x),
                                    static_cast<u32>(imgSize.y));
            if (const u64 texId = renderer.PreviewTextureId()) {
                ImGui::Image(static_cast<ImTextureID>(texId), imgSize);
                if (ImGui::IsItemHovered()) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                        previewYaw_ -= io.MouseDelta.x * 0.012f;
                        previewPitch_ = glm::clamp(
                            previewPitch_ + io.MouseDelta.y * 0.012f, -1.45f, 1.45f);
                    }
                    if (io.MouseWheel != 0.0f) {
                        previewZoom_ = glm::clamp(
                            previewZoom_ * std::pow(0.9f, io.MouseWheel), 1.05f, 12.0f);
                    }
                }
                ImGui::TextDisabled("Drag to orbit, wheel to zoom.");
            } else {
                ImGui::TextDisabled("(preview initializing...)");
            }

            // Submit this frame's preview scene (orbit camera around bounds,
            // lit by the scene's environment + sky).
            {
                const f32 cp = std::cos(previewPitch_), sp = std::sin(previewPitch_);
                const f32 cy = std::cos(previewYaw_), sy = std::sin(previewYaw_);
                const f32 dist = previewZoom_ * previewRadius_;
                const glm::vec3 eye =
                    previewCenter_ + glm::vec3(cy * cp, sp, sy * cp) * dist;
                Camera cam;
                cam.SetPerspective(50.0f, imgSize.x / imgSize.y,
                                   glm::max(previewRadius_ * 0.01f, 0.001f),
                                   glm::max(previewRadius_ * 60.0f, 10.0f));
                cam.LookAt(eye, previewCenter_);

                const SceneEnvironment& env = scene.Environment();
                rhi::SceneView pv;
                pv.viewProj = cam.ViewProjection();
                pv.invViewProj = glm::inverse(pv.viewProj);
                pv.cameraPos = eye;
                pv.exposure = 1.0f;
                pv.ambientIntensity = env.ambientIntensity;
                pv.light.direction = glm::normalize(env.sun.direction);
                pv.light.color = env.sun.color;
                pv.light.intensity = env.sun.intensity;
                pv.irradianceIndex = env.irradiance.index;
                pv.prefilteredIndex = env.prefiltered.index;
                pv.brdfLUTIndex = env.brdfLUT.index;
                pv.prefilteredMaxLod = env.prefilteredMaxLod;
                pv.skyIndex = env.sky.index;

                previewDraw_.clear();
                for (usize i = 0; i < previewMeshes_.size(); ++i) {
                    if (!previewMeshes_[i].IsValid()) continue;
                    const MeshInstance& mi = previewInstances_[i];
                    rhi::DrawItem item;
                    item.mesh = previewMeshes_[i];
                    item.baseColor = mi.baseColor;
                    item.metallic = mi.metallic;
                    item.roughness = mi.roughness;
                    item.albedoTexture = mi.albedoTexture;
                    item.normalTexture = mi.normalTexture;
                    item.mrTexture = mi.mrTexture;
                    item.aoTexture = mi.aoTexture;
                    item.emissiveTexture = mi.emissiveTexture;
                    item.emissiveColor = mi.emissiveColor;
                    item.emissiveIntensity = mi.emissiveIntensity;
                    item.subsurfaceColor = mi.subsurfaceColor;
                    item.subsurfaceRadius = mi.subsurfaceRadius;
                    item.thicknessTexture = mi.thicknessTexture;
                    item.materialFlags = mi.materialFlags;
                    previewDraw_.push_back(item);
                }
                renderer.SetPreviewScene(pv, previewDraw_);
                previewSubmitted_ = true; // the mesh preview owns the slot this frame
            }

            // Material slots: one .hbmat picker per submesh; Save writes the
            // refs back into the .uaf (like Unreal's static-mesh editor).
            ImGui::SeparatorText("Material slots");
            static std::vector<std::string> matChoices;
            static bool matChoicesScanned = false;
            if (!matChoicesScanned) {
                matChoices = ListAssetsByExt(".hbmat");
                matChoicesScanned = true;
            }
            if (ImGui::IsWindowAppearing()) matChoicesScanned = false;
            const std::filesystem::path assetsDir = Project::Active().AssetsDir();
            for (usize i = 0; i < previewModel_.size(); ++i) {
                MeshData& md = previewModel_[i];
                ImGui::PushID(static_cast<int>(i));
                const std::string label =
                    "#" + std::to_string(i) + " " +
                    (md.name.empty() ? "(unnamed)" : md.name);
                const std::string current = md.material.materialAsset.empty()
                                                ? "(inline material)"
                                                : md.material.materialAsset;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
                if (ImGui::BeginCombo(label.c_str(), current.c_str())) {
                    if (ImGui::Selectable("(inline material)",
                                          md.material.materialAsset.empty())) {
                        md.material.materialAsset.clear();
                        previewMeshDirty_ = true;
                        previewInstances_[i] = MeshInstance{};
                        previewInstances_[i].baseColor = md.material.baseColor;
                        previewInstances_[i].metallic = md.material.metallic;
                        previewInstances_[i].roughness = md.material.roughness;
                        previewInstances_[i].emissiveColor = md.material.emissive;
                    }
                    for (const std::string& choice : matChoices) {
                        if (ImGui::Selectable(choice.c_str(),
                                              choice == md.material.materialAsset)) {
                            md.material.materialAsset = choice;
                            previewMeshDirty_ = true;
                            if (const auto mat = assets::LoadMaterial(assetsDir / choice)) {
                                previewInstances_[i] = MeshInstance{};
                                assets::ApplyMaterial(renderer, assetsDir, *mat,
                                                      previewInstances_[i], textureCache_);
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                AssetDropTarget(".hbmat", uaf::AssetType::Unknown,
                                [&](const std::filesystem::path& src) {
                                    md.material.materialAsset =
                                        Project::Active().RelativeAssetPath(src);
                                    previewMeshDirty_ = true;
                                    if (const auto mat = assets::LoadMaterial(src)) {
                                        previewInstances_[i] = MeshInstance{};
                                        assets::ApplyMaterial(renderer, assetsDir, *mat,
                                                              previewInstances_[i],
                                                              textureCache_);
                                    }
                                });
                ImGui::PopID();
            }

            ImGui::Separator();
            if (ImGui::Button(previewMeshDirty_ ? "Save Mesh*" : "Save Mesh")) {
                if (uaf::WriteMesh(viewedAsset_, previewModel_)) {
                    previewMeshDirty_ = false;
                    // Resident copies of this mesh may now be stale.
                    scene::ClearInstantiateCaches();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Spawn in scene")) {
                SpawnMeshAsset(scene, renderer, viewedAsset_);
            }
            if (previewMeshDirty_) {
                ImGui::SameLine();
                ImGui::TextDisabled("(unsaved slots)");
            }
        }
        ImGui::End();
        return;
    }

    // --- Read-only previews ---------------------------------------------------
    if (viewerPreviewId_ != 0) {
        const f32 avail = glm::max(ImGui::GetContentRegionAvail().x, 64.0f);
        const f32 dim = glm::min(avail, 384.0f);
        const f32 indent = glm::max((avail - dim) * 0.5f, 0.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        ImGui::Image(static_cast<ImTextureID>(viewerPreviewId_), ImVec2(dim, dim));
    }
    for (const std::string& line : viewerInfo_) {
        ImGui::TextUnformatted(line.c_str());
    }

    if (isUaf && viewedTypeName_ == "Audio") {
        if (ImGui::Button("Play")) engine.GetAudio().PlayUAF(viewedAsset_);
        ImGui::Separator();
        // Asset tags: category (mixing/organisation) + a voiceline caption for
        // accessibility. Written back into the .uaf (preserving the PCM).
        const char* kinds[] = {"SFX", "Music", "Ambience", "Voiceline"};
        bool tagsChanged = false;
        if (ImGui::Combo("Type##audiokind", &viewedAudioKind_, kinds, 4)) tagsChanged = true;
        if (viewedAudioKind_ == static_cast<int>(uaf::AudioKind::Voiceline)) {
            ImGui::TextDisabled("Caption (shown on screen when this voiceline plays):");
            ImGui::InputTextMultiline("##audiocaption", viewedAudioCaption_,
                                      sizeof(viewedAudioCaption_), ImVec2(-1.0f, 56.0f));
            if (ImGui::IsItemDeactivatedAfterEdit()) tagsChanged = true;
        }
        if (tagsChanged) {
            if (auto a = uaf::ReadAudio(viewedAsset_)) { // read-modify-write keeps PCM
                a->kind = static_cast<uaf::AudioKind>(viewedAudioKind_);
                a->caption = viewedAudioCaption_;
                uaf::WriteAudio(viewedAsset_, *a);
            }
        }
    }
    if (isScene) {
        if (ImGui::Button("Load (replace)")) LoadSceneInEditor(engine, viewedAsset_);
        ImGui::SameLine();
        if (ImGui::Button("Stream in (additive)")) {
            streamer_.BeginLoad(viewedAsset_, Project::Active().AssetsDir(),
                                scene::LoadMode::Additive);
        }
    }
    ImGui::End();
}

// --- Scene manager ----------------------------------------------------------------

void Editor::RefreshScenes() {
    sceneList_.clear();
    scenesScanned_ = true;
    if (!Project::HasActive()) return;
    const std::filesystem::path root = Project::Active().AssetsDir();
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file() && it->path().extension() == ".hbscene") {
            sceneList_.push_back(it->path());
        }
    }
    std::sort(sceneList_.begin(), sceneList_.end());
}

void Editor::DrawSceneManager(Engine& engine) {
    if (!panelOpen_[Panel_Scenes]) return;
    Scene& scene = engine.GetScene();
    ImGui::Begin("Scenes", &panelOpen_[Panel_Scenes]);
    if (!Project::HasActive()) {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }
    if (!scenesScanned_) RefreshScenes();
    Project& project = Project::Active();

    // Toolbar.
    if (ImGui::Button("New Scene")) {
        PushUndo(scene);
        scene.Registry().clear();
        selected_ = entt::null;
        currentScenePath_.clear();
        levelOpen_ = false;
        currentLevel_ = {};
        wantSaveSceneAs_ = true; // name it right away
    }
    ImGui::SameLine();
    if (ImGui::Button("New Level")) {
        wantNewLevel_ = true; // static + dynamic layer files (name it next)
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create a level: <name>.static + <name>.dynamic scene files.\n"
                          "Navmesh bakes the static layer only. UI (menu/HUD) are\n"
                          "separate standalone scenes, not part of a level.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) SaveCurrent(scene);
    ImGui::SameLine();
    ImGui::BeginDisabled(levelOpen_); // a level saves to its two files
    if (ImGui::Button("Save As...")) wantSaveSceneAs_ = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) RefreshScenes();

    if (levelOpen_) {
        ImGui::Text("Level: %s  [static / dynamic / ui]", currentLevel_.Name().c_str());
    } else {
        ImGui::Text("Current: %s", currentScenePath_.empty()
                                       ? "(unsaved scene)"
                                       : currentScenePath_.stem().string().c_str());
    }
    ImGui::Separator();

    if (sceneList_.empty()) {
        ImGui::TextDisabled("No scenes yet - use Save As to create one.");
    }

    const std::string startupRel = project.Settings().startupScene;
    std::filesystem::path deferredLoad;
    scene::LevelPaths deferredLevel;
    bool wantOpenLevel = false;
    bool deferredAdditive = false;

    // Levels: group the .static/.dynamic/.ui members by their base name. Each
    // row opens the whole triplet; the individual members are hidden from the
    // standalone scene list below.
    {
        std::vector<std::filesystem::path> bases;
        for (const std::filesystem::path& p : sceneList_) {
            if (!scene::IsLevelMember(p)) continue;
            const std::filesystem::path base = scene::ResolveLevel(p).base;
            if (std::find(bases.begin(), bases.end(), base) == bases.end())
                bases.push_back(base);
        }
        if (!bases.empty()) {
            ImGui::SeparatorText("Levels");
            // Which level layer files are currently resident (for loaded/active marks).
            std::set<std::string> loadedSrc;
            for (const entt::entity e : scene.Registry().view<SceneSource>()) {
                const std::string& s = scene.Registry().get<SceneSource>(e).scene;
                if (!s.empty()) loadedSrc.insert(s);
            }
            for (usize i = 0; i < bases.size(); ++i) {
                scene::LevelPaths lp;
                lp.base = bases[i];
                const bool isActive = levelOpen_ && currentLevel_.base == lp.base;
                const bool isLoaded =
                    loadedSrc.count(lp.Member(SceneKind::Static).string()) ||
                    loadedSrc.count(lp.Member(SceneKind::Dynamic).string());
                const bool isCur = isActive; // selection highlight on the active level
                ImGui::PushID(static_cast<int>(i) + 10000);
                std::string layers;
                for (const SceneKind k : {SceneKind::Static, SceneKind::Dynamic}) {
                    std::error_code ec;
                    if (std::filesystem::exists(lp.Member(k), ec)) {
                        if (!layers.empty()) layers += "/";
                        layers += ToString(k);
                    }
                }
                const char* mark = isActive ? "  (active)" : (isLoaded ? "  (loaded)" : "");
                char row[512];
                std::snprintf(row, sizeof(row), "%s  [%s]%s##lvl", lp.Name().c_str(),
                              layers.c_str(), mark);
                if (ImGui::Selectable(row, isCur, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    deferredLevel = lp; // double-click loads ADDITIVELY (compose)
                    deferredAdditive = true;
                    wantOpenLevel = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Double-click: add to the world (additive).\n"
                                      "Right-click: open by itself (replace).");
                }
                if (ImGui::BeginPopupContextItem("##lvlctx")) {
                    if (ImGui::MenuItem("Open by Itself (replace)")) {
                        deferredLevel = lp;
                        deferredAdditive = false;
                        wantOpenLevel = true;
                    }
                    if (ImGui::MenuItem("Add Additively")) {
                        deferredLevel = lp;
                        deferredAdditive = true;
                        wantOpenLevel = true;
                    }
                    // When several levels are composed, choose which one new
                    // objects join (no reload). Only meaningful if it's resident.
                    if (ImGui::MenuItem("Set Active (new objects join here)", nullptr, false,
                                        isLoaded && !isActive)) {
                        currentLevel_ = lp;
                        levelOpen_ = true;
                    }
                    if (ImGui::MenuItem("Set static as startup scene")) {
                        project.Settings().startupScene =
                            project.RelativeAssetPath(lp.Member(SceneKind::Static));
                        project.Save();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete Level")) {
                        for (const SceneKind k : {SceneKind::Static, SceneKind::Dynamic}) {
                            std::error_code ec;
                            std::filesystem::remove(lp.Member(k), ec);
                        }
                        if (isCur) {
                            levelOpen_ = false;
                            currentLevel_ = {};
                        }
                        scenesScanned_ = false;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::SeparatorText("Scenes");
        }
    }

    for (usize i = 0; i < sceneList_.size(); ++i) {
        const std::filesystem::path& path = sceneList_[i];
        if (scene::IsLevelMember(path)) continue; // shown under Levels above
        const std::string rel = project.RelativeAssetPath(path);
        const bool isStartup = !startupRel.empty() && rel == startupRel;
        const bool isCurrent = path == currentScenePath_;

        ImGui::PushID(static_cast<int>(i));
        // Startup marker: filled star for the default scene; click to set.
        if (ImGui::SmallButton(isStartup ? "[default]" : "   set   ")) {
            project.Settings().startupScene = rel;
            project.Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(isStartup ? "This is the startup (default) scene"
                                        : "Make this the startup (default) scene");
        }
        ImGui::SameLine();
        char row[512];
        std::snprintf(row, sizeof(row), "%s%s##scene", rel.c_str(),
                      isCurrent ? "  (open)" : "");
        if (ImGui::Selectable(row, isCurrent,
                              ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            deferredLoad = path;
        }
        if (ImGui::BeginPopupContextItem("##scenectx")) {
            if (ImGui::MenuItem("Load (replace)")) deferredLoad = path;
            if (ImGui::MenuItem("Stream in (additive)")) {
                streamer_.BeginLoad(path, project.AssetsDir(), scene::LoadMode::Additive);
            }
            if (ImGui::MenuItem("Set as startup scene")) {
                project.Settings().startupScene = rel;
                project.Save();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename...")) {
                renameScene_ = path;
                std::snprintf(renameBuf_, sizeof(renameBuf_), "%s",
                              path.stem().string().c_str());
            }
            if (ImGui::MenuItem("Duplicate")) {
                std::error_code ec;
                std::filesystem::path copy = path;
                copy.replace_filename(path.stem().string() + " Copy.hbscene");
                for (int n = 2; std::filesystem::exists(copy, ec) && n < 100; ++n) {
                    copy.replace_filename(path.stem().string() + " Copy " +
                                          std::to_string(n) + ".hbscene");
                }
                std::filesystem::copy_file(path, copy, ec);
                if (!ec) scenesScanned_ = false;
            }
            if (ImGui::MenuItem("Delete")) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
                if (!ec) {
                    if (isStartup) {
                        project.Settings().startupScene.clear();
                        project.Save();
                    }
                    if (isCurrent) currentScenePath_.clear();
                    scenesScanned_ = false;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Deferred so the list isn't mutated mid-iteration by a load that
    // triggers RefreshAssets/RefreshScenes.
    if (wantOpenLevel) OpenLevel(engine, deferredLevel, deferredAdditive);
    else if (!deferredLoad.empty()) LoadSceneInEditor(engine, deferredLoad);

    // New Level modal: names the static/dynamic/UI triplet.
    if (wantNewLevel_ && !ImGui::IsPopupOpen("New Level")) ImGui::OpenPopup("New Level");
    if (ImGui::BeginPopupModal("New Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Creates <name>.static + <name>.dynamic scene files.");
        ImGui::SetNextItemWidth(240.0f);
        const bool enter = ImGui::InputText("Name##newlevel", levelNameBuf_,
                                            sizeof(levelNameBuf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string stem = SanitizeFileStem(levelNameBuf_);
        const bool can = !stem.empty();
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Create") || (enter && can)) {
            CreateLevel(engine, project.AssetsDir() / "Scenes" / stem); // levels live in Scenes/
            wantNewLevel_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            wantNewLevel_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Rename popup.
    if (!renameScene_.empty() && !ImGui::IsPopupOpen("Rename Scene")) {
        ImGui::OpenPopup("Rename Scene");
    }
    if (ImGui::BeginPopupModal("Rename Scene", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(240.0f);
        const bool enter = ImGui::InputText("##renamescene", renameBuf_, sizeof(renameBuf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool canRename = renameBuf_[0] != '\0';
        ImGui::BeginDisabled(!canRename);
        if (ImGui::Button("Rename") || (enter && canRename)) {
            std::error_code ec;
            std::filesystem::path to = renameScene_;
            to.replace_filename(std::string(renameBuf_) + ".hbscene");
            if (!std::filesystem::exists(to, ec)) {
                const std::string oldRel = Project::Active().RelativeAssetPath(renameScene_);
                std::filesystem::rename(renameScene_, to, ec);
                if (!ec) {
                    if (Project::Active().Settings().startupScene == oldRel) {
                        Project::Active().Settings().startupScene =
                            Project::Active().RelativeAssetPath(to);
                        Project::Active().Save();
                    }
                    if (currentScenePath_ == renameScene_) currentScenePath_ = to;
                    scenesScanned_ = false;
                }
            }
            renameScene_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            renameScene_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// Draws a screen-space outline of the selected entity's world AABB into the
// viewport so picking has visible feedback.
// --- Shipping ------------------------------------------------------------------

std::set<std::string> Editor::CollectReferencedAssets() {
    std::set<std::string> refs;
    if (!Project::HasActive()) return refs;
    namespace fs = std::filesystem;
    const fs::path assetsDir = Project::Active().AssetsDir();
    std::error_code ec;

    const auto addTexture = [&](const std::string& rel) {
        if (!rel.empty()) refs.insert(rel);
    };
    const auto addMesh = [&](const std::string& rel) {
        if (rel.empty() || refs.count(rel)) return;
        refs.insert(rel);
        // The mesh's own materials reference textures.
        if (const auto model = uaf::ReadMesh(assetsDir / rel)) {
            for (const MeshData& md : *model) {
                addTexture(md.material.baseColorTex);
                addTexture(md.material.normalTex);
                addTexture(md.material.mrTex);
                addTexture(md.material.aoTex);
                addTexture(md.material.emissiveTex);
            }
        }
    };

    for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file() || it->path().extension() != ".hbscene") continue;
        refs.insert(fs::relative(it->path(), assetsDir, ec).generic_string());

        scene::SceneData data;
        if (!scene::ParseSceneFile(it->path(), data)) continue;
        for (const scene::EntityData& d : data.entities) {
            // "uaf:<rel>#<submesh>" mesh provenance.
            if (d.hasMesh && d.meshSource.rfind("uaf:", 0) == 0) {
                const std::string rest = d.meshSource.substr(4);
                addMesh(rest.substr(0, rest.find_last_of('#')));
            }
            if (!d.materialAsset.empty() && !refs.count(d.materialAsset)) {
                refs.insert(d.materialAsset);
                if (const auto mat = assets::LoadMaterial(assetsDir / d.materialAsset)) {
                    addTexture(mat->albedoTex);
                    addTexture(mat->normalTex);
                    addTexture(mat->mrTex);
                    addTexture(mat->aoTex);
                    addTexture(mat->emissiveTex);
                }
            }
            if (d.hasAudio) addTexture(d.audio.asset); // same "insert rel" semantics
            // UI element image + font (.uaf), e.g. a boot-screen logo - otherwise
            // they pack out and show as a white square / default font in builds.
            if (d.hasUI) {
                addTexture(d.uiElement.texture);
                addTexture(d.uiElement.font);
            }
            // Art Editor paint canvas (.hbpaint): pixels live outside the scene.
            if (d.hasPaint && !d.paintSource.empty()) refs.insert(d.paintSource);
        }
    }
    return refs;
}

namespace {
// Pack options from the active project's BuildSettings. `refs` keeps the
// referenced-asset set alive for the returned options' filter pointer.
uap::WriteOptions PackOptionsFromSettings(std::set<std::string>& refs) {
    const BuildSettings& build = Project::Active().Settings().build;
    uap::WriteOptions options;
    options.compress = build.compressAssets;
    if (build.onlyReferenced) {
        refs = Editor::CollectReferencedAssets();
        options.filter = &refs;
    }
    return options;
}

std::string PackSummary(const uap::PackBuildResult& result) {
    char buf[160];
    const f64 mb = 1024.0 * 1024.0;
    std::snprintf(buf, sizeof(buf), "%u assets, %u pack(s), %.1f MB -> %.1f MB",
                  result.assetCount, result.packCount, result.rawBytes / mb,
                  result.packedBytes / mb);
    return buf;
}
} // namespace

bool Editor::BuildAssetPack(std::string& outMessage) {
    if (!Project::HasActive()) {
        outMessage = "No project open.";
        return false;
    }
    const std::string& name = Project::Active().Settings().name;
    const std::filesystem::path root = Project::Active().Root();
    std::set<std::string> refs;
    const uap::WriteOptions options = PackOptionsFromSettings(refs);
    const auto result = uap::WritePacks(root, name, Project::Active().AssetsDir(),
                                        root / (name + ".uapmanifest"), options);
    if (!result) {
        outMessage = "Pack failed: no assets found.";
        return false;
    }
    // Verify: reopen every chunk and read back the first entry (this also
    // exercises decompression on compressed packs).
    uap::PackSet set;
    if (!set.Open(root, name) || set.PackCount() != result->packCount ||
        set.AssetCount() != result->assetCount) {
        outMessage = "Pack verification failed (TOC mismatch).";
        return false;
    }
    const std::vector<uap::Entry> entries = set.Entries();
    const auto bytes = set.Read(entries.front().path);
    if (!bytes || bytes->size() != entries.front().rawSize) {
        outMessage = "Pack verification failed (payload mismatch).";
        return false;
    }
    outMessage = "Packed " + PackSummary(*result) + " (verified)";
    HBE_INFO("UAP: {}", outMessage);
    return true;
}

bool Editor::BuildShipping(std::string& outMessage) {
    if (!Project::HasActive()) {
        outMessage = "No project open.";
        return false;
    }
    // Persist the live project settings BEFORE packing - the build packs the
    // `.hbproj` from disk (below), so anything edited live but not yet saved (e.g.
    // the Dynamic Sky / day-night toggle, weather, build settings) would otherwise
    // ship stale. This makes a build always reflect the current editor state.
    Project::Active().Save();
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string& name = Project::Active().Settings().name;
    const fs::path dst = Project::Active().Root() / "Build";
    fs::create_directories(dst, ec);

    // Pick the freshest runtime executable across build configs (a Release
    // runtime ships several times smaller than a Debug one).
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const fs::path editorDir = fs::path(buf).parent_path();
    fs::path runtimeDir = editorDir;
    fs::file_time_type best{};
    for (const char* config : {".", "../Release", "../RelWithDebInfo", "../MinSizeRel"}) {
        const fs::path candidate = editorDir / config / "HeartbreakRuntime.exe";
        if (!fs::exists(candidate, ec)) continue;
        const auto t = fs::last_write_time(candidate, ec);
        if (t > best) {
            best = t;
            runtimeDir = candidate.parent_path();
        }
    }

    const auto copy = [&](const fs::path& from, const fs::path& to) {
        std::error_code c;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, c);
        return !c;
    };

    // Start clean. A shipped build is ONLY the game application, the asset
    // packs, and DLLs - so clear anything an older-style build left behind
    // (loose shaders/ folder, Assets/, .hbproj, stale exes/dlls/packs).
    for (auto it = fs::directory_iterator(dst, ec); it != fs::directory_iterator();
         it.increment(ec)) {
        const fs::path p = it->path();
        if (it->is_directory()) {
            fs::remove_all(p, ec);
            continue;
        }
        const std::string ext = p.extension().string();
        if (ext == ".exe" || ext == ".dll" || ext == ".json" || ext == ".uap" ||
            ext == ".hbproj" || ext == ".uapmanifest" || ext == ".pdb") {
            fs::remove(p, ec);
        }
    }

    // 1) The game application, named after the game (BuildSettings.gameName, else
    //    the project name). The runtime finds its packs by its own directory, not
    //    its filename, so renaming is safe.
    const std::string& gameName = Project::Active().Settings().build.gameName;
    std::string exeStem = SanitizeFileStem(gameName.empty() ? Project::Active().Settings().name
                                                            : gameName);
    if (exeStem.empty()) exeStem = "Game";
    bool ok = copy(runtimeDir / "HeartbreakRuntime.exe", dst / (exeStem + ".exe"));

    // 2) DLLs. Native dependencies sit next to the runtime (they link statically
    //    today, but this keeps any future ones working).
    for (const auto& it : fs::directory_iterator(runtimeDir, ec)) {
        if (it.is_regular_file() && it.path().extension() == ".dll") {
            ok &= copy(it.path(), dst / it.path().filename());
        }
    }

    // 3) The asset packs - now also carrying the engine's compiled shaders and
    //    the project file as packed extras, so the build needs no loose shaders/
    //    folder or .hbproj. The runtime mounts the packs and reads both back.
    std::vector<uap::ExtraFile> extras;
    const fs::path shaderDir =
        fs::exists(runtimeDir / "shaders", ec) ? runtimeDir / "shaders" : editorDir / "shaders";
    for (auto it = fs::directory_iterator(shaderDir, ec); it != fs::directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file()) {
            extras.push_back({"Shaders/" + it->path().filename().string(), it->path()});
        }
    }
    extras.push_back({"__project.hbproj", Project::Active().ProjectFile()});

    std::set<std::string> refs;
    uap::WriteOptions options = PackOptionsFromSettings(refs);
    options.extras = &extras;
    // A full export packs into dense slots (the fewest packs) - a shipped build
    // is a fresh whole-folder copy, so the dev manifest's sticky/sparse slots
    // (left by deleting assets) would only waste pack files here.
    options.compact = true;
    const auto packed = uap::WritePacks(dst, name, Project::Active().AssetsDir(),
                                        Project::Active().Root() / (name + ".ship.uapmanifest"),
                                        options);
    ok &= packed.has_value();
    if (packed) {
        // Verify the cooked packs read back (exercises decompression too) and
        // that the folded-in runtime files survived the round trip.
        uap::PackSet set;
        bool verified = set.Open(dst, name) && set.AssetCount() == packed->assetCount;
        if (verified) {
            const std::vector<uap::Entry> entries = set.Entries();
            const auto bytes = set.Read(entries.front().path);
            verified = bytes && bytes->size() == entries.front().rawSize;
        }
        verified = verified && set.Contains("__project.hbproj");
        ok &= verified;
        if (!verified) HBE_ERROR("Shipping: pack verification failed.");
    }

    u64 totalBytes = 0;
    for (auto it = fs::recursive_directory_iterator(dst, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file()) totalBytes += fs::file_size(it->path(), ec);
    }
    char size[64];
    std::snprintf(size, sizeof(size), "%.1f MB", totalBytes / (1024.0 * 1024.0));
    outMessage = ok ? "Shipping build (" + std::string(size) + ") at " + dst.string() +
                          (packed ? "  [" + PackSummary(*packed) + "]" : "")
                    : "Shipping build finished with errors (see log).";
    HBE_INFO("Shipping: {}", outMessage);
    return ok;
}

void Editor::DrawBuildSettings(Engine& engine) {
    (void)engine;
    if (!showBuildSettings_) return;
    if (!Project::HasActive()) {
        showBuildSettings_ = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Build Settings", &showBuildSettings_,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    Project& project = Project::Active();
    BuildSettings& build = project.Settings().build;
    bool changed = false;

    ImGui::SeparatorText("Game");
    {
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", build.gameName.c_str());
        if (ImGui::InputTextWithHint("Game Name", project.Settings().name.c_str(),
                                     nameBuf, sizeof(nameBuf))) {
            build.gameName = nameBuf;
            changed = true;
        }
        char companyBuf[128];
        std::snprintf(companyBuf, sizeof(companyBuf), "%s", build.company.c_str());
        if (ImGui::InputText("Company", companyBuf, sizeof(companyBuf))) {
            build.company = companyBuf;
            changed = true;
        }
        char versionBuf[32];
        std::snprintf(versionBuf, sizeof(versionBuf), "%s", build.version.c_str());
        if (ImGui::InputText("Version", versionBuf, sizeof(versionBuf))) {
            build.version = versionBuf;
            changed = true;
        }
    }

    ImGui::SeparatorText("Platform");
    {
        // One target platform today; the combo documents where others land.
        int platform = 0;
        ImGui::Combo("Platform", &platform, "Windows x64\0");

        static const char* kBackendIds[3] = {"d3d12", "vulkan", "opengl"};
        const auto backendIndex = [](const std::string& s) -> int {
            return s == "vulkan" ? 1 : s == "opengl" ? 2 : 0;
        };
        int backend = backendIndex(build.backend);
        if (ImGui::Combo("Graphics Backend", &backend, "Direct3D 12\0Vulkan\0OpenGL\0")) {
            build.backend = kBackendIds[backend];
            changed = true;
        }

        // Per-platform backend fallback (the "build profile"): the shipped game
        // tries these in order at boot and uses the first that initializes, so a
        // player whose machine fails D3D12 falls through to Vulkan, then OpenGL.
        ImGui::TextDisabled("Boot fallback order (first that initializes wins):");
        // Derive the 3 slots from the windows profile, else from the primary backend.
        std::vector<std::string> order;
        for (const BuildProfile& p : build.profiles)
            if (p.platform == "windows") { order = p.backends; break; }
        if (order.empty()) {
            order = {build.backend};
            for (const char* o : kBackendIds)
                if (build.backend != o) order.emplace_back(o);
        }
        int slot[3] = {0, 0, 0}; // 0 = None
        for (int i = 0; i < 3 && i < static_cast<int>(order.size()); ++i)
            slot[i] = backendIndex(order[i]) + 1;
        bool slotChanged = false;
        for (int i = 0; i < 3; ++i) {
            char label[24];
            std::snprintf(label, sizeof(label), "Try #%d", i + 1);
            if (ImGui::Combo(label, &slot[i], "None\0Direct3D 12\0Vulkan\0OpenGL\0"))
                slotChanged = true;
        }
        if (slotChanged) {
            BuildProfile* win = nullptr;
            for (BuildProfile& p : build.profiles)
                if (p.platform == "windows") { win = &p; break; }
            if (!win) { build.profiles.push_back(BuildProfile{}); win = &build.profiles.back(); }
            win->platform = "windows";
            win->backends.clear();
            for (int i = 0; i < 3; ++i)
                if (slot[i] > 0) win->backends.push_back(kBackendIds[slot[i] - 1]);
            changed = true;
        }
        changed |= ImGui::Checkbox("Fullscreen (borderless, covers the screen)",
                                   &build.fullscreen);
        ImGui::BeginDisabled(build.fullscreen);
        int res[2] = {static_cast<int>(build.width), static_cast<int>(build.height)};
        if (ImGui::InputInt2("Resolution", res)) {
            build.width = static_cast<u32>(glm::clamp(res[0], 320, 7680));
            build.height = static_cast<u32>(glm::clamp(res[1], 240, 4320));
            changed = true;
        }
        ImGui::EndDisabled();
        if (build.fullscreen)
            ImGui::TextDisabled("Windowed resolution is used only when Fullscreen is off.");
    }

    ImGui::SeparatorText("Content");
    {
        // Startup scene picker straight from the scene list.
        if (!scenesScanned_) RefreshScenes();
        const std::string& startup = project.Settings().startupScene;
        if (ImGui::BeginCombo("Startup Scene",
                              startup.empty() ? "(demo scene)" : startup.c_str())) {
            for (const std::filesystem::path& s : sceneList_) {
                const std::string rel = project.RelativeAssetPath(s);
                if (ImGui::Selectable(rel.c_str(), rel == startup)) {
                    project.Settings().startupScene = rel;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        changed |= ImGui::Checkbox("Ship packed assets only (.uap, no Assets folder)",
                                   &build.packAssets);
        changed |= ImGui::Checkbox("Compress packs (LZMS)", &build.compressAssets);
        changed |= ImGui::Checkbox("Pack only scene-referenced assets",
                                   &build.onlyReferenced);
        changed |= ImGui::Checkbox("Developer overlay in shipped build (Ctrl+`)",
                                   &build.devMenu);
        if (build.devMenu)
            ImGui::TextDisabled("In-game: Ctrl+` toggles dev stats; F5 save, F9 load,\n"
                                "F2 restart, F3 menu.");
    }

    ImGui::SeparatorText("Game Flow");
    {
        // Optional scene slots that wire up the runtime's menu -> loading ->
        // gameplay flow. Setting a Main Menu scene makes the runtime boot into it
        // (instead of the Startup Scene); UI buttons with a "play"/"menu"/"quit"
        // action drive the transitions. Each is a plain (non-level) UI scene.
        ImGui::TextDisabled("Set a Main Menu scene to boot into the menu flow.\n"
                            "Buttons with a play/menu/quit Action drive it.");
        auto scenePicker = [&](const char* label, std::string& slot) {
            if (ImGui::BeginCombo(label, slot.empty() ? "(none)" : slot.c_str())) {
                if (ImGui::Selectable("(none)", slot.empty())) {
                    slot.clear();
                    changed = true;
                }
                for (const std::filesystem::path& s : sceneList_) {
                    const std::string rel = project.RelativeAssetPath(s);
                    if (ImGui::Selectable(rel.c_str(), rel == slot)) {
                        slot = rel;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        };
        scenePicker("Main Menu Scene", project.Settings().mainMenuScene);
        scenePicker("HUD Scene", project.Settings().hudScene);
        scenePicker("Game Loading Scene", project.Settings().loadingScene);
        scenePicker("Studio (Boot) Scene", project.Settings().studioLoadingScene);
        ImGui::TextDisabled(
            "Loading scenes: add a ProgressBar; the engine drives its fill.\n"
            "Studio scene shows at boot. Put {log} in a Label to show the live\n"
            "boot console as it loads. Other text tokens: {progress} {version}\n"
            "{backend} {gpu} {audio}.");
    }

    ImGui::SeparatorText("UI Canvas");
    {
        int mode = static_cast<int>(glm::clamp(build.uiScaleMode, 0u, 2u));
        if (ImGui::Combo("Scale Mode", &mode,
                         "Stretch\0Match Height\0Pixel-Perfect\0")) {
            build.uiScaleMode = static_cast<u32>(mode);
            changed = true;
        }
        int refRes[2] = {static_cast<int>(build.uiRefWidth),
                         static_cast<int>(build.uiRefHeight)};
        if (ImGui::InputInt2("Reference Resolution", refRes)) {
            build.uiRefWidth = static_cast<u32>(glm::clamp(refRes[0], 64, 16384));
            build.uiRefHeight = static_cast<u32>(glm::clamp(refRes[1], 64, 16384));
            changed = true;
        }
        ImGui::TextDisabled("Match Height keeps UI size constant and grows width\n"
                            "with the aspect ratio (recommended).");
    }

    if (changed) project.Save();

    ImGui::Separator();
    // One Build: cooks (compressed) packs AND assembles the shipping folder.
    if (ImGui::Button("Build", ImVec2(120, 0))) BuildShipping(buildResult_);
    if (!buildResult_.empty()) {
        ImGui::TextWrapped("%s", buildResult_.c_str());
    }
    ImGui::End();
}

// --- Project manager ---------------------------------------------------------

void Editor::LoadRecentProjects() {
    recentProjects_.clear();
    std::ifstream in(RecentProjectsFile());
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        for (const auto& entry : j.value("recent", nlohmann::json::array())) {
            std::filesystem::path p = std::filesystem::path(entry.get<std::string>());
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) recentProjects_.push_back(std::move(p));
        }
    } catch (const std::exception&) {
        // Corrupt list: start fresh.
    }
}

void Editor::SaveRecentProjects() const {
    const std::filesystem::path file = RecentProjectsFile();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    nlohmann::json j;
    auto& arr = j["recent"] = nlohmann::json::array();
    for (const auto& p : recentProjects_) arr.push_back(p.string());
    std::ofstream out(file);
    if (out) out << j.dump(2);
}

void Editor::AddRecentProject(const std::filesystem::path& hbproj) {
    const auto same = [&](const std::filesystem::path& p) { return p == hbproj; };
    recentProjects_.erase(
        std::remove_if(recentProjects_.begin(), recentProjects_.end(), same),
        recentProjects_.end());
    recentProjects_.insert(recentProjects_.begin(), hbproj);
    if (recentProjects_.size() > 10) recentProjects_.resize(10);
}

void Editor::OnProjectChanged() {
    // Asset browser / viewer / scene-list state belongs to the previous project.
    assets_.clear();
    assetsScanned_ = false;
    currentDir_.clear();
    navTarget_.clear();
    thumbCache_.clear(); // ids stay alive on the device; just re-resolve lazily
    previewCache_.clear();
    viewedAsset_.clear();
    editedMatValid_ = false;
    editedEventValid_ = false;
    mixerSynced_ = false; // re-push the new project's bus tree
    ui::ClearFontCache(); // font-asset atlases belong to the previous project
    sceneList_.clear();
    scenesScanned_ = false;
    currentScenePath_.clear();
    undoStack_.clear(); // snapshots reference the previous project's assets
    redoStack_.clear();
    textureCache_.clear();
    cpuMeshCache_.clear();  // CPU geometry belongs to the previous project
    paintStrokeOrder_.clear();
    paintStrokeRedo_.clear();
    musicLoaded_ = false;   // re-sync the music graph from the new project
    musicPreviewing_ = false;
    brushesLoaded_ = false; // reload the new project's brush library (brushes.json)
    scene::ClearInstantiateCaches();

    // Seed starter content so a brand-new project isn't empty.
    const std::filesystem::path sphereUaf = Project::Active().AssetsDir() / "Sphere.uaf";
    std::error_code ec;
    if (!std::filesystem::exists(sphereUaf, ec)) {
        Model model{mesh::GenerateSphere(0.5f, 32, 16)};
        uaf::WriteMesh(sphereUaf, model);
    }
}

namespace {
// Hub hand-off: spawn the full editor on the chosen project.
void LaunchEditorDetached(const std::filesystem::path& hbproj) {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::filesystem::path editorExe =
        std::filesystem::path(buf).parent_path() / "HeartbreakEditor.exe";

    std::wstring cmd = L"\"" + editorExe.wstring() + L"\" --project \"" +
                       hbproj.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (::CreateProcessW(editorExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                         nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    } else {
        HBE_ERROR("Hub: failed to launch '{}'", editorExe.string());
    }
}
} // namespace

bool Editor::OpenProject(const std::filesystem::path& hbproj) {
    if (!Project::Active().Open(hbproj)) return false;
    AddRecentProject(Project::Active().ProjectFile());
    SaveRecentProjects();
    OnProjectChanged();
    showProjectManager_ = false;
    if (hubMode_) {
        LaunchEditorDetached(Project::Active().ProjectFile());
        if (engine_) engine_->Quit();
    }
    return true;
}

bool Editor::CreateProject(const std::filesystem::path& directory, const std::string& name) {
    if (!Project::Active().Create(directory, name)) return false;
    AddRecentProject(Project::Active().ProjectFile());
    SaveRecentProjects();
    OnProjectChanged();
    showProjectManager_ = false;
    if (hubMode_) {
        LaunchEditorDetached(Project::Active().ProjectFile());
        if (engine_) engine_->Quit();
    }
    return true;
}

void Editor::DrawProjectManager() {
    if (!recentsLoaded_) {
        recentsLoaded_ = true;
        LoadRecentProjects();
        const std::string def = std::filesystem::current_path().string();
        std::snprintf(newProjectDir_, sizeof(newProjectDir_), "%s", def.c_str());
    }

    const bool mustChoose = !Project::HasActive();
    if ((showProjectManager_ || mustChoose) && !ImGui::IsPopupOpen("Project Manager")) {
        ImGui::OpenPopup("Project Manager");
    }
    if (!ImGui::BeginPopupModal("Project Manager", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextUnformatted("Recent projects");
    if (recentProjects_.empty()) {
        ImGui::TextDisabled("  (none yet)");
    }
    // Iterate a copy: opening a project reorders the recents list.
    const std::vector<std::filesystem::path> recents = recentProjects_;
    for (usize i = 0; i < recents.size(); ++i) {
        const std::filesystem::path& p = recents[i];
        ImGui::PushID(static_cast<int>(i));
        char row[512];
        std::snprintf(row, sizeof(row), "%s   %s", p.stem().string().c_str(),
                      p.parent_path().string().c_str());
        if (ImGui::Selectable(row)) OpenProject(p);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove from list")) {
                recentProjects_.erase(
                    std::remove(recentProjects_.begin(), recentProjects_.end(), p),
                    recentProjects_.end());
                SaveRecentProjects();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Create new project");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Name", newProjectName_, sizeof(newProjectName_));
    ImGui::SetNextItemWidth(380.0f);
    ImGui::InputText("Location", newProjectDir_, sizeof(newProjectDir_));
    ImGui::SameLine();
    if (ImGui::Button("...")) {
        if (const auto dir = BrowseForFolderDialog()) {
            std::snprintf(newProjectDir_, sizeof(newProjectDir_), "%s",
                          dir->string().c_str());
        }
    }
    const bool canCreate = newProjectName_[0] != '\0' && newProjectDir_[0] != '\0';
    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button("Create Project")) {
        // The project gets its own subdirectory: <location>/<name>/<name>.hbproj
        CreateProject(std::filesystem::path(newProjectDir_) / newProjectName_,
                      newProjectName_);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button("Open existing (.hbproj)...")) {
        wchar_t file[1024] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = L"Heartbreak project\0*.hbproj\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(std::size(file));
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            OpenProject(std::filesystem::path(file));
        }
    }
    if (Project::HasActive()) {
        ImGui::SameLine();
        if (ImGui::Button("Close")) showProjectManager_ = false;
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("Open or create a project to continue.");
    }

    // Modal closes once a project is active and no longer requested.
    if (!showProjectManager_ && Project::HasActive()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::DrawSelectionOutline(Scene& scene, Renderer& renderer) {
    if (!vpVisible_) return; // another tab (Game / Schematic Editor) is in front
    auto& reg = scene.Registry();
    if (selected_ == entt::null || !reg.valid(selected_)) return;

    const glm::mat4 vp = renderer.GetCamera().ViewProjection();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(vpX_, vpY_), ImVec2(vpX_ + vpW_, vpY_ + vpH_), true);

    // Projects a world point to viewport-panel pixels; returns false if behind
    // the camera plane.
    const auto project = [&](const glm::vec3& world, ImVec2& out) -> bool {
        const glm::vec4 clip = vp * glm::vec4(world, 1.0f);
        if (clip.w <= 0.001f) return false;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        out = ImVec2(vpX_ + (ndc.x * 0.5f + 0.5f) * vpW_,
                     vpY_ + (0.5f - ndc.y * 0.5f) * vpH_);
        return true;
    };

    static constexpr int kEdges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},  // min face
        {4, 5}, {5, 7}, {7, 6}, {6, 4},  // max face
        {0, 4}, {1, 5}, {2, 6}, {3, 7},  // connecting edges
    };

    // Draws an oriented box (local corners min..max under `m`).
    const auto drawBox = [&](const glm::mat4& m, const glm::vec3& bmin,
                             const glm::vec3& bmax, ImU32 color, f32 thick) {
        ImVec2 pts[8];
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y,
                                   (c & 4) ? bmax.z : bmin.z);
            if (!project(glm::vec3(m * glm::vec4(corner, 1.0f)), pts[c])) return;
        }
        for (const auto& e : kEdges) draw->AddLine(pts[e[0]], pts[e[1]], color, thick);
    };

    const glm::mat4 m = scene.WorldMatrix(selected_);
    if (const AABB* box = reg.try_get<AABB>(selected_)) {
        drawBox(m, box->min, box->max, IM_COL32(255, 160, 40, 230), 1.5f);
    }
    // Camera zone volume (green = active, cyan = idle).
    if (const CameraZone* z = reg.try_get<CameraZone>(selected_)) {
        const ImU32 col = z->active ? IM_COL32(80, 230, 120, 235)
                                    : IM_COL32(80, 200, 230, 200);
        drawBox(m, -z->halfExtents, z->halfExtents, col, 1.5f);
    }
    // Camera spline path (control points + connecting lines).
    if (const CameraSpline* sp = reg.try_get<CameraSpline>(selected_)) {
        const ImU32 col = IM_COL32(255, 220, 90, 230);
        for (usize i = 0; i < sp->points.size(); ++i) {
            ImVec2 a;
            if (project(sp->points[i], a)) {
                const bool active = splinePoint_ == static_cast<int>(i);
                draw->AddCircleFilled(a, active ? 6.0f : 4.0f,
                                      active ? IM_COL32(255, 255, 255, 255) : col);
                if (active) draw->AddCircle(a, 9.0f, IM_COL32(255, 160, 40, 235), 0, 2.0f);
            }
            const usize j = i + 1;
            if (j < sp->points.size()) {
                ImVec2 b;
                if (project(sp->points[i], a) && project(sp->points[j], b))
                    draw->AddLine(a, b, col, 1.5f);
            } else if (sp->loop && sp->points.size() > 2) {
                ImVec2 b;
                if (project(sp->points[i], a) && project(sp->points[0], b))
                    draw->AddLine(a, b, col, 1.5f);
            }
        }
    }

    // --- Camera / light direction gizmos (for the selected entity) -----------
    // World-space line helper (drawn only when both ends are in front).
    const auto line3d = [&](const glm::vec3& a, const glm::vec3& b, ImU32 c, float th) {
        ImVec2 pa, pb;
        if (project(a, pa) && project(b, pb)) draw->AddLine(pa, pb, c, th);
    };
    const glm::vec3 origin = glm::vec3(m[3]);
    const glm::vec3 fwd = glm::normalize(glm::vec3(m * glm::vec4(0, 0, -1, 0)));
    const glm::vec3 upv = glm::normalize(glm::vec3(m * glm::vec4(0, 1, 0, 0)));
    const glm::vec3 rightv = glm::normalize(glm::vec3(m * glm::vec4(1, 0, 0, 0)));
    // Orthonormal basis around an axis (for cones / rings).
    const auto basis = [](const glm::vec3& axis, glm::vec3& u, glm::vec3& w) {
        u = std::abs(axis.y) < 0.99f ? glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)))
                                     : glm::vec3(1, 0, 0);
        w = glm::normalize(glm::cross(axis, u));
    };

    // Camera frustum (FOV cone toward local -Z).
    if (const CameraComponent* camc = reg.try_get<CameraComponent>(selected_)) {
        const ImU32 col = IM_COL32(120, 200, 255, 220);
        const float aspect = vpH_ > 1.0f ? vpW_ / vpH_ : 1.7778f;
        const float len = glm::clamp(camc->farZ, 2.0f, 10.0f);
        const float halfH = len * std::tan(glm::radians(camc->fovY * 0.5f));
        const float halfW = halfH * aspect;
        const glm::vec3 fc = origin + fwd * len;
        const glm::vec3 c0 = fc - rightv * halfW - upv * halfH;
        const glm::vec3 c1 = fc + rightv * halfW - upv * halfH;
        const glm::vec3 c2 = fc + rightv * halfW + upv * halfH;
        const glm::vec3 c3 = fc - rightv * halfW + upv * halfH;
        line3d(origin, c0, col, 1.4f); line3d(origin, c1, col, 1.4f);
        line3d(origin, c2, col, 1.4f); line3d(origin, c3, col, 1.4f);
        line3d(c0, c1, col, 1.4f); line3d(c1, c2, col, 1.4f);
        line3d(c2, c3, col, 1.4f); line3d(c3, c0, col, 1.4f);
        const glm::vec3 tip = fc + upv * (halfH * 1.4f); // "up" cue on the far plane
        line3d(c3, tip, col, 1.4f); line3d(c2, tip, col, 1.4f);
    }
    // Spot light cone (axis = local -Y in world; matches Scene lighting).
    if (const SpotLightComponent* sl = reg.try_get<SpotLightComponent>(selected_)) {
        const ImU32 col = IM_COL32(120, 210, 255, 230);
        const glm::vec3 axis = glm::normalize(glm::vec3(m * glm::vec4(0, -1, 0, 0)));
        glm::vec3 u, w; basis(axis, u, w);
        const float len = glm::clamp(sl->range, 0.5f, 14.0f);
        const float rad = len * std::tan(glm::radians(glm::clamp(sl->outerAngle, 1.0f, 89.0f)));
        const glm::vec3 endC = origin + axis * len;
        for (int i = 0; i < 24; ++i) {
            const float a0 = (i / 24.0f) * 6.2831853f, a1 = ((i + 1) / 24.0f) * 6.2831853f;
            line3d(endC + (u * std::cos(a0) + w * std::sin(a0)) * rad,
                   endC + (u * std::cos(a1) + w * std::sin(a1)) * rad, col, 1.2f);
        }
        for (int i = 0; i < 4; ++i) {
            const float a = i * 1.5707963f;
            line3d(origin, endC + (u * std::cos(a) + w * std::sin(a)) * rad, col, 1.4f);
        }
    }
    // Directional light: a small bundle of parallel arrows along `direction`.
    if (const DirectionalLightComponent* dl = reg.try_get<DirectionalLightComponent>(selected_)) {
        const ImU32 col = IM_COL32(255, 214, 92, 235);
        const glm::vec3 d = glm::length(dl->direction) > 1e-5f ? glm::normalize(dl->direction)
                                                               : glm::vec3(0, -1, 0);
        glm::vec3 u, w; basis(d, u, w);
        const float len = 3.0f;
        for (int i = -1; i <= 1; ++i) {
            for (int j = -1; j <= 1; ++j) {
                const glm::vec3 s = origin + (u * float(i) + w * float(j)) * 0.6f;
                const glm::vec3 e = s + d * len;
                line3d(s, e, col, 1.3f);
                line3d(e, e - d * 0.45f + u * 0.18f, col, 1.3f); // arrowhead
                line3d(e, e - d * 0.45f - u * 0.18f, col, 1.3f);
            }
        }
    }
    // Point light: world-axis range rings (its sphere of influence).
    if (const PointLightComponent* pl = reg.try_get<PointLightComponent>(selected_)) {
        const ImU32 col = IM_COL32(255, 230, 130, 120);
        const float rad = glm::clamp(pl->range, 0.05f, 60.0f);
        const auto ring = [&](int ax) {
            for (int i = 0; i < 28; ++i) {
                const float a0 = (i / 28.0f) * 6.2831853f, a1 = ((i + 1) / 28.0f) * 6.2831853f;
                const auto pt = [&](float a) {
                    const float c = std::cos(a), s = std::sin(a);
                    if (ax == 0) return origin + glm::vec3(0, c, s) * rad;
                    if (ax == 1) return origin + glm::vec3(c, 0, s) * rad;
                    return origin + glm::vec3(c, s, 0) * rad;
                };
                line3d(pt(a0), pt(a1), col, 1.1f);
            }
        };
        ring(0); ring(1); ring(2);
    }

    draw->PopClipRect();
}

void Editor::DrawEntityIcons(Scene& scene, Renderer& renderer) {
    iconConsumedClick_ = false;
    if (!showIcons_ || !vpVisible_) return;
    auto& reg = scene.Registry();

    const glm::mat4 vp = renderer.GetCamera().ViewProjection();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(vpX_, vpY_), ImVec2(vpX_ + vpW_, vpY_ + vpH_), true);

    const auto project = [&](const glm::vec3& world, ImVec2& out) -> bool {
        const glm::vec4 clip = vp * glm::vec4(world, 1.0f);
        if (clip.w <= 0.001f) return false; // behind the camera
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        out = ImVec2(vpX_ + (ndc.x * 0.5f + 0.5f) * vpW_, vpY_ + (0.5f - ndc.y * 0.5f) * vpH_);
        return true;
    };

    // --- Vector glyphs (centered at c, radius r). No texture assets needed. ---
    const float r = 9.0f;
    const auto gSun = [&](ImVec2 c, ImU32 col) {
        draw->AddCircleFilled(c, r * 0.5f, col);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.7853982f; // 45 deg
            const ImVec2 d(std::cos(a), std::sin(a));
            draw->AddLine(ImVec2(c.x + d.x * r * 0.8f, c.y + d.y * r * 0.8f),
                          ImVec2(c.x + d.x * r * 1.35f, c.y + d.y * r * 1.35f), col, 1.6f);
        }
    };
    const auto gPoint = [&](ImVec2 c, ImU32 col) {
        draw->AddCircleFilled(c, r * 0.5f, col);
        draw->AddCircle(c, r * 0.82f, col, 0, 1.4f);
        for (int i = 0; i < 4; ++i) {
            const float a = 0.7853982f + i * 1.5707963f; // diagonals
            const ImVec2 d(std::cos(a), std::sin(a));
            draw->AddLine(ImVec2(c.x + d.x * r * 0.95f, c.y + d.y * r * 0.95f),
                          ImVec2(c.x + d.x * r * 1.35f, c.y + d.y * r * 1.35f), col, 1.4f);
        }
    };
    const auto gSpot = [&](ImVec2 c, ImU32 col) { // downward cone
        const ImVec2 apex(c.x, c.y - r);
        const ImVec2 bl(c.x - r * 0.85f, c.y + r), br(c.x + r * 0.85f, c.y + r);
        draw->AddLine(apex, bl, col, 1.6f);
        draw->AddLine(apex, br, col, 1.6f);
        draw->AddBezierQuadratic(bl, ImVec2(c.x, c.y + r * 1.5f), br, col, 1.5f, 0);
        draw->AddCircleFilled(apex, 2.0f, col);
    };
    const auto gCamera = [&](ImVec2 c, ImU32 col) {
        draw->AddRect(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r * 0.3f, c.y + r * 0.6f),
                      col, 2.0f, 0, 1.6f); // body
        draw->AddTriangle(ImVec2(c.x + r * 0.3f, c.y - r * 0.35f),
                          ImVec2(c.x + r * 0.3f, c.y + r * 0.35f), ImVec2(c.x + r, c.y), col,
                          1.6f); // lens
    };
    const auto gZone = [&](ImVec2 c, ImU32 col) { // corner brackets
        const float e = r, k = r * 0.5f;
        const ImVec2 tl(c.x - e, c.y - e), tr(c.x + e, c.y - e), bl(c.x - e, c.y + e),
            brc(c.x + e, c.y + e);
        draw->AddLine(tl, ImVec2(tl.x + k, tl.y), col, 1.6f);
        draw->AddLine(tl, ImVec2(tl.x, tl.y + k), col, 1.6f);
        draw->AddLine(tr, ImVec2(tr.x - k, tr.y), col, 1.6f);
        draw->AddLine(tr, ImVec2(tr.x, tr.y + k), col, 1.6f);
        draw->AddLine(bl, ImVec2(bl.x + k, bl.y), col, 1.6f);
        draw->AddLine(bl, ImVec2(bl.x, bl.y - k), col, 1.6f);
        draw->AddLine(brc, ImVec2(brc.x - k, brc.y), col, 1.6f);
        draw->AddLine(brc, ImVec2(brc.x, brc.y - k), col, 1.6f);
    };
    const auto gAudio = [&](ImVec2 c, ImU32 col) { // speaker + wave
        draw->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.4f), ImVec2(c.x - r * 0.4f, c.y + r * 0.4f),
                            col);
        draw->AddTriangle(ImVec2(c.x - r * 0.4f, c.y - r * 0.8f),
                          ImVec2(c.x - r * 0.4f, c.y + r * 0.8f), ImVec2(c.x + r * 0.1f, c.y), col,
                          1.4f);
        draw->AddBezierQuadratic(ImVec2(c.x + r * 0.35f, c.y - r * 0.5f),
                                 ImVec2(c.x + r * 0.85f, c.y),
                                 ImVec2(c.x + r * 0.35f, c.y + r * 0.5f), col, 1.4f, 0);
    };
    const auto gSpline = [&](ImVec2 c, ImU32 col) { // three connected dots
        const ImVec2 p0(c.x - r, c.y + r * 0.5f), p1(c.x, c.y - r * 0.6f), p2(c.x + r, c.y + r * 0.5f);
        draw->AddLine(p0, p1, col, 1.3f);
        draw->AddLine(p1, p2, col, 1.3f);
        draw->AddCircleFilled(p0, 2.0f, col);
        draw->AddCircleFilled(p1, 2.0f, col);
        draw->AddCircleFilled(p2, 2.0f, col);
    };
    const auto gEmpty = [&](ImVec2 c, ImU32 col) { // small plus
        draw->AddLine(ImVec2(c.x - r * 0.7f, c.y), ImVec2(c.x + r * 0.7f, c.y), col, 1.4f);
        draw->AddLine(ImVec2(c.x, c.y - r * 0.7f), ImVec2(c.x, c.y + r * 0.7f), col, 1.4f);
    };

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool wantPick = vpClicked_ && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
                          !terrainConsumedClick_ && !paintConsumedClick_ && !splineConsumedClick_;
    entt::entity pickE = entt::null;
    float bestD2 = 16.0f * 16.0f; // pick radius (pixels)

    enum Kind { None, Sun, Point, Spot, Cam, Zone, Audio, Spline, Empty };
    for (const entt::entity e : reg.view<entt::entity>()) {
        if (reg.all_of<MeshInstance>(e)) continue; // already visible as geometry
        if (reg.any_of<UIElement, UICanvas, TerrainComponent, TerrainChunk>(e))
            continue; // screen-space or runtime-generated

        Kind kind = None;
        ImU32 col = IM_COL32(180, 180, 190, 230);
        if (reg.all_of<DirectionalLightComponent>(e)) { kind = Sun;   col = IM_COL32(255, 214, 92, 235); }
        else if (reg.all_of<SpotLightComponent>(e))   { kind = Spot;  col = IM_COL32(120, 210, 255, 235); }
        else if (reg.all_of<PointLightComponent>(e))  { kind = Point; col = IM_COL32(255, 230, 130, 235); }
        else if (reg.all_of<CameraComponent>(e))      { kind = Cam;   col = IM_COL32(230, 230, 235, 235); }
        else if (reg.all_of<CameraZone>(e))           { kind = Zone;  col = reg.get<CameraZone>(e).active
                                                                            ? IM_COL32(90, 235, 130, 235)
                                                                            : IM_COL32(90, 200, 235, 220); }
        else if (reg.all_of<AudioSource>(e))          { kind = Audio; col = IM_COL32(130, 225, 150, 235); }
        else if (reg.all_of<CameraSpline>(e))         { kind = Spline; col = IM_COL32(255, 220, 90, 230); }
        else if (reg.all_of<Transform>(e))            { kind = Empty; col = IM_COL32(150, 150, 160, 170); }
        if (kind == None) continue;

        ImVec2 c;
        if (!project(glm::vec3(scene.WorldMatrix(e)[3]), c)) continue;

        if (e == selected_) { // selection feedback (these have no AABB outline)
            draw->AddCircleFilled(c, r * 1.7f, IM_COL32(255, 160, 40, 60));
            draw->AddCircle(c, r * 1.7f, IM_COL32(255, 160, 40, 220), 0, 1.5f);
        }
        switch (kind) {
            case Sun:    gSun(c, col); break;
            case Point:  gPoint(c, col); break;
            case Spot:   gSpot(c, col); break;
            case Cam:    gCamera(c, col); break;
            case Zone:   gZone(c, col); break;
            case Audio:  gAudio(c, col); break;
            case Spline: gSpline(c, col); break;
            case Empty:  gEmpty(c, col); break;
            default: break;
        }

        if (wantPick) {
            const float dx = c.x - mouse.x, dy = c.y - mouse.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; pickE = e; }
        }
    }

    if (pickE != entt::null) {
        selected_ = pickE;
        iconConsumedClick_ = true; // selected an icon; don't also ray-pick a mesh
    }

    draw->PopClipRect();
}

void Editor::DrawCameraPreview(Engine& engine) {
    if (!showCameraPreview_ || !vpVisible_) return;
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    auto& reg = scene.Registry();
    if (selected_ == entt::null || !reg.valid(selected_)) return;
    const CameraComponent* camc = reg.try_get<CameraComponent>(selected_);
    if (!camc) return;
    if (previewSubmitted_) return; // the asset viewer's mesh preview owns the slot

    // PiP rect: bottom-right corner of the viewport, 16:9.
    const float pipW = glm::clamp(vpW_ * 0.28f, 160.0f, 460.0f);
    const float pipH = pipW * 9.0f / 16.0f;
    const float margin = 12.0f;
    const ImVec2 p1(vpX_ + vpW_ - margin, vpY_ + vpH_ - margin);
    const ImVec2 p0(p1.x - pipW, p1.y - pipH);

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(vpX_, vpY_), ImVec2(vpX_ + vpW_, vpY_ + vpH_), true);
    // Last frame's preview texture (1-frame lag, like the mesh preview).
    if (const u64 tex = renderer.PreviewTextureId()) {
        draw->AddImage(static_cast<ImTextureID>(tex), p0, p1);
    } else {
        draw->AddRectFilled(p0, p1, IM_COL32(18, 18, 22, 235));
    }
    draw->AddRect(p0, p1, IM_COL32(120, 200, 255, 235), 0, 0, 1.5f);
    const char* label = "Camera";
    if (const Name* n = reg.try_get<Name>(selected_); n && !n->value.empty())
        label = n->value.c_str();
    draw->AddRectFilled(ImVec2(p0.x, p0.y - 16.0f), ImVec2(p1.x, p0.y),
                        IM_COL32(18, 18, 22, 210));
    draw->AddText(ImVec2(p0.x + 5.0f, p0.y - 15.0f), IM_COL32(220, 230, 255, 255), label);
    draw->PopClipRect();

    // Submit this frame's preview from the camera's authored transform (looks
    // along local -Z). For target-driven modes this shows the placement; the
    // exact play-time pose depends on the runtime target.
    const glm::mat4 world = scene.WorldMatrix(selected_);
    const glm::vec3 eye = glm::vec3(world[3]);
    const glm::vec3 fwd = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
    glm::vec3 up = glm::vec3(world * glm::vec4(0, 1, 0, 0));
    up = glm::length(up) > 1e-5f ? glm::normalize(up) : glm::vec3(0, 1, 0);

    Camera cam;
    cam.SetPerspective(camc->fovY, pipW / pipH, glm::max(camc->nearZ, 0.001f),
                       glm::max(camc->farZ, camc->nearZ + 0.01f));
    cam.LookAt(eye, eye + fwd, up);

    const SceneEnvironment& env = scene.Environment();
    rhi::SceneView pv;
    pv.viewProj = cam.ViewProjection();
    pv.invViewProj = glm::inverse(pv.viewProj);
    pv.cameraPos = eye;
    pv.exposure = 1.0f;
    pv.ambientIntensity = env.ambientIntensity;
    pv.light.direction = glm::normalize(env.sun.direction);
    pv.light.color = env.sun.color;
    pv.light.intensity = env.sun.intensity;
    pv.irradianceIndex = env.irradiance.index;
    pv.prefilteredIndex = env.prefiltered.index;
    pv.brdfLUTIndex = env.brdfLUT.index;
    pv.prefilteredMaxLod = env.prefilteredMaxLod;
    pv.skyIndex = env.sky.index;

    cameraPreviewItems_.clear();
    scene.CollectDrawItems(cameraPreviewItems_);
    renderer.SetPreviewSize(static_cast<u32>(pipW), static_cast<u32>(pipH));
    renderer.SetPreviewScene(pv, cameraPreviewItems_);
    previewSubmitted_ = true;
}

void Editor::DrawNavigation(Engine& engine) {
    if (!panelOpen_[Panel_Navigation]) return;
    ImGui::Begin("Navigation", &panelOpen_[Panel_Navigation]);

    // --- Real-time A* (the live pathfinder agents use; no bake) ----------------
    ImGui::SeparatorText("Pathfinding (real-time A*)");
    ImGui::TextWrapped("Agents path on a grid A* built live from static geometry - no bake. "
                       "It reroutes around moving Navigation Obstacles each ~0.3s.");
    ImGui::SliderFloat("Cell size##grid", &gridParams_.cellSize, 0.25f, 2.0f, "%.2f m");
    ImGui::SliderFloat("Agent radius##grid", &gridParams_.agentRadius, 0.1f, 2.0f, "%.2f");
    ImGui::SliderFloat("Max step##grid", &gridParams_.maxStep, 0.1f, 2.0f, "%.2f");
    ImGui::SliderFloat("Max slope##grid", &gridParams_.maxSlopeDeg, 10.0f, 70.0f, "%.0f deg");
    const std::filesystem::path navAssets =
        Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
    const auto gatherObstacles = [&]() {
        std::vector<nav::GridObstacle> obs;
        entt::registry& r = engine.GetScene().Registry();
        for (const entt::entity e : r.view<Transform, NavigationObstacle>()) {
            const NavigationObstacle& o = r.get<NavigationObstacle>(e);
            if (o.enabled) obs.push_back({glm::vec3(engine.GetScene().WorldMatrix(e)[3]), o.radius});
        }
        return obs;
    };
    if (ImGui::Button("Rebuild Grid")) {
        EnsureLevelMembership(engine.GetScene());
        nav::GridNav& g = engine.GetGridNav();
        g.SetParams(gridParams_);
        g.Rebuild(engine.GetScene(), navAssets);
        g.DebugCells(navStart_, 60.0f, navCells_);
        navBuilt_ = g.Ready();
        navStatus_ = g.Ready() ? ("Grid: " + std::to_string(g.TriangleCount()) +
                                  " static tris, " + std::to_string(navCells_.size()) +
                                  " walkable cells near start.")
                               : "Grid: no static geometry found.";
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show##grid", &navShow_);
    ImGui::DragFloat3("Start##grid", &navStart_.x, 0.1f);
    ImGui::DragFloat3("End##grid", &navEnd_.x, 0.1f);
    if (selected_ != entt::null && engine.GetScene().Registry().valid(selected_)) {
        if (ImGui::SmallButton("Start = selection##g"))
            navStart_ = glm::vec3(engine.GetScene().WorldMatrix(selected_)[3]);
        ImGui::SameLine();
        if (ImGui::SmallButton("End = selection##g"))
            navEnd_ = glm::vec3(engine.GetScene().WorldMatrix(selected_)[3]);
    }
    if (ImGui::Button("Find Path (A*)")) {
        nav::GridNav& g = engine.GetGridNav();
        g.SetParams(gridParams_);
        g.EnsureBuilt(engine.GetScene(), navAssets);
        navPath_ = g.FindPath(navStart_, navEnd_, gatherObstacles());
        g.DebugCells(navStart_, 60.0f, navCells_);
        navBuilt_ = g.Ready();
        navStatus_ = navPath_.empty()
                         ? "A*: no path (check there is static ground + a reachable goal)."
                         : ("A*: " + std::to_string(navPath_.size()) + " corners.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        navBuilt_ = false;
        navPath_.clear();
        navCells_.clear();
        navStatus_.clear();
    }
    if (navBuilt_ && !navPath_.empty()) {
        f32 len = 0.0f;
        for (usize i = 1; i < navPath_.size(); ++i)
            len += glm::distance(navPath_[i - 1], navPath_[i]);
        ImGui::Text("Path: %d corners, length %.1f", static_cast<int>(navPath_.size()), len);
    }
    if (!navStatus_.empty()) ImGui::TextWrapped("%s", navStatus_.c_str());

    // Input geometry: NavmeshInput tags restrict which static meshes the A* grid
    // samples (otherwise every static mesh is used).
    {
        entt::registry& reg = engine.GetScene().Registry();
        int tagged = 0;
        for (const entt::entity e : reg.view<NavmeshInput>()) {
            if (reg.get<NavmeshInput>(e).enabled) ++tagged;
        }
        ImGui::SeparatorText("Input geometry");
        if (tagged > 0) {
            ImGui::TextWrapped("Baking %d tagged mesh%s (Navmesh Input).", tagged,
                               tagged == 1 ? "" : "es");
        } else {
            ImGui::TextWrapped("No tags - baking ALL meshes. Add a Navmesh Input "
                               "component to limit the bake.");
        }
        const bool hasSel = selected_ != entt::null && reg.valid(selected_);
        ImGui::BeginDisabled(!hasSel);
        if (ImGui::Button("Tag Selected")) {
            if (hasSel && !reg.all_of<NavmeshInput>(selected_)) {
                PushUndo(engine.GetScene());
                reg.emplace<NavmeshInput>(selected_);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Untag Selected")) {
            if (hasSel && reg.all_of<NavmeshInput>(selected_)) {
                PushUndo(engine.GetScene());
                reg.remove<NavmeshInput>(selected_);
            }
        }
        ImGui::EndDisabled();
    }

    ImGui::End();
}


void Editor::DrawNavOverlay(Scene& scene, Renderer& renderer) {
    (void)scene;
    if (!vpVisible_ || !navBuilt_ || !navShow_) return;
    const glm::mat4 vp = renderer.GetCamera().ViewProjection();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(vpX_, vpY_), ImVec2(vpX_ + vpW_, vpY_ + vpH_), true);

    const auto project = [&](const glm::vec3& w, ImVec2& out) -> bool {
        const glm::vec4 clip = vp * glm::vec4(w, 1.0f);
        if (clip.w <= 0.001f) return false; // behind the camera
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        out = ImVec2(vpX_ + (ndc.x * 0.5f + 0.5f) * vpW_, vpY_ + (0.5f - ndc.y * 0.5f) * vpH_);
        return true;
    };

    // Real-time A* walkable cells (small dots).
    const ImU32 cellCol = IM_COL32(80, 200, 120, 120);
    for (const glm::vec3& p : navCells_) {
        ImVec2 s;
        if (project(p, s)) draw->AddRectFilled(ImVec2(s.x - 1.5f, s.y - 1.5f),
                                               ImVec2(s.x + 1.5f, s.y + 1.5f), cellCol);
    }

    if (navPath_.size() >= 2) {
        const ImU32 pathCol = IM_COL32(255, 220, 40, 255);
        for (usize i = 1; i < navPath_.size(); ++i) {
            ImVec2 a, b;
            if (project(navPath_[i - 1], a) && project(navPath_[i], b))
                draw->AddLine(a, b, pathCol, 3.0f);
        }
        for (const glm::vec3& p : navPath_) {
            ImVec2 s;
            if (project(p, s)) draw->AddCircleFilled(s, 4.0f, pathCol);
        }
    }
    draw->PopClipRect();
}

void Editor::DrawStreaming(Engine& engine) {
    if (!panelOpen_[Panel_Streaming]) return;
    StreamingWorld& world = engine.GetStreamingWorld();
    ImGui::Begin("Streaming", &panelOpen_[Panel_Streaming]);
    ImGui::TextDisabled("World partition: cells stream in/out around the camera.");

    static char pathBuf[256] = "World.hbworld";
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##worldpath", pathBuf, sizeof(pathBuf));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        if (Project::HasActive()) {
            const std::filesystem::path assets = Project::Active().AssetsDir();
            const std::filesystem::path manifest = assets / pathBuf;
            streamStatus_ = world.LoadManifest(manifest, assets)
                                ? std::string("Loaded ") + pathBuf
                                : std::string("Failed to load ") + pathBuf;
        } else {
            streamStatus_ = "No project open.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Unload All")) {
        world.UnloadAll(engine.GetScene());
        streamStatus_ = "Unloaded all cells.";
    }
    ImGui::TextDisabled(".hbworld path relative to the project's Assets/.");
    ImGui::TextDisabled("A cell's scene may be a level (streams static + dynamic).");

    // Whole-world editing: load every cell at once and pause distance streaming
    // so nothing unloads while you edit. Each cell's entities stay tagged to
    // their scene/level files, so a normal Save writes them all back.
    ImGui::Separator();
    if (ImGui::Button("Load All (edit world)")) {
        world.LoadAll(engine.GetScene(), engine.GetRenderer());
        streamStatus_ = "Loaded all cells; streaming paused for editing.";
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(world.Enabled());
    if (ImGui::Button("Resume Streaming")) {
        world.SetEnabled(true);
        streamStatus_ = "Distance streaming resumed.";
    }
    ImGui::EndDisabled();
    if (!world.Enabled()) ImGui::TextDisabled("Streaming paused (editing the whole world).");

    const StreamingWorld::Stats st = world.GetStats();
    ImGui::Separator();
    ImGui::Text("Cells: %u total, %u loaded, %u loading", st.cells, st.loaded, st.loading);
    ImGui::Text("Streamed entities: %u", st.entities);
    if (!streamStatus_.empty()) ImGui::TextWrapped("%s", streamStatus_.c_str());
    ImGui::End();
}

void Editor::ExtendSpline(Scene& scene, CameraSpline& sp, bool atStart) {
    PushUndo(scene);
    const int n = static_cast<int>(sp.points.size());
    if (n == 0) {
        sp.points.push_back(glm::vec3(0.0f));
        splinePoint_ = 0;
        return;
    }
    // Extrapolate along the end segment (fallback +X for a single point).
    if (atStart) {
        glm::vec3 dir = (n >= 2) ? (sp.points[0] - sp.points[1]) : glm::vec3(2.0f, 0.0f, 0.0f);
        if (glm::length(dir) < 1e-4f) dir = glm::vec3(2.0f, 0.0f, 0.0f);
        sp.points.insert(sp.points.begin(), sp.points.front() + dir);
        splinePoint_ = 0;
    } else {
        glm::vec3 dir =
            (n >= 2) ? (sp.points[n - 1] - sp.points[n - 2]) : glm::vec3(2.0f, 0.0f, 0.0f);
        if (glm::length(dir) < 1e-4f) dir = glm::vec3(2.0f, 0.0f, 0.0f);
        sp.points.push_back(sp.points.back() + dir);
        splinePoint_ = static_cast<int>(sp.points.size()) - 1;
    }
}

// Drives the selected CameraSpline: a translate gizmo on the active control
// point, click-to-select a point, and Tab to extend the active end. Runs inside
// the Viewport window (see DrawGizmo), so ImGuizmo and the mouse line up.
void Editor::EditCameraSpline(Engine& engine, CameraSpline& sp) {
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();

    // Reset the active point when the selected spline changes; clamp otherwise.
    if (splineEntity_ != selected_) {
        splineEntity_ = selected_;
        splinePoint_ = static_cast<int>(sp.points.size()) - 1;
    }
    if (!sp.points.empty() &&
        (splinePoint_ < 0 || splinePoint_ >= static_cast<int>(sp.points.size()))) {
        splinePoint_ = static_cast<int>(sp.points.size()) - 1;
    }

    const Camera& cam = renderer.GetCamera();
    const glm::mat4 view = cam.View();
    const glm::mat4 proj = cam.Projection();
    const glm::mat4 vp = cam.ViewProjection();
    const auto project = [&](const glm::vec3& world, ImVec2& out) -> bool {
        const glm::vec4 clip = vp * glm::vec4(world, 1.0f);
        if (clip.w <= 0.001f) return false;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        out = ImVec2(vpX_ + (ndc.x * 0.5f + 0.5f) * vpW_, vpY_ + (0.5f - ndc.y * 0.5f) * vpH_);
        return true;
    };

    // Translate gizmo on the active point (points are world-space positions).
    if (splinePoint_ >= 0 && splinePoint_ < static_cast<int>(sp.points.size())) {
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(vpX_, vpY_, vpW_, vpH_);
        glm::mat4 model(1.0f);
        model[3] = glm::vec4(sp.points[splinePoint_], 1.0f);
        if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                 ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(model))) {
            if (!gizmoEditing_) {
                gizmoEditing_ = true;
                PushUndo(scene);
            }
            sp.points[splinePoint_] = glm::vec3(model[3]);
        }
        if (!ImGuizmo::IsUsing()) gizmoEditing_ = false;
    }

    // Click a point (when not driving the gizmo) to make it the active point.
    if (vpClicked_ && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int best = -1;
        float bestD2 = 14.0f * 14.0f; // pick radius (pixels)
        for (int i = 0; i < static_cast<int>(sp.points.size()); ++i) {
            ImVec2 a;
            if (!project(sp.points[i], a)) continue;
            const float dx = a.x - mouse.x, dy = a.y - mouse.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; best = i; }
        }
        if (best >= 0) {
            splinePoint_ = best;
            splineConsumedClick_ = true; // keep the spline selected; don't re-pick
        }
    }

    // Tab extends the path from the active end (start if the first point is
    // active, else the end), extrapolating the end segment.
    const ImGuiIO& io = ImGui::GetIO();
    if (vpHovered_ && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        ExtendSpline(scene, sp, splinePoint_ == 0);
    }
}

void Editor::DrawGizmo(Engine& engine) {
    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    auto& reg = scene.Registry();
    // Physics must know which entity the user is hand-manipulating (its body
    // follows the Transform instead of overwriting it mid-simulation).
    PhysicsWorld& physics = engine.GetPhysics();
    physics.SetEditedEntity(entt::null);
    splineConsumedClick_ = false;
    if (selected_ == entt::null || !reg.valid(selected_)) return;
    // The terrain sculpt brush owns viewport drags; hide the gizmo for it.
    if (terrainSculpt_ && reg.all_of<TerrainComponent>(selected_)) return;

    // Camera spline: the gizmo edits one CONTROL POINT (world-space) instead of
    // the entity transform. Click a point to select it; Tab extends the active end.
    if (CameraSpline* sp = reg.try_get<CameraSpline>(selected_)) {
        EditCameraSpline(engine, *sp);
        return;
    }

    Transform* t = reg.try_get<Transform>(selected_);
    if (!t) return;

    ImGuizmo::SetOrthographic(false);
    // Draw into the Viewport window's own draw list (see DrawViewport).
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(vpX_, vpY_, vpW_, vpH_);

    const Camera& cam = renderer.GetCamera();
    glm::mat4 view = cam.View();
    glm::mat4 proj = cam.Projection();
    glm::mat4 model = scene.WorldMatrix(selected_); // gizmo operates in world space

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (gizmoMode_ == 1) op = ImGuizmo::ROTATE;
    else if (gizmoMode_ == 2) op = ImGuizmo::SCALE;

    // Grid snapping: ImGuizmo rounds to `snap` while dragging (translate = metres,
    // rotate = degrees, scale = factor). Hold Ctrl to snap even when the toggle is off.
    const f32 snapVal = op == ImGuizmo::ROTATE  ? gizmoSnapAngle_
                        : op == ImGuizmo::SCALE ? gizmoSnapScale_
                                                : gizmoSnapStep_;
    const f32 snap[3] = {snapVal, snapVal, snapVal};
    const bool doSnap = gizmoSnap_ || ImGui::GetIO().KeyCtrl;

    if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op,
                             ImGuizmo::WORLD, glm::value_ptr(model), nullptr,
                             doSnap ? snap : nullptr)) {
        // One undo step per drag, captured before the first write-back (the
        // scene still holds the pre-drag transform at this point).
        if (!gizmoEditing_) {
            gizmoEditing_ = true;
            PushUndo(scene);
        }
        // Convert the manipulated world matrix back into the entity's local
        // space (relative to its parent, if any) and store it as TRS.
        glm::mat4 parentWorld(1.0f);
        if (const Parent* p = reg.try_get<Parent>(selected_); p && reg.valid(p->entity)) {
            parentWorld = scene.WorldMatrix(p->entity);
        }
        DecomposeTRS(glm::inverse(parentWorld) * model, *t);
    }
    if (!ImGuizmo::IsUsing()) gizmoEditing_ = false; // drag ended
    if (ImGuizmo::IsUsing()) physics.SetEditedEntity(selected_);
}

} // namespace hbe
