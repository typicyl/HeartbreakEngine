// HubApp/main_hubapp.cpp - Heartbreak Hub. A STANDALONE launcher.
//
// This program links NO ENGINE CODE. Not the renderer, not the scene, not the editor,
// not Jolt or assimp or miniaudio. That is the entire design and it is not tidiness:
//
//   * The Hub must be able to run when the engine is BROKEN, HALF-INSTALLED, or ABSENT -
//     that is precisely when someone opens a launcher. The previous hub linked the whole
//     editor, so it imported vulkan-1.dll, d3d12.dll, opengl32.dll and mfreadwrite.dll.
//     vulkan-1.dll is installed by GPU DRIVERS, not by Windows, so on a fresh machine or
//     a VM the launcher failed to load BEFORE main() - the one program meant to repair a
//     broken install could not start without one.
//   * The Hub SWAPS the engine's bin/ directory during an update. A process cannot
//     rename the directory it is running from, so it must live outside the payload.
//   * It was 20.5 MB and relinked on every engine commit; "update the engine" and
//     "update the Hub" were the same operation.
//
// Rendering is D3D11 with a WARP (software) fallback. D3D11 ships with Windows and WARP
// always works, so the launcher draws on a machine with no usable GPU driver - again,
// exactly the machine that needs it.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shobjidl.h>

#include "Hub/HubConfig.h"
#include "Hub/HubSelfUpdate.h"
#include "Hub/HubJoin.h"
#include "Hub/ProjectCatalog.h"
#include "Hub/UpdateCheck.h"
#include "Hub/Updater.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_usingWarp = false;

void CreateRtv() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    // HARDWARE first, then WARP. WARP is Microsoft's software rasterizer - it ships with
    // Windows and needs no driver, which is what lets the launcher start on a machine
    // whose GPU stack is missing or broken. Without this fallback the Hub would fail on
    // exactly the machines it exists to help.
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                 0, want, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                                 &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {
        g_usingWarp = true;
        hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want,
                                             2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got,
                                             &g_ctx);
    }
    if (FAILED(hr)) return false;
    CreateRtv();
    return true;
}

void DestroyDevice() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_dev && wp != SIZE_MINIMIZED) {
                if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
                g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                CreateRtv();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0; // ALT should not open a system menu
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// A native folder/file picker, so the Hub does not need the editor's asset browser.
std::wstring PickFile(HWND owner, bool folder) {
    std::wstring result;
    IFileDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg))))
        return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | (folder ? FOS_PICKFOLDERS : 0) | FOS_FORCEFILESYSTEM);
    if (!folder) {
        COMDLG_FILTERSPEC f[] = {{L"Heartbreak project", L"*.hbproj"}};
        dlg->SetFileTypes(1, f);
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                ::CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

void ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.11f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.28f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.32f, 0.42f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.40f, 0.55f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.20f, 0.26f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.30f, 0.40f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.19f, 1.0f);
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                          nullptr, nullptr);
    return s;
}

} // namespace

