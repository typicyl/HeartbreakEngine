// Hub/Updater.cpp
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include "Hub/Updater.h"
#include "Core/Platform.h"

#include "Hub/HubSelfUpdate.h"

#include "Hub/HubConfig.h"
#include "Hub/ZipArchive.h"

#include <cstdio>
#include <fstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace hbe::hub {

namespace fs = std::filesystem;

const char* UpdateStateName(UpdateState s) {
    switch (s) {
        case UpdateState::Idle: return "Idle";
        case UpdateState::Checking: return "Checking";
        case UpdateState::UpToDate: return "UpToDate";
        case UpdateState::Available: return "Available";
        case UpdateState::Downloading: return "Downloading";
        case UpdateState::Verifying: return "Verifying";
        case UpdateState::Installing: return "Installing";
        case UpdateState::Done: return "Done";
        case UpdateState::Failed: return "Failed";
    }
    return "?";
}

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct WinHttpSession {
    HINTERNET session = nullptr, conn = nullptr, req = nullptr;
    ~WinHttpSession() {
        if (req) ::WinHttpCloseHandle(req);
        if (conn) ::WinHttpCloseHandle(conn);
        if (session) ::WinHttpCloseHandle(session);
    }
};

// Opens an HTTPS GET. Refuses non-https before touching the network, and leaves every
// certificate check ON - WinHTTP validates by default and nothing here weakens it.
bool OpenGet(const std::string& url, WinHttpSession& s, u64& contentLength,
             std::string& outError) {
    if (!UrlIsSafe(url)) {
        outError = "refused a non-https or malformed URL: " + url;
        return false;
    }
    const std::wstring wurl = Widen(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        outError = "could not parse the URL";
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        outError = "refused: not HTTPS";
        return false;
    }

    s.session = ::WinHttpOpen(L"HeartbreakHub/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s.session) {
        outError = "WinHttpOpen failed";
        return false;
    }
    // Bounded timeouts: without them a black-holed connection hangs the Hub forever with
    // no way for the user to tell it apart from a slow download.
    ::WinHttpSetTimeouts(s.session, 15000, 15000, 30000, 30000);

    s.conn = ::WinHttpConnect(s.session, host, uc.nPort, 0);
    if (!s.conn) {
        outError = "WinHttpConnect failed";
        return false;
    }
    // WINHTTP_FLAG_REFRESH forces WinHTTP past its OWN cache. Without it, a check run
    // twice in a session can be answered from disk and never touch the network - so a
    // release published a minute ago stays invisible until the process restarts.
    s.req = ::WinHttpOpenRequest(s.conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
    if (!s.req) {
        outError = "WinHttpOpenRequest failed";
        return false;
    }
    // REFUSE A REDIRECT THAT LEAVES HTTPS. WinHTTP follows redirects itself, and its
    // default policy would happily follow https -> http, which silently undoes the only
    // integrity guarantee this design has.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    ::WinHttpSetOption(s.req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    // Ask every cache in the path not to answer from a stored copy. These are the
    // CORRECT mechanism; the `_hb=` query parameter the caller appends is the one that
    // works on intermediaries which ignore them (and on a CDN keyed purely by URL).
    // Pragma is the HTTP/1.0 spelling and still respected by older proxies.
    static const wchar_t* kNoCache =
        L"Cache-Control: no-cache, no-store, max-age=0\r\nPragma: no-cache\r\n";
    if (!::WinHttpSendRequest(s.req, kNoCache, static_cast<DWORD>(-1),
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(s.req, nullptr)) {
        outError = "the request failed (no network, DNS, or TLS rejected the certificate)";
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    ::WinHttpQueryHeaders(s.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        outError = "server returned HTTP " + std::to_string(status);
        return false;
    }
    DWORD cl = 0;
    len = sizeof(cl);
    contentLength =
        ::WinHttpQueryHeaders(s.req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &cl, &len, WINHTTP_NO_HEADER_INDEX)
            ? cl
            : 0;
    return true;
}

} // namespace

bool HttpGetString(const std::string& url, std::string& out, usize maxBytes,
                   std::string& outError) {
    WinHttpSession s;
    u64 total = 0;
    if (!OpenGet(url, s, total, outError)) return false;
    out.clear();
    std::vector<char> buf(16384);
    for (;;) {
        DWORD got = 0;
        if (!::WinHttpReadData(s.req, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            outError = "read failed";
            return false;
        }
        if (got == 0) break;
        if (out.size() + got > maxBytes) {
            // Bound the RESPONSE, not just the declared Content-Length: a server can lie
            // about the length or omit it entirely.
            outError = "response exceeded the size cap";
            return false;
        }
        out.append(buf.data(), got);
    }
    return true;
}

bool HttpDownloadFile(const std::string& url, const fs::path& dest,
                      const std::function<bool(u64, u64)>& onProgress, u64 maxBytes,
                      std::string& outError) {
    WinHttpSession s;
    u64 total = 0;
    if (!OpenGet(url, s, total, outError)) return false;
    if (total > maxBytes) {
        outError = "the download declares a size above the cap";
        return false;
    }

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream of(dest, std::ios::binary | std::ios::trunc);
    if (!of) {
        outError = "cannot open the download file";
        return false;
    }

    u64 done = 0;
    std::vector<char> buf(65536);
    for (;;) {
        DWORD got = 0;
        if (!::WinHttpReadData(s.req, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            outError = "read failed mid-download";
            of.close();
            fs::remove(dest, ec);
            return false;
        }
        if (got == 0) break;
        done += got;
        if (done > maxBytes) {
            outError = "the download exceeded the size cap";
            of.close();
            fs::remove(dest, ec); // never leave a partial file that looks complete
            return false;
        }
        of.write(buf.data(), got);
        if (!of.good()) {
            outError = "write failed (disk full?)";
            of.close();
            fs::remove(dest, ec);
            return false;
        }
        if (onProgress && !onProgress(done, total)) {
            outError = "cancelled";
            of.close();
            fs::remove(dest, ec);
            return false;
        }
    }
    of.close();
    // A server that declared a length and then delivered fewer bytes gave us a truncated
    // archive. Catching it here turns a confusing "corrupt zip" into a clear message.
    if (total != 0 && done != total) {
        outError = "the download was truncated";
        fs::remove(dest, ec);
        return false;
    }
    return true;
}

std::string Sha256File(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    DWORD objLen = 0, cb = 0;
    ::BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                        sizeof(objLen), &cb, 0);
    std::vector<u8> obj(objLen);
    BCRYPT_HASH_HANDLE h = nullptr;
    if (::BCryptCreateHash(alg, &h, obj.data(), objLen, nullptr, 0, 0) != 0) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::vector<char> buf(65536);
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
        ::BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()),
                         static_cast<ULONG>(in.gcount()), 0);
    }
    u8 digest[32] = {};
    ::BCryptFinishHash(h, digest, sizeof(digest), 0);
    ::BCryptDestroyHash(h);
    ::BCryptCloseAlgorithmProvider(alg, 0);

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const u8 b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

// --- Updater -----------------------------------------------------------------

namespace {
// Locates the directory holding HeartbreakEditor.exe inside a freshly extracted tree.
//
// Bounded depth: an archive is a handful of levels deep, and an unbounded walk over a
// hostile or merely huge zip is a denial of service. Returns empty when the archive
// contains no editor at all, which the caller reports rather than installing garbage.
std::filesystem::path FindPayloadRoot(const std::filesystem::path& staged) {
    std::error_code ec;
    if (std::filesystem::exists(staged / "HeartbreakEditor.exe", ec)) return staged;
    for (std::filesystem::recursive_directory_iterator it(staged, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec) &&
            it->path().filename() == "HeartbreakEditor.exe")
            return it->path().parent_path();
    }
    return {};
}
} // namespace


