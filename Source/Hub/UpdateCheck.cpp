// Hub/UpdateCheck.cpp
#include "Hub/UpdateCheck.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace hbe::hub {

using json = nlohmann::json;

std::string Version::ToString() const {
    char b[48];
    std::snprintf(b, sizeof(b), "%u.%u.%u", major, minor, patch);
    return b;
}

std::optional<Version> ParseVersion(const std::string& in) {
    if (in.empty()) return std::nullopt;
    usize i = 0;
    if (in[i] == 'v' || in[i] == 'V') ++i;
    if (i >= in.size()) return std::nullopt;

    u32 part[3] = {0, 0, 0};
    int seen = 0;
    while (i < in.size() && seen < 3) {
        if (!std::isdigit(static_cast<unsigned char>(in[i]))) return std::nullopt;
        u64 v = 0;
        while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i]))) {
            v = v * 10 + static_cast<u64>(in[i] - '0');
            // Clamp rather than wrap. A wrapped component could compare BELOW the local
            // version and silently stop offering updates forever.
            if (v > 0xFFFFFFFFull) return std::nullopt;
            ++i;
        }
        part[seen++] = static_cast<u32>(v);
        if (i < in.size() && in[i] == '.') {
            ++i;
            if (i >= in.size()) return std::nullopt; // trailing dot: "1.0."
            continue;
        }
        break;
    }
    // TRAILING JUNK IS A PARSE FAILURE, not something to ignore. "1.0.0-beta" and
    // "1.0.0 " must not silently read as 1.0.0 - if the publisher starts tagging
    // pre-releases, the client should say "I do not understand this manifest" rather
    // than offer a build it cannot name correctly.
    if (i != in.size()) return std::nullopt;

    Version out;
    out.major = part[0];
    out.minor = part[1];
    out.patch = part[2];
    return out;
}

bool UrlIsSafe(const std::string& url) {
    static const std::string kScheme = "https://";
    if (url.size() <= kScheme.size()) return false;
    // Case-insensitive scheme compare: "HTTPS://" is legal in a URL.
    for (usize i = 0; i < kScheme.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(url[i])) != kScheme[i]) return false;
    if (url.size() > 2048) return false; // absurd length: refuse rather than parse

    const std::string rest = url.substr(kScheme.size());
    const usize slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.empty()) return false;
    // Embedded credentials make the real host hard to read - the classic
    // "https://trusted.com@evil.com/x" shape. Refuse outright.
    if (authority.find('@') != std::string::npos) return false;

    for (const char c : url) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return false; // control chars / CRLF injection
        if (c == ' ') return false;
    }
    return true;
}

std::optional<UpdateManifest> ParseManifest(const std::string& text) {
    const json j = json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (!j.is_object()) return std::nullopt;

    UpdateManifest m;
    const std::string ver = j.value("latest_version", std::string());
    const std::optional<Version> parsed = ParseVersion(ver);
    if (!parsed) return std::nullopt;
    m.latest = *parsed;

    m.releaseUrl = j.value("release_url", std::string());
    // The release URL is checked HERE, at parse time, so no caller can ever hold a
    // manifest containing a URL that would not be allowed to download.
    if (!UrlIsSafe(m.releaseUrl)) return std::nullopt;

    // Optional, forward-compatible fields. Absent is normal today.
    m.sha256 = j.value("sha256", std::string());
    m.notes = j.value("notes", std::string());
    if (const auto it = j.find("size_bytes"); it != j.end() && it->is_number_unsigned())
        m.sizeBytes = it->get<u64>();

    // A malformed hash is worse than none: it would fail every download and look like a
    // network fault. Accept only a full lowercase-able 64-char hex digest.
    if (!m.sha256.empty()) {
        if (m.sha256.size() != 64) return std::nullopt;
        for (char& c : m.sha256) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!std::isxdigit(u)) return std::nullopt;
            c = static_cast<char>(std::tolower(u));
        }
    }
    return m;
}