// WinMain, not main: the Hub is a GUI-subsystem app so double-clicking it does not
// flash a console window. A launcher that opens a black box first looks broken.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // If the PREVIOUS Hub replaced itself, its old binary is sitting beside this one under
    // a different name. This is the first moment it is no longer running and can actually
    // be deleted - a self-update cannot tidy up after itself from inside the process it is
    // replacing.
    hbe::hub::CleanupAfterSelfUpdate();

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, ::GetModuleHandleW(nullptr),
                   nullptr, ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), nullptr, nullptr,
                   L"HeartbreakHubWnd", nullptr};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Heartbreak Hub", WS_OVERLAPPEDWINDOW,
                                100, 100, 1000, 660, nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDevice(hwnd)) {
        // A launcher that cannot draw must SAY SO. Failing silently is what the old one
        // did (it simply never appeared) and it is unanswerable for a user.
        ::MessageBoxW(nullptr,
                      L"Heartbreak Hub could not create a Direct3D 11 device, even in "
                      L"software (WARP) mode.\n\nThis usually means the Windows graphics "
                      L"stack is damaged.",
                      L"Heartbreak Hub", MB_ICONERROR | MB_OK);
        DestroyDevice();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The Hub keeps its OWN layout file, in the per-user data dir. Writing next to the
    // executable would be wiped by every update, and sharing the editor's would let a
    // launcher tweak move editor panels.
    static std::string iniPath =
        (hbe::hub::RecentProjectsFile().parent_path() / "hub_layout.ini").string();
    io.IniFilename = iniPath.c_str();
    ApplyTheme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // --- state ---
    std::vector<hbe::hub::ProjectEntry> projects = hbe::hub::LoadProjects();
    // WHERE THE ENGINE LIVES is configuration now, not an assumption about where the Hub
    // sits. A standalone Hub can be anywhere - Downloads, a USB stick - and the engine it
    // manages is somewhere else entirely.
    hbe::hub::HubConfig cfg = hbe::hub::LoadHubConfig();
    if (cfg.installRoot.empty()) {
        cfg.installRoot = hbe::hub::DefaultInstallRoot();
        // WRITE IT NOW. The install root has to be on disk before anything is installed
        // into it, or a crash between install and shutdown loses the only record of
        // WHERE the engine went - and the next launch would look in the default place
        // for something that might be elsewhere.
        hbe::hub::SaveHubConfig(cfg);
    }
    hbe::hub::UpdatePaths paths;
    paths.installRoot = cfg.installRoot;
    hbe::hub::Updater updater(cfg.manifestUrl, paths);
    // The INSTALLED version, read off disk - never the Hub own compile-time constant.
    // They are separate downloads with separate release cadences, and reporting the Hub
    // number would claim an engine the user may not have.
    updater.SetInstalledVersion(hbe::hub::ReadInstalledVersion(cfg.installRoot));
    updater.CleanWorkspace();
    bool installed = hbe::hub::LooksInstalled(cfg.installRoot);
    // Hub self-update state. Deliberately plain locals: it is a three-state flow (nothing /
    // staged / failed) that lives entirely inside this loop.
    bool hubSelfStaged = hbe::hub::SelfUpdateStaged();
    std::string hubSelfStatus, hubSelfError;

    std::string status, rollbackMsg;
    // Joining a colleague's session to fetch a project - see HubJoin.h.
    hbe::hub::JoinSession join;
    bool showJoin = false;
    char joinPath[512] = {};
    bool checking = false;
    // Latches the one-shot "record the install" work so it does not re-run every frame
    // while the Done screen is up.
    bool persistedDone = false;
    std::thread worker;
    // The network runs on a WORKER THREAD so a slow or black-holed server cannot freeze
    // the window. The UI only ever READS the progress struct; the worker only writes it,
    // and `checking` gates the transition - one owner at a time, no lock needed.
    const auto joinWorker = [&] {
        if (worker.joinable()) worker.join();
        checking = false;
    };

    bool running = true;
    while (running) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        // THE TRANSPORT PUMP BELONGS HERE, not inside the popup body where it used to
        // live. ImGui closes a modal by itself when it is completely clipped - e.g. when
        // this window is minimised - which stopped the transfer dead, mid-file, with no
        // state change on either end and nothing on screen to say so.
        join.Tick();

        if (checking) {
            const auto st = updater.Progress().state;
            if (st != hbe::hub::UpdateState::Checking &&
                st != hbe::hub::UpdateState::Downloading &&
                st != hbe::hub::UpdateState::Verifying &&
                st != hbe::hub::UpdateState::Installing)
                joinWorker();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ONE full-window layout, not floating panels. A launcher is a fixed screen; the
        // old hub was a draggable ImGui window in the middle of an empty viewport, which
        // read as a debug overlay rather than an application.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##hub", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 1.0f, 1.0f));
        ImGui::TextUnformatted("Heartbreak Engine");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("v%s", hbe::hub::CurrentEngineVersion().ToString().c_str());
        if (g_usingWarp) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(software rendering)");
        }
        ImGui::Separator();

        // ---------------- projects ----------------
        ImGui::BeginChild("##projects", ImVec2(vp->WorkSize.x * 0.62f, -60), true);
        ImGui::TextUnformatted("Projects");
        ImGui::Spacing();
        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
            const hbe::hub::ProjectEntry& p = projects[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::BeginDisabled(p.missing);
            if (ImGui::Button("Open", ImVec2(70, 0))) {
                std::string err;
                if (hbe::hub::LaunchEditor(p.file, err)) {
                    hbe::hub::TouchProject(projects, p.file);
                    hbe::hub::SaveProjects(projects);
                    status = "Launched " + p.name + ".";
                } else {
                    status = err;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (p.missing) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", p.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(missing)");
            } else {
                ImGui::TextUnformatted(p.name.c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", p.file.parent_path().string().c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 10.0f);
            if (ImGui::SmallButton("x")) removeIdx = i;
            ImGui::PopID();
        }
        if (projects.empty())
            ImGui::TextDisabled("No projects yet - open one below.");
        if (removeIdx >= 0) {
            hbe::hub::RemoveProject(projects, projects[static_cast<size_t>(removeIdx)].file);
            hbe::hub::SaveProjects(projects);
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // ---------------- engine / updates ----------------
        ImGui::BeginChild("##engine", ImVec2(0, -60), true);
        ImGui::TextUnformatted("Engine");
        ImGui::Spacing();
        const hbe::hub::UpdateProgress& pr = updater.Progress();

        // THREE DISTINCT STATES, worded differently on purpose. "Not installed" is not
        // an error and must not read like one - it is where every new user starts.
        if (!installed) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Not installed");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("The Hub will download and install the engine for you.");
            ImGui::PopTextWrapPos();
        } else if (const auto v = hbe::hub::ReadInstalledVersion(cfg.installRoot)) {
            ImGui::Text("Installed: %s", v->ToString().c_str());
        } else {
            // Present but unstamped: offer a REPAIR, never a guess. Guessing a version is
            // how a Hub decides you are up to date and refuses to fix a broken install.
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Installed: unknown version");
            ImGui::TextDisabled("Reinstall to restore a known state.");
        }
        ImGui::TextDisabled("%s", cfg.installRoot.string().c_str());
        if (ImGui::SmallButton("Change location...")) {
            const std::wstring picked = PickFile(hwnd, true);
            if (!picked.empty()) {
                cfg.installRoot = picked;
                hbe::hub::SaveHubConfig(cfg);
                updater.SetInstallRoot(cfg.installRoot);
                updater.SetInstalledVersion(hbe::hub::ReadInstalledVersion(cfg.installRoot));
                installed = hbe::hub::LooksInstalled(cfg.installRoot);
            }
        }
        ImGui::Separator();

        switch (pr.state) {
            case hbe::hub::UpdateState::Available:
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", pr.message.c_str());
                ImGui::PopTextWrapPos();
                break;
            case hbe::hub::UpdateState::UpToDate:
            case hbe::hub::UpdateState::Done:
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", pr.message.c_str());
                ImGui::PopTextWrapPos();
                break;
            case hbe::hub::UpdateState::Failed:
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", pr.message.c_str());
                ImGui::PopTextWrapPos();
                break;
            default:
                if (!pr.message.empty()) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(pr.message.c_str());
                    ImGui::PopTextWrapPos();
                }
                break;
        }
        if (pr.state == hbe::hub::UpdateState::Downloading && pr.bytesTotal > 0) {
            char lbl[64];
            std::snprintf(lbl, sizeof(lbl), "%.1f / %.1f MB", pr.bytesDone / 1048576.0,
                          pr.bytesTotal / 1048576.0);
            ImGui::ProgressBar(static_cast<float>(static_cast<double>(pr.bytesDone) /
                                                  static_cast<double>(pr.bytesTotal)),
                               ImVec2(-1, 0), lbl);
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(checking);
        if (ImGui::Button(installed ? "Check for updates" : "Find engine", ImVec2(-1, 0))) {
            checking = true;
            worker = std::thread([&updater] { updater.Check(); });
        }
        if (pr.state == hbe::hub::UpdateState::Available) {
            const bool fresh = updater.IsFreshInstall();
            if (ImGui::Button(fresh ? "Install engine" : "Download and install", ImVec2(-1, 0))) {
                checking = true;
                worker = std::thread([&updater] {
                    updater.Apply([](const hbe::hub::UpdateProgress&) { return true; });
                });
            }
        }
        // Reinstall is the REPAIR path: it re-runs the same install over a broken or
        // unstamped tree. Clearing the installed version first is what makes Check treat
        // the published build as installable rather than reporting "up to date" and
        // leaving a damaged install with no way out.
        if (installed && (pr.state == hbe::hub::UpdateState::UpToDate ||
                          pr.state == hbe::hub::UpdateState::Failed)) {
            if (ImGui::Button("Reinstall / repair", ImVec2(-1, 0))) {
                checking = true;
                worker = std::thread([&updater] {
                    updater.SetInstalledVersion(std::nullopt);
                    updater.Check();
                    updater.Apply([](const hbe::hub::UpdateProgress&) { return true; });
                });
            }
        }
        ImGui::EndDisabled();

        // REMEMBER IT. Runs on every transition INTO Done, not just a fresh install:
        // an update changes the recorded version too, and a Hub that only persisted the
        // first install would report a stale version forever after. Re-read from disk
        // rather than trusting the in-memory value - the stamp is the truth, and if the
        // swap somehow left the tree unusable we want to know that here, not later.
        if (pr.state == hbe::hub::UpdateState::Done && !persistedDone) {
            persistedDone = true;
            installed = hbe::hub::LooksInstalled(cfg.installRoot);
            cfg.installedVersion = hbe::hub::ReadInstalledVersion(cfg.installRoot);
            hbe::hub::SaveHubConfig(cfg);
            updater.SetInstalledVersion(cfg.installedVersion);
        }
        if (pr.state != hbe::hub::UpdateState::Done) persistedDone = false;

        // --- The Hub updating ITSELF -----------------------------------------
        // Separate from the engine update on purpose: they are different downloads with
        // different versions, and the Hub is deliberately NOT part of the payload it swaps.
        ImGui::Spacing();
        ImGui::SeparatorText("The Hub itself");
        // THE HUB'S VERSION IS THE ENGINE'S VERSION. It ships inside the engine payload,
        // so reporting its own compile-time constant was reporting the version of whatever
        // build someone happened to compile it from - permanently 1.0.0 no matter what
        // engine it was actually delivered with. Prefer the INSTALLED engine's stamp, which
        // is what the payload that carried this Hub wrote; fall back to the build constant
        // only when nothing is installed yet, where it is the honest answer.
        {
            const auto stamped = hbe::hub::ReadInstalledVersion(cfg.installRoot);
            const std::string shown = stamped ? stamped->ToString()
                                              : hbe::hub::CurrentEngineVersion().ToString();
            ImGui::TextDisabled("Hub %s%s  -  %s", shown.c_str(),
                                stamped ? "" : " (no engine installed)",
                                hbe::hub::HubExePath().parent_path().string().c_str());
        }
        // The engine update may have brought a newer Hub with it - every engine archive
        // ships the matching launcher, so this is the normal way the Hub updates now.
        if (pr.hubUpdateStaged && !hubSelfStaged) {
            hubSelfStaged = true;
            hubSelfStatus = "A newer Hub came with the engine and is ready.";
        }
        if (!pr.hubUpdateNote.empty() && hubSelfError.empty())
            hubSelfError = pr.hubUpdateNote;
        if (hubSelfError.empty() && !hubSelfStaged) {
            ImGui::TextDisabled("The Hub updates with the engine.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Every engine archive carries the matching Hub, so installing or "
                    "updating the engine updates this launcher too.\n\n"
                    "There is deliberately no separate Hub download: the manifest publishes "
                    "ONE release URL - the engine archive - and a button that fetched it as "
                    "though it were a Hub executable renamed a .zip over this program.");
        }
        if (hubSelfStaged) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", hubSelfStatus.c_str());
            ImGui::TextDisabled("The Hub will restart to finish.");
            if (ImGui::Button("Restart and update the Hub")) {
                std::string err;
                if (!hbe::hub::ApplySelfUpdate(err)) {
                    hubSelfError = err;
                    hubSelfStaged = false;
                } else if (!hbe::hub::RelaunchHub(err)) {
                    // The swap SUCCEEDED, so the new Hub is in place - only the restart
                    // failed. Say exactly that; telling the user the update failed would
                    // send them re-downloading something they already have.
                    hubSelfError = "The Hub was updated but could not restart itself. "
                                   "Close and reopen it. (" + err + ")";
                    hubSelfStaged = false;
                } else {
                    running = false; // the replacement is already starting
                }
            }
        } else if (!hubSelfStatus.empty()) {
            ImGui::TextDisabled("%s", hubSelfStatus.c_str());
        }
        if (!hubSelfError.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", hubSelfError.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::SmallButton("Dismiss")) hubSelfError.clear();
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Advanced")) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Updates are fetched over HTTPS with certificate validation. "
                                "A published sha256 is enforced; this manifest does not "
                                "include one yet, so integrity rests on HTTPS alone.");
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Roll back previous build", ImVec2(-1, 0))) {
                std::string err;
                rollbackMsg = updater.Rollback(err) ? "Rolled back. Restart the Hub." : err;
            }
            if (!rollbackMsg.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(rollbackMsg.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::EndChild();

        // ---------------- footer ----------------
        if (ImGui::Button("Open project...")) {
            const std::wstring picked = PickFile(hwnd, /*folder*/ false);
            if (!picked.empty()) {
                std::string err;
                if (hbe::hub::LaunchEditor(picked, err)) {
                    hbe::hub::TouchProject(projects, picked);
                    hbe::hub::SaveProjects(projects);
                    status = "Launched " + Utf8(picked);
                } else {
                    status = err;
                }
            }
        }
        ImGui::SameLine();
        // GET A PROJECT FROM A COLLEAGUE. The reason this lives in the LAUNCHER and not
        // only in the editor: the person who needs it has no project to open, so
        // requiring them to reach the editor's Collaborate panel first meant inventing an
        // empty project purely to get somewhere they could download the real one.
        if (ImGui::Button("Get a project from someone...")) {
            join.LoadIdentity(hbe::hub::IdentityFileForHub());
            showJoin = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) projects = hbe::hub::LoadProjects();
        ImGui::SameLine();
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());

        if (showJoin) {
            ImGui::OpenPopup("Get a project");
            showJoin = false;
        }
        if (ImGui::BeginPopupModal("Get a project", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            // join.Tick() runs in the frame loop above - drawing must not be what drives
            // the network.
            ImGui::TextDisabled("Your fingerprint (read this out so they can let you in):");
            ImGui::Text("%s", join.Fingerprint().c_str());
            ImGui::SameLine();
            if (ImGui::Button("Copy")) ImGui::SetClipboardText(join.Fingerprint().c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", join.Status().empty()
                                         ? hbe::hub::JoinSession::StepName(join.State())
                                         : join.Status().c_str());
            ImGui::Separator();

            switch (join.State()) {
            case hbe::hub::JoinSession::Step::NeedInvitation:
                if (ImGui::Button("Paste their invitation")) {
                    if (const char* t = ImGui::GetClipboardText()) join.Paste(t);
                }
                break;
            case hbe::hub::JoinSession::Step::Confirm:
                ImGui::TextWrapped("This invitation says it is from:");
                ImGui::Text("%s", join.HostFingerprint().c_str());
                ImGui::TextWrapped("Check that against what they told you.");
                ImGui::InputTextWithHint("##into", "folder to put the project in",
                                         joinPath, sizeof(joinPath));
                ImGui::SameLine();
                if (ImGui::Button("Browse...")) {
                    const std::wstring d = PickFile(hwnd, /*folder*/ true);
                    if (!d.empty())
                        std::snprintf(joinPath, sizeof(joinPath), "%s", Utf8(d).c_str());
                }
                ImGui::BeginDisabled(joinPath[0] == 0);
                if (ImGui::Button("Connect")) join.Confirm(joinPath);
                ImGui::EndDisabled();
                break;
            case hbe::hub::JoinSession::Step::Connecting:
                if (!join.Reply().empty()) {
                    ImGui::TextWrapped("Send this reply back to them:");
                    if (ImGui::Button("Copy the reply"))
                        ImGui::SetClipboardText(join.Reply().c_str());
                }
                break;
            case hbe::hub::JoinSession::Step::Fetching:
                ImGui::Text("%d / %d file(s), %.1f MB", static_cast<int>(join.FilesDone()),
                            static_cast<int>(join.FilesTotal()),
                            static_cast<double>(join.Bytes()) / (1024.0 * 1024.0));
                break;
            case hbe::hub::JoinSession::Step::Done: {
                const std::filesystem::path proj = join.ProjectFile();
                ImGui::BeginDisabled(proj.empty());
                if (ImGui::Button("Open it")) {
                    std::string err;
                    if (hbe::hub::LaunchEditor(proj.wstring(), err)) {
                        hbe::hub::TouchProject(projects, proj.wstring());
                        hbe::hub::SaveProjects(projects);
                        status = "Launched " + proj.filename().string();
                        join.Cancel();
                        ImGui::CloseCurrentPopup();
                    } else {
                        status = err;
                    }
                }
                ImGui::EndDisabled();
                break;
            }
            default:
                break;
            }

            ImGui::Separator();
            if (ImGui::Button("Close")) {
                join.Cancel();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.06f, 0.06f, 0.08f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    joinWorker();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyDevice();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ::CoUninitialize();
    return 0;
}