Updater::Updater(std::string manifestUrl, UpdatePaths paths)
    : manifestUrl_(std::move(manifestUrl)), paths_(std::move(paths)) {
    // Deliberately 0.0.0 until SetInstalledVersion is called. The Hub's own compile-time
    // version is NOT the installed engine's - they are separate downloads - and
    // defaulting to it would report an engine the user may not have.
    progress_.localVersion = Version{};
}

void Updater::SetInstalledVersion(std::optional<Version> v) {
    installed_ = v;
    progress_.localVersion = v.value_or(Version{});
}

void Updater::Fail(std::string why) {
    progress_.state = UpdateState::Failed;
    progress_.message = std::move(why);
}

void Updater::Check() {
    haveManifest_ = false;
    progress_.state = UpdateState::Checking;
    progress_.message = installed_ ? "Checking for updates..." : "Checking for the engine...";
    progress_.localVersion = installed_.value_or(Version{});

    if (!UrlIsSafe(manifestUrl_)) {
        Fail("The configured update URL is not a valid https:// address.");
        return;
    }
    std::string body, err;
    // 256 KiB is far above any legitimate manifest and far below a memory problem.
    // A UNIQUE URL PER CHECK. The manifest lives at one fixed address, so without this
    // a CDN or proxy can serve the copy it took an hour ago and the Hub reports "up to
    // date" against a release that already shipped.
    if (!HttpGetString(WithCacheBuster(manifestUrl_), body, 256 * 1024, err)) {
        Fail("Could not reach the update server: " + err);
        return;
    }
    const std::optional<UpdateManifest> m = ParseManifest(body);
    if (!m) {
        Fail("The update manifest could not be understood (bad version, missing or "
             "non-https release_url, or malformed sha256).");
        return;
    }
    manifest_ = *m;
    haveManifest_ = true;
    progress_.remoteVersion = m->latest;
    progress_.releaseUrl = m->releaseUrl;

    // NOTHING INSTALLED = always available. ShouldUpdate would reach the same answer
    // against 0.0.0, but saying it explicitly stops a future change to the comparison
    // from telling a user with no engine that they are "up to date" - the single most
    // confusing thing a first-run installer could do.
    if (!installed_) {
        progress_.state = UpdateState::Available;
        progress_.message = "Engine " + m->latest.ToString() + " is ready to install.";
        return;
    }
    if (!ShouldUpdate(progress_.localVersion, m->latest)) {
        progress_.state = UpdateState::UpToDate;
        progress_.message = "Up to date (" + progress_.localVersion.ToString() + ").";
        return;
    }
    progress_.state = UpdateState::Available;
    progress_.message = "Version " + m->latest.ToString() + " is available (you have " +
                        progress_.localVersion.ToString() + ").";
}