std::string WithCacheBuster(const std::string& url) {
    if (url.empty()) return url;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    // A per-process counter on top of the clock: a check and its follow-up download can
    // land inside the same millisecond, and two requests sharing a buster value share a
    // cache entry - which is the whole thing this exists to prevent.
    static std::atomic<u32> seq{0};
    const u32 n = seq.fetch_add(1, std::memory_order_relaxed);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s_hb=%lld-%u",
                  url.find('?') == std::string::npos ? "?" : "&",
                  static_cast<long long>(ms), n);
    return url + buf;
}

bool ShouldUpdate(const Version& local, const Version& remote) { return local < remote; }

Version CurrentEngineVersion() {
    // Single source of truth for what this build calls itself. Deliberately a literal
    // rather than __DATE__: a compile timestamp does not ORDER, so it cannot answer
    // "is the remote newer?" - which is the only question an updater asks.
    Version v;
    v.major = HBE_VERSION_MAJOR;
    v.minor = HBE_VERSION_MINOR;
    v.patch = HBE_VERSION_PATCH;
    return v;
}

// --- self-test ---------------------------------------------------------------

namespace {
int g_fails = 0;
void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_fails;
    std::printf("hub FAIL: %s\n", what);
}
} // namespace

bool UpdateCheckSelfTest() {
    g_fails = 0;

    // --- version parsing ---
    Check(ParseVersion("1.0.0") == Version{1, 0, 0}, "1.0.0 did not parse");
    Check(ParseVersion("v2.3.4") == Version{2, 3, 4}, "a leading v should be accepted");
    Check(ParseVersion("1") == Version{1, 0, 0}, "a bare major should default minor/patch");
    Check(ParseVersion("1.2") == Version{1, 2, 0}, "major.minor should default patch");
    Check(!ParseVersion("").has_value(), "an empty version must NOT parse");
    Check(!ParseVersion("1.0.").has_value(), "a trailing dot must not parse");
    Check(!ParseVersion("1.0.0-beta").has_value(), "trailing junk must not parse");
    Check(!ParseVersion("abc").has_value(), "a non-numeric version must not parse");
    Check(!ParseVersion("99999999999999999999").has_value(), "an overflowing part must not parse");

    // THE ORDERING BUG THIS EXISTS TO PREVENT. A string compare puts "1.0.10" below
    // "1.0.9" and the tenth patch release silently stops being offered.
    Check(Version{1, 0, 9} < Version{1, 0, 10}, "1.0.9 must sort BELOW 1.0.10");
    Check(Version{1, 9, 0} < Version{1, 10, 0}, "1.9.0 must sort BELOW 1.10.0");
    Check(Version{2, 0, 0} > Version{1, 99, 99}, "a major bump must dominate");

    // --- update policy ---
    Check(ShouldUpdate({1, 0, 0}, {1, 0, 1}), "a newer patch should be offered");
    Check(!ShouldUpdate({1, 0, 0}, {1, 0, 0}), "an EQUAL version must not be an update");
    Check(!ShouldUpdate({1, 2, 0}, {1, 1, 0}),
          "an OLDER remote must never be offered - that is a silent downgrade");

    // --- URL policy ---
    Check(UrlIsSafe("https://hollowdreamstudios.com/enginemanifest.json"),
          "a plain https URL should be allowed");
    Check(UrlIsSafe("HTTPS://Example.com/a.zip"), "the scheme compare should be case-insensitive");
    Check(!UrlIsSafe("http://hollowdreamstudios.com/latest.zip"),
          "PLAIN HTTP MUST BE REFUSED - it is a remote-code-execution primitive here");
    Check(!UrlIsSafe("ftp://example.com/a.zip"), "a non-https scheme must be refused");
    Check(!UrlIsSafe("https://trusted.com@evil.com/x.zip"),
          "an embedded credential/@ authority must be refused");
    Check(!UrlIsSafe("https://"), "a scheme with no host must be refused");
    Check(!UrlIsSafe(""), "an empty URL must be refused");
    Check(!UrlIsSafe("https://example.com/a\r\nHost: evil"), "CRLF injection must be refused");

    // --- cache busting ---
    //
    // The failure this prevents is specific and nasty: `release_url` is a STABLE name
    // whose contents change every release, so a cached copy means the manifest says
    // 1.0.1, the download hands back 1.0.0 bytes, and the installer reports success.
    {
        const std::string base = "https://hollowdreamstudios.com/enginemanifest.json";
        const std::string a = WithCacheBuster(base);
        const std::string b = WithCacheBuster(base);
        Check(a != base, "the cache buster must actually change the URL");
        Check(a != b, "two busted URLs must DIFFER, or they share a cache entry");
        Check(a.find("?_hb=") != std::string::npos,
              "a URL with no query should gain one with '?'");
        Check(UrlIsSafe(a), "a busted URL must still pass the https/safety policy");
        // A URL that already has a query must gain '&', not a second '?': the latter
        // makes the whole thing a malformed URL that some servers 400 on.
        const std::string q = WithCacheBuster("https://example.com/m.json?ch=beta");
        Check(q.find("&_hb=") != std::string::npos,
              "a URL with an existing query must be extended with '&'");
        Check(q.find("?ch=beta") != std::string::npos,
              "the existing query must be preserved");
        Check(UrlIsSafe(q), "a busted URL with an existing query must stay safe");
        Check(WithCacheBuster("").empty(), "busting an empty URL must not invent one");
        // Uniqueness has to survive a burst: a check and its download can land in the
        // same millisecond, and the clock alone would give them the same value.
        std::string prev;
        bool allDistinct = true;
        for (int i = 0; i < 64; ++i) {
            const std::string u = WithCacheBuster(base);
            if (u == prev) allDistinct = false;
            prev = u;
        }
        Check(allDistinct, "rapid successive busts collided - the counter is not working");
    }

    // --- manifest parsing, against the REAL published shape ---
    {
        const std::string real =
            "{\n    \"latest_version\": \"1.0.0\",\n"
            "    \"release_url\": \"https://hollowdreamstudios.com/latest.zip\"\n}";
        const auto m = ParseManifest(real);
        Check(m.has_value(), "the published manifest shape must parse");
        if (m) {
            Check(m->latest == Version{1, 0, 0}, "the published version did not parse");
            Check(m->releaseUrl == "https://hollowdreamstudios.com/latest.zip",
                  "the published release URL did not survive");
            Check(m->sha256.empty(), "sha256 is optional and absent here");
        }
    }
    // A manifest whose release URL is plain HTTP must fail WHOLESALE, not yield a
    // struct with a URL the downloader would then refuse.
    Check(!ParseManifest("{\"latest_version\":\"2.0.0\","
                         "\"release_url\":\"http://evil.com/x.zip\"}")
               .has_value(),
          "a manifest with an http release URL must not parse at all");
    Check(!ParseManifest("{\"latest_version\":\"\",\"release_url\":\"https://a.com/x.zip\"}")
               .has_value(),
          "a manifest with an unparseable version must not parse");
    Check(!ParseManifest("not json").has_value(), "garbage must not parse");
    Check(!ParseManifest("[]").has_value(), "a non-object must not parse");
    Check(!ParseManifest("{\"latest_version\":\"1.0.0\"}").has_value(),
          "a manifest with no release URL must not parse");
    // A malformed hash must fail loudly rather than silently failing every download.
    Check(!ParseManifest("{\"latest_version\":\"1.0.0\",\"release_url\":"
                         "\"https://a.com/x.zip\",\"sha256\":\"abc\"}")
               .has_value(),
          "a short sha256 must be rejected at parse time");
    {
        const std::string withHash =
            "{\"latest_version\":\"1.0.1\",\"release_url\":\"https://a.com/x.zip\",\"sha256\":"
            "\"" + std::string(64, 'A') + "\"}";
        const auto m = ParseManifest(withHash);
        Check(m.has_value(), "a well-formed 64-hex sha256 must be accepted");
        if (m) Check(m->sha256 == std::string(64, 'a'), "the hash should be normalised to lower case");
    }

    return g_fails == 0;
}

} // namespace hbe::hub
