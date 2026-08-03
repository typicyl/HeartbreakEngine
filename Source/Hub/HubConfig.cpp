// Hub/HubConfig.cpp
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "Hub/HubConfig.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace hbe::hub {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
fs::path LocalAppData() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? fs::path(buf) : fs::temp_directory_path();
}
constexpr const char* kStampName = "engine_version.txt";
} // namespace

fs::path HubConfigFile() { return LocalAppData() / "HeartbreakEngine" / "hub.json"; }

fs::path DefaultInstallRoot() { return LocalAppData() / "HeartbreakEngine" / "Engine"; }

HubConfig LoadHubConfig() {
    HubConfig c;
    std::ifstream in(HubConfigFile());
    if (!in) return c;
    const json j = json::parse(in, nullptr, /*allow_exceptions*/ false);
    if (!j.is_object()) return c;
    const std::string root = j.value("installRoot", std::string());
    if (!root.empty()) c.installRoot = fs::path(root);
    const std::string url = j.value("manifestUrl", std::string());
    // Only accept a stored URL that still passes the https policy. A config file is
    // user-writable, so a hand-edited http:// URL must not silently downgrade the one
    // channel that delivers executable code.
    if (!url.empty() && UrlIsSafe(url)) c.manifestUrl = url;
    // The version is re-read from the INSTALL, not trusted from the config: the two can
    // disagree if someone replaced the directory by hand, and the directory is the
    // truth. The config's copy is only a cache for display before the read.
    if (!c.installRoot.empty()) c.installedVersion = ReadInstalledVersion(c.installRoot);
    return c;
}

bool SaveHubConfig(const HubConfig& c) {
    const fs::path f = HubConfigFile();
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    json j;
    j["installRoot"] = c.installRoot.string();
    j["manifestUrl"] = c.manifestUrl;
    if (c.installedVersion) j["installedVersion"] = c.installedVersion->ToString();
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

std::optional<Version> ReadInstalledVersion(const fs::path& installRoot) {
    if (installRoot.empty()) return std::nullopt;
    std::ifstream in(installRoot / kStampName);
    if (!in) return std::nullopt;
    std::string line;
    std::getline(in, line);
    // Trim - a stamp written by a shell redirect or edited by hand picks up whitespace,
    // and ParseVersion deliberately refuses trailing junk.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
        line.pop_back();
    return ParseVersion(line);
}

bool WriteInstalledVersion(const fs::path& installRoot, const Version& v) {
    std::error_code ec;
    fs::create_directories(installRoot, ec);
    const fs::path f = installRoot / kStampName;
    const fs::path tmp = f.string() + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) return false;
        o << v.ToString() << "\n";
        if (!o.good()) return false;
    }
    fs::rename(tmp, f, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool LooksInstalled(const fs::path& installRoot) {
    if (installRoot.empty()) return false;
    std::error_code ec;
    // The EDITOR specifically, not just "the directory exists": a half-deleted install
    // or an empty folder the user picked must read as not-installed so the Hub offers to
    // install rather than to update something that is not there.
    return fs::exists(installRoot / "bin" / "HeartbreakEditor.exe", ec);
}

bool HubConfigSelfTest() {
    int fails = 0;
    const auto check = [&fails](bool c, const char* what) {
        if (c) return;
        ++fails;
        std::printf("hubconfig FAIL: %s\n", what);
    };

    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "hbe_hubcfg_test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // An empty directory is NOT an install. Getting this wrong makes the Hub offer
    // "update" against nothing and then fail confusingly.
    check(!LooksInstalled(root), "an empty directory must not read as installed");
    check(!ReadInstalledVersion(root).has_value(), "no stamp means no version");

    check(WriteInstalledVersion(root, Version{1, 2, 3}), "the stamp should write");
    const auto got = ReadInstalledVersion(root);
    check(got.has_value() && *got == Version{1, 2, 3}, "the stamp did not round-trip");

    // A stamp alone is not an install either - the binaries have to be there.
    check(!LooksInstalled(root), "a stamp without binaries must not read as installed");
    fs::create_directories(root / "bin", ec);
    { std::ofstream o(root / "bin" / "HeartbreakEditor.exe"); o << "x"; }
    check(LooksInstalled(root), "an install with the editor present must be detected");

    // A corrupt stamp must be UNKNOWN, never a guess. Guessing a version is how a Hub
    // decides you are up to date and refuses to repair a broken install.
    { std::ofstream o(root / "engine_version.txt", std::ios::trunc); o << "garbage"; }
    check(!ReadInstalledVersion(root).has_value(), "a corrupt stamp must read as unknown");
    // ...and trailing whitespace must NOT make a good stamp unreadable.
    { std::ofstream o(root / "engine_version.txt", std::ios::trunc); o << "2.0.1  \r\n"; }
    const auto trimmed = ReadInstalledVersion(root);
    check(trimmed.has_value() && *trimmed == Version{2, 0, 1},
          "a stamp with trailing whitespace must still parse");

    check(!DefaultInstallRoot().empty(), "there must be a default install location");
    check(!HubConfigFile().empty(), "the config path must resolve");

    fs::remove_all(root, ec);
    return fails == 0;
}

} // namespace hbe::hub