void Updater::Apply(const std::function<bool(const UpdateProgress&)>& confirm) {
    if (!haveManifest_ || progress_.state != UpdateState::Available) {
        Fail("No update is ready to install.");
        return;
    }
    // CONSENT FIRST, before a single byte is fetched. An updater that installs without
    // an explicit yes is indistinguishable from malware, however good its intentions.
    if (confirm && !confirm(progress_)) {
        progress_.state = UpdateState::Available;
        progress_.message = "Update cancelled.";
        return;
    }

    std::error_code ec;
    fs::remove_all(paths_.Staged(), ec); // never merge with a previous failed attempt
    fs::create_directories(paths_.Work(), ec);

    progress_.state = UpdateState::Downloading;
    progress_.message = "Downloading " + manifest_.latest.ToString() + "...";
    std::string err;
    constexpr u64 kMaxDownload = 2ull * 1024 * 1024 * 1024; // 2 GiB
    // AND the archive. This matters MORE than the manifest: `latest.zip` is a stable
    // name whose contents change every release, so a cached copy means the manifest
    // says 1.0.1, the download hands back yesterday's 1.0.0 bytes, and the installer
    // "updates" you to the build you already had while reporting the new version.
    if (!HttpDownloadFile(
            WithCacheBuster(manifest_.releaseUrl), paths_.Download(),
            [this](u64 done, u64 total) {
                progress_.bytesDone = done;
                progress_.bytesTotal = total;
                return true;
            },
            kMaxDownload, err)) {
        Fail("Download failed: " + err);
        return;
    }

    progress_.state = UpdateState::Verifying;
    if (!manifest_.sha256.empty()) {
        progress_.message = "Verifying...";
        const std::string got = Sha256File(paths_.Download());
        if (got.empty()) {
            Fail("Could not hash the download.");
            return;
        }
        if (got != manifest_.sha256) {
            // A hash mismatch is the one failure that must never be shrugged off: the
            // bytes are not what the publisher published.
            fs::remove(paths_.Download(), ec);
            Fail("INTEGRITY CHECK FAILED - the download does not match the published "
                 "hash. Nothing was installed.");
            return;
        }
    } else {
        // Say it plainly rather than implying a check happened.
        progress_.message = "No hash published; integrity rests on HTTPS alone.";
    }

    progress_.state = UpdateState::Installing;
    ZipArchive zip;
    if (!zip.Open(paths_.Download())) {
        Fail("The downloaded file is not a readable .zip archive.");
        return;
    }
    constexpr u64 kMaxExpand = 8ull * 1024 * 1024 * 1024; // 8 GiB expanded
    if (!zip.ExtractAll(paths_.Staged(), kMaxExpand, err)) {
        fs::remove_all(paths_.Staged(), ec); // discard a partial, possibly hostile unpack
        Fail("Extraction refused: " + err);
        return;
    }

    // THE SWAP. Directory renames on one volume, with the old tree kept.
    //
    // FIND THE PAYLOAD BY LOOKING FOR THE EDITOR, not by guessing a folder name. The
    // published archive wraps everything in "Release/", an earlier assumption was
    // "bin/", and a hand-made zip may have neither. Guessing produced
    // <install>/bin/Release/HeartbreakEditor.exe - a working copy that LooksInstalled
    // could not see, so the Hub kept reporting "not installed" after a successful
    // install. Searching for the executable makes every layout work and needs no
    // convention from whoever builds the zip.
    const fs::path stagedBin = FindPayloadRoot(paths_.Staged());
    if (stagedBin.empty()) {
        fs::remove_all(paths_.Staged(), ec);
        Fail("The downloaded archive does not contain HeartbreakEditor.exe, so it is not "
             "an engine build. Nothing was changed.");
        return;
    }
    // REFUSE TO SWAP OUT FROM UNDER OURSELVES. If this Hub is running from inside the
    // directory about to be renamed, the rename fails on Windows (a running image
    // cannot be moved) and we would land in the rollback path for a reason the user
    // cannot act on. Say it plainly instead.
    {
        const fs::path selfDir = platform::ExecutableDir().lexically_normal();
        const fs::path live = (paths_.installRoot / "bin").lexically_normal();
        const std::string rel = selfDir.lexically_relative(live).generic_string();
        if (!rel.empty() && rel.rfind("..", 0) != 0) {
            Fail("The Hub is running from inside the folder it needs to replace. Move "
                 "HeartbreakHub.exe outside the engine install and try again.");
            return;
        }
    }
    const fs::path liveBin = paths_.installRoot / "bin";
    const fs::path backup = paths_.BackupFor(progress_.localVersion);
    fs::remove_all(backup, ec);

    if (fs::exists(liveBin, ec)) {
        fs::rename(liveBin, backup, ec);
        if (ec) {
            Fail("Could not move the current install aside (is the editor still "
                 "running?). Nothing was changed.");
            return;
        }
    }
    fs::rename(stagedBin, liveBin, ec);
    if (ec) {
        // ROLL BACK IMMEDIATELY. Leaving the install with no bin/ is the one outcome
        // worse than not updating at all.
        std::error_code ec2;
        if (fs::exists(backup, ec2)) fs::rename(backup, liveBin, ec2);
        Fail("Could not put the new build in place; the previous install was restored.");
        return;
    }

    // THE HUB TRAVELS WITH THE ENGINE. The payload that just went live carries the
    // matching launcher, so take it from there instead of downloading a second file. This
    // runs AFTER the bin/ swap: if the engine install had failed we would have rolled back,
    // and staging a Hub for a build that is not installed would guarantee a mismatch.
    {
        std::string hubErr;
        if (StageSelfUpdateFromPayload(liveBin / "HeartbreakHub.exe", hubErr)) {
            progress_.hubUpdateStaged = true;
        } else if (!hubErr.empty()) {
            // Never fail the ENGINE update over the launcher - the engine is installed and
            // working at this point. Surface it through the progress note, which is the
            // Hub's own reporting channel: this target deliberately links no engine code,
            // so the engine's logger is not available here.
            progress_.hubUpdateNote = hubErr;
        }
    }

    fs::remove(paths_.Download(), ec);
    fs::remove_all(paths_.Staged(), ec);
    // STAMP IT - only now, after the swap actually succeeded, so a failed install never
    // leaves a version claiming to be there. Every later run reads this instead of
    // guessing; it is what lets the Hub tell "engine 1.0.1" from "the Hub happens to be
    // built as 1.0.1".
    const bool wasFresh = !installed_.has_value();
    WriteInstalledVersion(paths_.installRoot, manifest_.latest);
    installed_ = manifest_.latest;
    progress_.localVersion = manifest_.latest;
    progress_.state = UpdateState::Done;
    progress_.message = (wasFresh ? "Installed engine " : "Updated to ") +
                        manifest_.latest.ToString() + ". You can open a project now.";
}

