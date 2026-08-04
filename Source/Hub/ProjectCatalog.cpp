// Hub/ProjectCatalog.cpp
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "Hub/ProjectCatalog.h"

#include "Core/Platform.h"

#include "Hub/HubConfig.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace hbe::hub {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr usize kMaxRecent = 24;
}

fs::path RecentProjectsFile() {
    // Per-user, not next to the executable: an update REPLACES the install directory,
    // and a project list stored there would be wiped by every update.
    return platform::UserDataDir() / "recent_projects.json";
}

std::string ReadProjectName(const fs::path& hbproj) {
    std::ifstream in(hbproj);
    if (in) {
        json j = json::parse(in, nullptr, /*allow_exceptions*/ false);
        if (j.is_object()) {
            const std::string n = j.value("name", std::string());
            if (!n.empty()) return n;
        }
    }
    // A corrupt or unreadable project must still be LISTABLE - the launcher is where
    // someone would go to find and fix it. Falling back to the stem keeps it visible.
    return hbproj.stem().string();
}

std::vector<ProjectEntry> LoadProjects() {
    std::vector<ProjectEntry> out;
    std::ifstream in(RecentProjectsFile());
    if (!in) return out;
    json j = json::parse(in, nullptr, false);
    if (!j.is_object()) return out;
    for (const auto& e : j.value("recent", json::array())) {
        if (!e.is_string()) continue;
        ProjectEntry p;
        p.file = fs::path(e.get<std::string>());
        p.root = p.file.parent_path();
        std::error_code ec;
        p.missing = !fs::exists(p.file, ec);
        // A missing project is SHOWN, greyed, not silently dropped. Dropping it means a
        // moved drive letter makes projects vanish with no explanation.
        p.name = p.missing ? p.file.stem().string() : ReadProjectName(p.file);
        out.push_back(std::move(p));
        if (out.size() >= kMaxRecent) break;
    }
    return out;
}

bool SaveProjects(const std::vector<ProjectEntry>& list) {
    const fs::path f = RecentProjectsFile();
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    json arr = json::array();
    usize n = 0;
    for (const ProjectEntry& p : list) {
        arr.push_back(p.file.string());
        if (++n >= kMaxRecent) break;
    }
    json j;
    j["recent"] = std::move(arr);
    // Temp-then-rename, same reason every other writer in this engine does it: a crash
    // mid-write would otherwise leave a truncated list, and the user's projects would
    // appear to vanish.
    const fs::path tmp = f.string() + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) return false;
        o << j.dump(2);
        if (!o.good()) return false;
    }
    fs::rename(tmp, f, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void TouchProject(std::vector<ProjectEntry>& list, const fs::path& file) {
    RemoveProject(list, file);
    ProjectEntry p;
    p.file = file;
    p.root = file.parent_path();
    std::error_code ec;
    p.missing = !fs::exists(file, ec);
    p.name = p.missing ? file.stem().string() : ReadProjectName(file);
    list.insert(list.begin(), std::move(p));
    if (list.size() > kMaxRecent) list.resize(kMaxRecent);
}

void RemoveProject(std::vector<ProjectEntry>& list, const fs::path& file) {
    std::error_code ec;
    for (usize i = 0; i < list.size();) {
        // Compare canonically where possible: the same project reached through a
        // different spelling ("V:/x" vs "V:\\x") would otherwise appear twice.
        const bool same = list[i].file == file ||
                          fs::equivalent(list[i].file, file, ec);
        if (same && !ec) list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
        ec.clear();
    }
}

fs::path ResolveEditorExe(std::string& outError) {
    outError.clear();
    std::error_code ec;

    // THE RECORDED INSTALL ROOT COMES FIRST. The Hub already remembers where it put the
    // engine - LoadHubConfig().installRoot, written to hub.json the moment it is chosen -
    // and the updater and LooksInstalled both work in terms of <installRoot>/bin. This
    // function did not: it looked only NEXT TO THE HUB. The Hub is REQUIRED to live
    // outside the tree it swaps (that is what makes renaming bin/ legal), so in a real
    // install "next to the Hub" is precisely where the editor is not. It only ever worked
    // in the dev build, where both happen to land in the same output directory - which is
    // exactly the configuration that hides the bug.
    const fs::path root = LoadHubConfig().installRoot;
    if (!root.empty()) {
        const fs::path installed = root / "bin" / "HeartbreakEditor.exe";
        if (fs::exists(installed, ec)) return installed;
    }

    // Fallback: beside the Hub. Keeps the development layout working, where the Hub and
    // the editor are built into one directory.
    const fs::path adjacent = platform::ExecutableDir() / L"HeartbreakEditor.exe";
    if (fs::exists(adjacent, ec)) return adjacent;

    // Name BOTH places that were tried. "Not found" without saying where is unanswerable,
    // and the two locations mean two different problems: a broken install versus a Hub
    // pointed at the wrong folder.
    outError = "HeartbreakEditor.exe was not found. Looked in " +
               (root.empty() ? std::string("<no engine folder set>")
                             : (root / "bin").string()) +
               " and beside the Hub (" + adjacent.parent_path().string() +
               "). Install the engine, or set the engine folder in the Hub.";
    return {};
}

bool LaunchEditor(const fs::path& hbproj, std::string& outError) {
    const fs::path editor = ResolveEditorExe(outError);
    if (editor.empty()) return false;
    std::wstring cmd = L"\"" + editor.wstring() + L"\" --project \"" + hbproj.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          nullptr, &si, &pi)) {
        outError = "Could not start the editor process.";
        return false;
    }
    // DETACHED: the Hub does not wait, and closing the Hub must not kill the editor.
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

