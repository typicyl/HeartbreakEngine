// Hub/UpdateCheck.h - "is there a newer engine, and where is it?"
//
// The decision half of the updater, kept pure so --test-hub can prove it without a
// network: version ordering, manifest parsing, and the URL policy are all functions of
// their inputs. The transport lives in HttpClient and the install lives in Updater.
//
// SECURITY POSTURE, stated once and honestly. This mechanism downloads code and runs
// it. The manifest format carries no signature, so the ONLY thing standing between a
// developer and arbitrary code execution is TLS: HTTPS with certificate validation,
// against a host the user configured. That is defensible for a small team pulling from
// a domain they own, and it is NOT defensible over plain HTTP or with cert checks off -
// so both are refused here rather than being options someone can switch.
//
// `sha256` is OPTIONAL in the manifest and ENFORCED when present. That is deliberate:
// it costs nothing today, and the day the author starts publishing hashes, every
// already-shipped client begins verifying them with no update of its own.
#pragma once

#include "Core/Types.h"

#include <optional>
#include <string>

namespace hbe::hub {

// A dotted release version: MAJOR.MINOR.PATCH.
//
// Compared FIELD BY FIELD as integers, never as a string. "1.0.10" sorts BELOW "1.0.9"
// lexicographically, so a string compare silently stops offering updates at the tenth
// patch - a bug that only appears months after shipping.
struct Version {
    u32 major = 0, minor = 0, patch = 0;

    bool operator==(const Version& o) const {
        return major == o.major && minor == o.minor && patch == o.patch;
    }
    bool operator!=(const Version& o) const { return !(*this == o); }
    bool operator<(const Version& o) const {
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return patch < o.patch;
    }
    bool operator>(const Version& o) const { return o < *this; }
    std::string ToString() const;
};

// Parses "1.0.0". Accepts a leading 'v' and 1-3 components ("1" -> 1.0.0). Returns
// nullopt for anything else - including an EMPTY string and trailing junk, because a
// manifest that fails to parse must read as "no update available" rather than as
// version 0.0.0, which would compare BELOW everything and quietly disable updates.
std::optional<Version> ParseVersion(const std::string& s);

// What the server published.
struct UpdateManifest {
    Version latest;
    std::string releaseUrl;
    std::string sha256;   // optional; enforced when non-empty
    std::string notes;    // optional, shown to the user
    u64 sizeBytes = 0;    // optional; 0 = unknown
};

// Parses the manifest JSON. Returns nullopt on malformed input, a bad version, or a
// release URL that fails UrlIsSafe below - a manifest we cannot fully trust must not
// produce a half-populated struct a caller might act on.
std::optional<UpdateManifest> ParseManifest(const std::string& json);

// HTTPS-ONLY, and no credentials or ports smuggled in the authority.
//
// Rejects: anything not starting with "https://" (an updater over plain HTTP is a
// remote-code-execution primitive for anyone on the path), an embedded "user:pass@"
// (which makes the real host hard to read and is a classic phishing shape), an empty
// host, and control characters. Called on the CONFIGURED manifest URL, on the release
// URL inside the manifest, AND on every redirect target - a check that only runs on the
// first URL is not a check.
bool UrlIsSafe(const std::string& url);

// Appends a unique query parameter so a cache cannot serve a stale copy.
//
// BOTH the manifest and the release archive need this, and the archive needs it MORE.
// `release_url` is a stable name ("latest.zip") whose CONTENTS change every release -
// the exact shape a CDN, a corporate proxy or WinHTTP's own cache will happily answer
// from disk. Without a buster the sequence that bites is: manifest says 1.0.1, the zip
// URL is unchanged, the cache serves yesterday's 1.0.0 bytes, and the installer
// cheerfully "updates" you to the build you already had - with a version number that
// now lies.
//
// The value is milliseconds since the epoch PLUS a per-process counter, because two
// requests in the same millisecond would otherwise collide and share a cache entry.
// Appended with '?' or '&' depending on whether the URL already has a query.
//
// This is belt-and-braces with the no-cache request headers the fetcher sends: headers
// are the correct mechanism and a well-behaved cache honours them, but intermediaries
// that ignore them are common enough that the query parameter is what actually works.
std::string WithCacheBuster(const std::string& url);

// True when `remote` should be offered to a machine running `local`.
// STRICTLY GREATER: equal is not an update, and OLDER is never offered, so a rolled-back
// or mis-tagged manifest cannot silently downgrade a developer's engine.
bool ShouldUpdate(const Version& local, const Version& remote);

// The version this build reports as its own.
Version CurrentEngineVersion();

bool UpdateCheckSelfTest(); // part of --test-hub

} // namespace hbe::hub