void Updater::CleanWorkspace() {
    std::error_code ec;
    fs::remove_all(paths_.Staged(), ec);
    fs::remove(paths_.Download(), ec);
    // Backups are deliberately NOT removed here - the most recent one is the only way
    // back from a bad update.
}

bool Updater::Rollback(std::string& outError) {
    std::error_code ec;
    fs::path newest;
    fs::file_time_type best{};
    for (const auto& e : fs::directory_iterator(paths_.Work(), ec)) {
        if (!e.is_directory(ec)) continue;
        if (e.path().filename().string().rfind("backup_", 0) != 0) continue;
        const auto t = fs::last_write_time(e.path(), ec);
        if (newest.empty() || t > best) {
            newest = e.path();
            best = t;
        }
    }
    if (newest.empty()) {
        outError = "There is no previous install to roll back to.";
        return false;
    }
    const fs::path liveBin = paths_.installRoot / "bin";
    const fs::path aside = paths_.Work() / "rollback_discard";
    fs::remove_all(aside, ec);
    if (fs::exists(liveBin, ec)) {
        fs::rename(liveBin, aside, ec);
        if (ec) {
            outError = "Could not move the current install aside (still running?).";
            return false;
        }
    }
    fs::rename(newest, liveBin, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(aside, liveBin, ec2);
        outError = "Rollback failed; the current install was left in place.";
        return false;
    }
    fs::remove_all(aside, ec);
    return true;
}