bool ProjectCatalogSelfTest() {
    int fails = 0;
    const auto check = [&fails](bool c, const char* what) {
        if (c) return;
        ++fails;
        std::printf("projectcatalog FAIL: %s\n", what);
    };

    std::vector<ProjectEntry> list;
    TouchProject(list, "C:/a/One.hbproj");
    TouchProject(list, "C:/b/Two.hbproj");
    check(list.size() == 2, "two projects should be listed");
    check(list[0].file == fs::path("C:/b/Two.hbproj"), "the newest must be first");

    // Re-touching must MOVE, not duplicate - otherwise the list fills with one project.
    TouchProject(list, "C:/a/One.hbproj");
    check(list.size() == 2, "re-touching must not duplicate an entry");
    check(list[0].file == fs::path("C:/a/One.hbproj"), "a re-touched project moves to front");

    RemoveProject(list, "C:/a/One.hbproj");
    check(list.size() == 1, "remove should drop exactly one");
    check(list[0].file == fs::path("C:/b/Two.hbproj"), "the wrong entry was removed");

    // A nonexistent project is FLAGGED, not dropped.
    check(list[0].missing, "a project that is not on disk must be flagged missing");
    check(!list[0].name.empty(), "a missing project must still have a display name");

    // The cap must hold, or a long-running install grows the file forever.
    for (int i = 0; i < 100; ++i)
        TouchProject(list, fs::path("C:/p/") / (std::to_string(i) + ".hbproj"));
    check(list.size() <= 24, "the recent list must be capped");

    // Launch POLICY, without launching. Calling LaunchEditor here would spawn a real
    // editor process - the exe sits right next to this one - which is a side effect no
    // self-test may have. It did exactly that once and hung the suite in a GUI window.
    {
        std::string err;
        const fs::path resolved = ResolveEditorExe(err);
        // Either it resolved (engine installed) or it explained itself. Never both
        // empty: a silent failure is the thing this function exists to prevent.
        check(!resolved.empty() || !err.empty(),
              "resolving the editor must either succeed or say why not");
        if (resolved.empty())
            check(err.find("HeartbreakEditor.exe") != std::string::npos,
                  "the error must name the missing file");
        else
            check(resolved.filename() == "HeartbreakEditor.exe",
                  "resolution must point at the editor executable");
    }
    check(!RecentProjectsFile().empty(), "the recents path must resolve");
    return fails == 0;
}

} // namespace hbe::hub
