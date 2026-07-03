// Core/UserSettings.cpp
#include "Core/UserSettings.h"

#include <nlohmann/json.hpp>

#include <fstream>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace hbe {

std::filesystem::path UserSettings::ResolveDir(const std::string& gameName) {
    std::filesystem::path base(".");
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) base = buf;
#endif
    std::string safe = gameName.empty() ? std::string("HeartbreakGame") : gameName;
    for (char& c : safe)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            c = '_';
    return base / safe;
}

bool UserSettings::Save(const std::filesystem::path& dir) const {
    nlohmann::json j;
    j["masterVolume"] = masterVolume;
    j["graphicsPreset"] = graphicsPreset;
    j["brightness"] = brightness;
    j["captionsEnabled"] = captionsEnabled;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream f(dir / "usersettings.json", std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::string s = j.dump(2);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    return static_cast<bool>(f);
}

bool UserSettings::Load(const std::filesystem::path& dir) {
    std::ifstream f(dir / "usersettings.json", std::ios::binary);
    if (!f) return false;
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception&) {
        return false;
    }
    masterVolume = j.value("masterVolume", 1.0f);
    graphicsPreset = j.value("graphicsPreset", 0);
    brightness = j.value("brightness", 0.5f);
    captionsEnabled = j.value("captionsEnabled", false);
    return true;
}

} // namespace hbe
