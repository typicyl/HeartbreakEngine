// Hub/ZipArchive.cpp - minimal, defensive ZIP reader (stored + deflate).
#include "Hub/ZipArchive.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace hbe::hub {

namespace fs = std::filesystem;

namespace {

constexpr u32 kEocdSig = 0x06054b50;
constexpr u32 kCenSig = 0x02014b50;
constexpr u32 kLocSig = 0x04034b50;
// A release archive is tens of MiB. These bounds exist so a corrupt header cannot make
// us allocate or loop unboundedly before we have read a single byte of content.
constexpr u64 kMaxEntries = 200000;
constexpr u64 kMaxNameLen = 4096;

u16 Rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 Rd32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

bool IsReservedDeviceName(const std::string& stem) {
    static const char* kDevices[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
                                     "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                     "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                     "LPT7", "LPT8", "LPT9"};
    std::string up;
    up.reserve(stem.size());
    for (const char c : stem) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    for (const char* d : kDevices)
        if (up == d) return true;
    return false;
}

} // namespace

fs::path SafeJoin(const fs::path& destRoot, const std::string& entryName) {
    if (entryName.empty() || entryName.size() > kMaxNameLen) return {};
    // A NUL truncates the name for the OS but not for our checks - the classic
    // "safe.txt\0../../evil" bypass. Refuse outright.
    if (entryName.find('\0') != std::string::npos) return {};

    // Normalise separators first; a zip may legally store either, and checking only '/'
    // lets "..\\.." through on Windows.
    std::string norm = entryName;
    std::replace(norm.begin(), norm.end(), '\\', '/');

    // Absolute, drive-relative or UNC: never legal inside an archive we are unpacking
    // into our own directory.
    if (norm[0] == '/') return {};
    if (norm.size() >= 2 && norm[1] == ':') return {};
    if (norm.size() >= 2 && norm[0] == '/' && norm[1] == '/') return {};

    // Reject "..", including as a whole component only - "..foo" is a legal name.
    usize start = 0;
    while (start <= norm.size()) {
        const usize slash = norm.find('/', start);
        const std::string comp =
            norm.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (comp == "..") return {};
        // A trailing dot or space is stripped by Windows on create, which lets
        // "foo.exe " and "foo.exe." collide with an existing file. Refuse both.
        if (!comp.empty() && (comp.back() == ' ' || comp.back() == '.') && comp != ".")
            return {};
        if (!comp.empty()) {
            const usize dot = comp.find('.');
            if (IsReservedDeviceName(dot == std::string::npos ? comp : comp.substr(0, dot)))
                return {};
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    std::error_code ec;
    const fs::path rootAbs = fs::weakly_canonical(destRoot, ec);
    const fs::path root = ec ? destRoot.lexically_normal() : rootAbs;
    const fs::path joined = (root / fs::path(norm)).lexically_normal();

    // THE ACTUAL CONTAINMENT CHECK, on normalised paths. Everything above is defence in
    // depth; this is the one that must be right. lexically_relative gives "..*" when the
    // target is outside, which is the only reliable way to ask "is A under B?".
    const fs::path rel = joined.lexically_relative(root);
    if (rel.empty()) return {};
    const std::string relStr = rel.generic_string();
    if (relStr == ".." || relStr.rfind("../", 0) == 0) return {};
    return joined;
}

bool ZipArchive::Open(const fs::path& file) {
    entries_.clear();
    file_ = file;
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 22) return false; // smaller than an empty EOCD record

    // Find the End Of Central Directory by scanning backwards over the maximum comment
    // length. Bounded so a hostile file cannot make us scan the whole disk image.
    const std::streamoff tail = std::min<std::streamoff>(size, 66000);
    std::vector<u8> buf(static_cast<usize>(tail));
    in.seekg(size - tail);
    in.read(reinterpret_cast<char*>(buf.data()), tail);
    if (!in) return false;

    std::streamoff eocd = -1;
    for (std::streamoff i = tail - 22; i >= 0; --i) {
        if (Rd32(buf.data() + i) == kEocdSig) { eocd = i; break; }
    }
    if (eocd < 0) return false;

    const u8* e = buf.data() + eocd;
    const u64 count = Rd16(e + 10);
    const u64 cdSize = Rd32(e + 12);
    const u64 cdOff = Rd32(e + 16);
    if (count > kMaxEntries) return false;
    if (cdOff + cdSize > static_cast<u64>(size)) return false;

    std::vector<u8> cd(static_cast<usize>(cdSize));
    in.seekg(static_cast<std::streamoff>(cdOff));
    in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cdSize));
    if (!in) return false;

    usize p = 0;
    for (u64 i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || Rd32(cd.data() + p) != kCenSig) return false;
        ZipEntry z;
        z.method = Rd16(cd.data() + p + 10);
        z.crc32 = Rd32(cd.data() + p + 16);
        z.compressedSize = Rd32(cd.data() + p + 20);
        z.uncompressedSize = Rd32(cd.data() + p + 24);
        const u16 nameLen = Rd16(cd.data() + p + 28);
        const u16 extraLen = Rd16(cd.data() + p + 30);
        const u16 commentLen = Rd16(cd.data() + p + 32);
        z.localHeaderOffset = Rd32(cd.data() + p + 42);
        if (p + 46 + nameLen > cd.size()) return false;
        z.name.assign(reinterpret_cast<const char*>(cd.data() + p + 46), nameLen);
        z.isDirectory = !z.name.empty() && (z.name.back() == '/' || z.name.back() == '\\');
        entries_.push_back(std::move(z));
        p += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

