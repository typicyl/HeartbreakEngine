// Hub/ZipArchive.h - read a `.zip` release, safely.
//
// THE ONE THING THIS FILE EXISTS FOR: an archive entry's name is attacker-controlled
// data, and naive extraction treats it as a path. An entry called
// "../../../Windows/System32/foo.dll", or "C:/Windows/foo.dll", or one reached through
// a symlink, writes OUTSIDE the destination directory. That is "zip slip", it is the
// single most common way an updater becomes a privilege escalation, and the guard has
// to be in the extractor - not in the caller, who will forget.
//
// SafeJoin is therefore a separate, pure, individually-tested function, and Extract
// cannot write a byte without going through it.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hbe::hub {

struct ZipEntry {
    std::string name;       // as stored, forward slashes
    u64 compressedSize = 0;
    u64 uncompressedSize = 0;
    u32 crc32 = 0;
    u64 localHeaderOffset = 0;
    u16 method = 0;         // 0 = stored, 8 = deflate; anything else is refused
    bool isDirectory = false;
};

// Resolves `entryName` against `destRoot` and returns the absolute path ONLY if it
// stays inside `destRoot`. Returns an empty path for anything that escapes.
//
// Refuses: absolute paths ("/x", "C:/x", "\\\\server\\share"), any ".." component,
// drive-relative paths, NUL bytes, and reserved Windows device names (CON, PRN, AUX,
// NUL, COM1-9, LPT1-9 - opening one of those as a file is not a normal file write).
// The comparison is done on LEXICALLY NORMALISED paths, because "a/../../b" only
// reveals itself as an escape after normalisation.
std::filesystem::path SafeJoin(const std::filesystem::path& destRoot,
                               const std::string& entryName);

class ZipArchive {
public:
    // Reads the central directory. Fails on a truncated, non-zip, or absurd archive
    // rather than reading whatever it can - a partially understood archive is exactly
    // what an attacker wants extracted.
    bool Open(const std::filesystem::path& file);
    const std::vector<ZipEntry>& Entries() const { return entries_; }

    // Extracts everything under `destRoot`, creating directories as needed.
    //
    // `maxTotalBytes` caps the UNCOMPRESSED total. Without it a "zip bomb" - a few KiB
    // that inflate to terabytes - fills the disk. Returns false and leaves whatever was
    // already written for the caller to discard (the caller extracts into a STAGING
    // directory precisely so a failed extract is thrown away wholesale).
    bool ExtractAll(const std::filesystem::path& destRoot, u64 maxTotalBytes,
                    std::string& outError) const;

private:
    std::filesystem::path file_;
    std::vector<ZipEntry> entries_;
};

bool ZipSelfTest(); // part of --test-hub

} // namespace hbe::hub
