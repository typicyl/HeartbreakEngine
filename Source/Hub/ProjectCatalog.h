// Hub/ProjectCatalog.h - the Hub's view of "which projects exist on this machine".
//
// Deliberately NOT the editor's Project class. The Hub must be able to LIST and LAUNCH
// a project without opening it: opening one pulls in the asset system, the tag table,
// the scene serializer and eventually a GPU - and the Hub has to work when the engine
// it is managing is broken or not installed at all. So this reads the same
// recent-projects file the editor writes, and nothing else.
//
// It also never MUTATES a project. Merely appearing in a launcher list must not dirty
// someone's working tree - which the previous hub did, seeding a Sphere.uaf into any
// project you clicked.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hbe::hub {

struct ProjectEntry {
    std::filesystem::path file;  // the .hbproj
    std::string name;            // display name (from the file, else its stem)
    std::filesystem::path root;  // the directory containing it
    bool missing = false;        // listed but no longer on disk
};

// %LOCALAPPDATA%/HeartbreakEngine/recent_projects.json - the SAME file the editor
// writes, so opening a project in either place shows up in the other.
std::filesystem::path RecentProjectsFile();

// Reads the list. Entries whose file has vanished are returned with `missing` set
// rather than dropped, so the user can see and remove them instead of wondering where
// a project went.
std::vector<ProjectEntry> LoadProjects();

// Writes the list back (most recent first, de-duplicated, capped).
bool SaveProjects(const std::vector<ProjectEntry>& list);

// Moves `file` to the front, creating the entry if needed.
void TouchProject(std::vector<ProjectEntry>& list, const std::filesystem::path& file);
void RemoveProject(std::vector<ProjectEntry>& list, const std::filesystem::path& file);

// Reads just the "name" key out of a .hbproj. Returns the file stem when the project
// cannot be parsed - a corrupt project must still be LISTABLE so the user can find and
// fix it, rather than silently disappearing from the launcher.
std::string ReadProjectName(const std::filesystem::path& hbproj);

// Resolves the editor executable next to the Hub, WITHOUT running it. Split out from
// LaunchEditor so the launch policy is testable: a self-test that called LaunchEditor
// would SPAWN A REAL EDITOR (the exe is right there), which is a side effect no test
// may have - it did exactly that once and hung the suite in a GUI.
// Returns an empty path and fills outError when the engine is not installed.
std::filesystem::path ResolveEditorExe(std::string& outError);

// Spawns the editor on a project and returns immediately. False when the executable is
// missing next to the Hub - the "engine not installed / mid-update" case, which the Hub
// must report rather than appearing to do nothing.
bool LaunchEditor(const std::filesystem::path& hbproj, std::string& outError);

bool ProjectCatalogSelfTest(); // part of --test-hub

} // namespace hbe::hub
