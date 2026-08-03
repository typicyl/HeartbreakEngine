// Hub/HubConfig.h - what the Hub remembers between runs.
//
// The Hub is now the INSTALLER, not just the updater, and that changes one thing
// fundamentally: it can no longer assume an engine exists, and it must not report its
// OWN compiled version as the engine's.
//
// CurrentEngineVersion() is a compile-time constant baked into whatever binary asks. In
// the old arrangement the Hub shipped inside the engine, so its constant and the
// engine's were the same number by construction. A standalone Hub is a separate
// download with its own release cadence - it can sit next to no engine at all, or an
// engine three versions behind it. Reporting its own constant would tell a user they
// have an engine they do not have, and then refuse to install because "you are up to
// date".
//
// So the INSTALLED version is state on disk, written when an install or update
// completes, and read back here.
#pragma once

#include "Hub/UpdateCheck.h"

#include <filesystem>
#include <optional>

namespace hbe::hub {

struct HubConfig {
    // Where the engine lives. Empty = nothing installed yet, which is a first-class
    // state rather than an error - it is what every new user starts in.
    std::filesystem::path installRoot;
    // What is actually installed there. nullopt when nothing is, or when the recorded
    // stamp is missing/corrupt - in which case the Hub must say "unknown" and offer a
    // repair, never guess a number.
    std::optional<Version> installedVersion;
    std::string manifestUrl = "https://hollowdreamstudios.com/enginemanifest.json";
};

// %LOCALAPPDATA%/HeartbreakEngine/hub.json - per user, NOT next to the executable. An
// install replaces the engine directory wholesale, so config stored there would be
// destroyed by the very operation that needs to record its result.
std::filesystem::path HubConfigFile();

HubConfig LoadHubConfig();
bool SaveHubConfig(const HubConfig& c);

// The default place to put a first install: %LOCALAPPDATA%/HeartbreakEngine/Engine.
// Deliberately NOT Program Files - writing there needs elevation, and an updater that
// demands admin every launch is one people disable.
std::filesystem::path DefaultInstallRoot();

// Reads the version stamp an install left behind. nullopt when the directory holds no
// engine, or the stamp is unreadable.
std::optional<Version> ReadInstalledVersion(const std::filesystem::path& installRoot);
// Writes it. Called ONLY after the swap succeeds, so a failed install never leaves a
// stamp claiming a version that is not there.
bool WriteInstalledVersion(const std::filesystem::path& installRoot, const Version& v);

// True when `installRoot` looks like a real engine install (has the editor executable).
// Used to distinguish "never installed" from "installed but the stamp is missing",
// which need different offers: install vs repair.
bool LooksInstalled(const std::filesystem::path& installRoot);

bool HubConfigSelfTest(); // part of --test-hub

} // namespace hbe::hub