bool ZipArchive::ExtractAll(const fs::path& destRoot, u64 maxTotalBytes,
                            std::string& outError) const {
    std::error_code ec;
    fs::create_directories(destRoot, ec);
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        outError = "cannot reopen the archive";
        return false;
    }

    u64 written = 0;
    for (const ZipEntry& z : entries_) {
        // EVERY entry goes through SafeJoin. No exceptions, no "trusted" prefix.
        const fs::path out = SafeJoin(destRoot, z.name);
        if (out.empty()) {
            outError = "archive entry escapes the destination: " + z.name;
            return false; // REFUSE THE WHOLE ARCHIVE - one hostile entry taints it all
        }
        if (z.isDirectory) {
            fs::create_directories(out, ec);
            continue;
        }
        if (z.method != 0 && z.method != 8) {
            outError = "unsupported compression method in: " + z.name;
            return false;
        }
        written += z.uncompressedSize;
        if (written > maxTotalBytes) {
            outError = "archive expands beyond the size cap (zip bomb?)";
            return false;
        }

        // Read the LOCAL header: its name/extra lengths differ from the central
        // directory's, and using the central values here is a classic off-by-N that
        // shifts every byte of the payload.
        u8 loc[30];
        in.seekg(static_cast<std::streamoff>(z.localHeaderOffset));
        in.read(reinterpret_cast<char*>(loc), sizeof(loc));
        if (!in || Rd32(loc) != kLocSig) {
            outError = "bad local header for: " + z.name;
            return false;
        }
        const u16 lname = Rd16(loc + 26), lextra = Rd16(loc + 28);
        in.seekg(static_cast<std::streamoff>(z.localHeaderOffset) + 30 + lname + lextra);

        std::vector<u8> comp(static_cast<usize>(z.compressedSize));
        if (!comp.empty()) {
            in.read(reinterpret_cast<char*>(comp.data()),
                    static_cast<std::streamsize>(comp.size()));
            if (!in) {
                outError = "truncated data for: " + z.name;
                return false;
            }
        }

        std::vector<u8> raw;
        if (z.method == 0) {
            raw = std::move(comp);
        } else {
            raw.resize(static_cast<usize>(z.uncompressedSize));
            z_stream s{};
            // -MAX_WBITS = RAW deflate. A zip member has no zlib header; passing the
            // positive value makes inflate reject every entry.
            if (::inflateInit2(&s, -MAX_WBITS) != Z_OK) {
                outError = "inflateInit failed";
                return false;
            }
            s.next_in = comp.data();
            s.avail_in = static_cast<uInt>(comp.size());
            s.next_out = raw.data();
            s.avail_out = static_cast<uInt>(raw.size());
            const int r = ::inflate(&s, Z_FINISH);
            ::inflateEnd(&s);
            if (r != Z_STREAM_END || s.total_out != z.uncompressedSize) {
                outError = "inflate failed for: " + z.name;
                return false;
            }
        }

        // VERIFY THE CRC. It is not a security control - an attacker can recompute it -
        // but it catches the truncated or partially-written download, which is by far
        // the likelier failure and would otherwise install a corrupt engine.
        const uLong crc = ::crc32(::crc32(0, nullptr, 0), raw.data(),
                                  static_cast<uInt>(raw.size()));
        if (static_cast<u32>(crc) != z.crc32) {
            outError = "CRC mismatch (corrupt download) for: " + z.name;
            return false;
        }

        fs::create_directories(out.parent_path(), ec);
        std::ofstream of(out, std::ios::binary | std::ios::trunc);
        if (!of) {
            outError = "cannot write: " + out.string();
            return false;
        }
        if (!raw.empty())
            of.write(reinterpret_cast<const char*>(raw.data()),
                     static_cast<std::streamsize>(raw.size()));
        if (!of.good()) {
            outError = "write failed: " + out.string();
            return false;
        }
    }
    return true;
}