// --- self-test ---------------------------------------------------------------

namespace {
int g_ufails = 0;
void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_ufails;
    std::printf("hub FAIL: %s\n", what);
}
} // namespace

bool UpdaterSelfTest() {
    g_ufails = 0;

    // Path layout: everything scratch must live under the install's own _update dir, so
    // a failed update never scatters files somewhere nobody thinks to clean.
    UpdatePaths p;
    p.installRoot = "C:/Engine";
    Check(p.Work() == fs::path("C:/Engine/_update"), "the work dir must be under the install");
    Check(p.Download().parent_path() == p.Work(), "the download must live in the work dir");
    Check(p.Staged().parent_path() == p.Work(), "staging must live in the work dir");
    Check(p.BackupFor(Version{1, 2, 3}).filename().string() == "backup_1.2.3",
          "a backup must be named for the version it holds");

    // An updater pointed at a non-https URL must refuse BEFORE any network access.
    {
        Updater u("http://example.com/manifest.json", p);
        u.Check();
        Check(u.Progress().state == UpdateState::Failed,
              "a plain-http manifest URL must fail the check outright");
    }
    {
        Updater u("", p);
        u.Check();
        Check(u.Progress().state == UpdateState::Failed, "an empty manifest URL must fail");
    }
    // Apply must refuse when no manifest has been fetched - the state machine cannot be
    // driven straight to "install" by a caller that skipped the check.
    {
        Updater u("https://example.com/m.json", p);
        bool asked = false;
        u.Apply([&](const UpdateProgress&) {
            asked = true;
            return true;
        });
        Check(u.Progress().state == UpdateState::Failed, "Apply without a manifest must fail");
        Check(!asked, "Apply must not even ASK for confirmation with no manifest");
    }

    // SHA-256, against the published test vector for the empty input. A hash function
    // that is subtly wrong would reject every legitimate download.
    {
        const fs::path f = fs::temp_directory_path() / "hbe_hub_empty.bin";
        { std::ofstream o(f, std::ios::binary); }
        const std::string h = Sha256File(f);
        Check(h == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "SHA-256 of an empty file does not match the known vector");
        std::error_code ec;
        fs::remove(f, ec);
    }
    {
        const fs::path f = fs::temp_directory_path() / "hbe_hub_abc.bin";
        { std::ofstream o(f, std::ios::binary); o << "abc"; }
        const std::string h = Sha256File(f);
        Check(h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256 of \"abc\" does not match the known vector");
        std::error_code ec;
        fs::remove(f, ec);
    }

    Check(std::string(UpdateStateName(UpdateState::Done)) == "Done", "state names must work");
    return g_ufails == 0;
}

} // namespace hbe::hub