// --- self-test ---------------------------------------------------------------

namespace {
int g_zfails = 0;
void Check(bool cond, const char* what) {
    if (cond) return;
    ++g_zfails;
    std::printf("hub FAIL: %s\n", what);
}
} // namespace

bool ZipSelfTest() {
    g_zfails = 0;
    const fs::path root = fs::temp_directory_path() / "hbe_zip_guard";

    // --- the containment guard, which is the whole point of this file ---
    Check(!SafeJoin(root, "../evil.dll").empty() == false, "'../evil.dll' must be refused");
    Check(SafeJoin(root, "../../../Windows/System32/x.dll").empty(),
          "a deep traversal must be refused");
    Check(SafeJoin(root, "..\\..\\evil.dll").empty(),
          "BACKSLASH traversal must be refused too (Windows accepts both separators)");
    Check(SafeJoin(root, "a/../../b").empty(),
          "a traversal that only escapes AFTER normalisation must be refused");
    Check(SafeJoin(root, "/absolute").empty(), "an absolute path must be refused");
    Check(SafeJoin(root, "C:/Windows/x.dll").empty(), "a drive-absolute path must be refused");
    Check(SafeJoin(root, std::string("ok.txt\0../evil", 14)).empty(),
          "an embedded NUL must be refused");
    Check(SafeJoin(root, "CON").empty(), "a reserved device name must be refused");
    Check(SafeJoin(root, "sub/NUL.txt").empty(),
          "a reserved device name in a subdirectory must be refused");
    Check(SafeJoin(root, "foo.exe ").empty(),
          "a trailing space must be refused (Windows strips it, causing a collision)");
    Check(SafeJoin(root, "").empty(), "an empty name must be refused");

    // ...and legitimate names must still work, or the guard is useless.
    Check(!SafeJoin(root, "bin/HeartbreakEditor.exe").empty(),
          "a normal nested path must be ALLOWED");
    Check(!SafeJoin(root, "shaders/MeshPBR.cso").empty(), "a normal path must be allowed");
    Check(!SafeJoin(root, "a/./b.txt").empty(), "a './' component is legal and must pass");
    Check(!SafeJoin(root, "..foo").empty(),
          "'..foo' is a LEGAL name and must not be caught by the '..' rule");

    // Containment must hold for every allowed path.
    {
        const fs::path p = SafeJoin(root, "bin/x/y.dll");
        const std::string rel = p.lexically_relative(root.lexically_normal()).generic_string();
        Check(!rel.empty() && rel.rfind("..", 0) != 0,
              "an allowed path resolved outside the destination root");
    }
    return g_zfails == 0;
}

} // namespace hbe::hub
