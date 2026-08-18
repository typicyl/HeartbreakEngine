// Assets/CookStats.cpp - see CookStats.h.
#include "Assets/CookStats.h"

#include "Assets/AssetFormats.h"
#include "Assets/Compression.h"
#include "Assets/UAF.h"
#include "Assets/UAP.h"
#include "Core/Log.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace hbe::cookstats {
namespace fs = std::filesystem;
namespace {

// A pack key (Assets-relative path) -> a human category. `.uaf` files are split by the
// asset type recorded in their header (textures dominate most games; separating them is
// the whole point); a `.bc.uaf` is a compressed texture variant. Everything else is
// grouped by its engine extension.
std::string CategoryOf(const fs::path& assetsDir, const std::string& relPath) {
    const fs::path p(relPath);
    const std::string ext = assets::NormalizeExtension(p);
    if (ext == ".uaf") {
        const bool bc = p.stem().extension() == ".bc";
        switch (uaf::PeekType(assetsDir / p)) {
            case uaf::AssetType::Texture: return bc ? "Texture (BC)" : "Texture";
            case uaf::AssetType::Mesh:    return "Mesh";
            case uaf::AssetType::Audio:   return "Audio";
            case uaf::AssetType::Font:    return "Font";
            default:                      return "Asset (.uaf)";
        }
    }
    if (!ext.empty()) return ext; // .hbscene, .hbmat, .hbnav, ...
    return "(other)";
}

std::string MB(u64 bytes) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << (static_cast<double>(bytes) / (1024.0 * 1024.0));
    return o.str();
}

} // namespace

bool Report(const fs::path& assetsDir, const fs::path& manifestPath, const std::string& label,
            std::string& outText) {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / "hbe_cookstats";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    // Cook against a COPY of the ledger so the project's real slot manifest is never
    // written by a diagnostic run.
    const fs::path tmpManifest = tmp / "cookstats.uapmanifest";
    if (fs::exists(manifestPath, ec)) fs::copy_file(manifestPath, tmpManifest, ec);

    uap::WriteOptions opt;
    opt.compress = true;
    const auto res = uap::WritePacks(tmp, "cookstats", assetsDir, tmpManifest, opt);
    if (!res) {
        fs::remove_all(tmp, ec);
        return false;
    }

    struct Cat {
        u32 count = 0;
        u64 rawBytes = 0;    // loose .uaf/asset bytes (pre-pack-compression)
        u64 packedBytes = 0; // bytes in the pack after codec + dedup
    };
    std::map<std::string, Cat> cats;
    struct Big {
        std::string path;
        u64 raw = 0;
        u64 packed = 0;
    };
    std::vector<Big> big;

    // Read + decompress + hash-verify EVERY packed asset: this proves the cooked pack
    // actually loads (disk -> decompress -> validate), reports the total decompression
    // time, and catches any integrity mismatch - the "validation" half of the tool.
    u32 verified = 0, failed = 0;
    double decodeMs = 0.0;
    uap::PackSet set;
    if (set.Open(tmp, "cookstats")) {
        for (const uap::Entry& e : set.Entries()) {
            const std::string cat = CategoryOf(assetsDir, e.path);
            Cat& c = cats[cat];
            ++c.count;
            c.rawBytes += e.rawSize;
            c.packedBytes += e.size;
            big.push_back({e.path, e.rawSize, e.size});

            const auto t0 = std::chrono::steady_clock::now();
            const auto bytes = set.Read(e.path); // disk read + decompress
            const auto t1 = std::chrono::steady_clock::now();
            decodeMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (bytes && bytes->size() == e.rawSize &&
                (e.contentHash == 0 ||
                 comp::Hash64(bytes->data(), bytes->size()) == e.contentHash))
                ++verified;
            else
                ++failed;
        }
    }

    u64 packFileBytes = 0;
    for (u32 p = 0; p < res->packCount; ++p) {
        const fs::path pf = tmp / ("cookstats_" + std::to_string(p) + ".uap");
        if (fs::exists(pf, ec)) packFileBytes += fs::file_size(pf, ec);
    }

    // ---- render ----
    std::ostringstream o;
    o << "Cook stats for " << label << "\n";
    o << "  " << res->assetCount << " asset(s) in " << res->packCount << " pack(s); codec zstd(level "
      << comp::DefaultZstdLevel() << ")\n\n";
    o << "  Category           Count      Source(MB)     Packed(MB)   Ratio\n";
    o << "  ---------------------------------------------------------------------\n";

    // Sort categories by raw (source) size descending - the biggest sink first.
    std::vector<std::pair<std::string, Cat>> rows(cats.begin(), cats.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.rawBytes > b.second.rawBytes; });
    char line[256];
    for (const auto& [name, c] : rows) {
        const int pct = c.rawBytes ? static_cast<int>(c.packedBytes * 100 / c.rawBytes) : 100;
        std::snprintf(line, sizeof(line), "  %-16s %6u  %13s  %13s   %3d%%\n", name.c_str(),
                      c.count, MB(c.rawBytes).c_str(), MB(c.packedBytes).c_str(), pct);
        o << line;
    }
    o << "  ---------------------------------------------------------------------\n";
    const int totalPct = res->rawBytes ? static_cast<int>(res->packedBytes * 100 / res->rawBytes) : 100;
    std::snprintf(line, sizeof(line), "  %-16s %6u  %13s  %13s   %3d%%\n", "TOTAL",
                  res->assetCount, MB(res->rawBytes).c_str(), MB(res->packedBytes).c_str(),
                  totalPct);
    o << line;
    o << "\n  Pack files on disk: " << MB(packFileBytes) << " MB (TOC + aligned blobs)\n";
    {
        char v[192];
        std::snprintf(v, sizeof(v),
                      "  Load check: %u/%u asset(s) decompressed + hash-verified in %.1f ms"
                      " (%s)\n",
                      verified, verified + failed, decodeMs,
                      failed == 0 ? "all OK" : "INTEGRITY FAILURES");
        o << v;
    }
    if (res->dedupedCount)
        o << "  Content dedup: " << res->dedupedCount << " duplicate slot(s), saving "
          << MB(res->dedupedBytes) << " MB\n";
    else
        o << "  Content dedup: none (no byte-identical assets within a pack)\n";

    // Largest assets - the "what specifically is big" answer.
    std::sort(big.begin(), big.end(), [](const Big& a, const Big& b) { return a.raw > b.raw; });
    o << "\n  Largest assets (source -> packed):\n";
    for (usize i = 0; i < big.size() && i < 10; ++i) {
        std::snprintf(line, sizeof(line), "    %10s -> %-10s  %s\n",
                      (MB(big[i].raw) + " MB").c_str(), (MB(big[i].packed) + " MB").c_str(),
                      big[i].path.c_str());
        o << line;
    }
    if (!rows.empty())
        o << "\n  Biggest sink: " << rows.front().first << " (" << MB(rows.front().second.rawBytes)
          << " MB of source, " << MB(rows.front().second.packedBytes) << " MB packed).\n";

    outText = o.str();
    fs::remove_all(tmp, ec);
    return true;
}

// ---------------------------------------------------------------------------
// Self-test (--test-cookstats)
// ---------------------------------------------------------------------------
bool SelfTest() {
    u32 fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { HBE_ERROR("cookstats: FAIL - {}", what); ++fails; }
    };
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "hbe_cookstats_test";
    fs::remove_all(dir, ec);
    const fs::path assets = dir / "Assets";
    fs::create_directories(assets, ec);

    // A texture, a mesh, an audio clip, and a byte-identical texture duplicate.
    {
        uaf::Texture t;
        t.width = 64;
        t.height = 64;
        t.format = 28; // R8G8B8A8_UNORM-ish; value is not interpreted by cook stats
        t.mipCount = 1;
        t.pixels.assign(64u * 64u * 4u, 0x80);
        check(uaf::WriteTexture(assets / "tex.uaf", t, 1), "write texture");
        // A byte-identical duplicate = a copy-pasted .uaf (same guid + bytes). This is the
        // realistic dedup case; two separately-imported identical images get distinct guids
        // and are (correctly) NOT deduped, since the guid is the asset's identity.
        fs::copy_file(assets / "tex.uaf", assets / "tex_dup.uaf", ec);
    }
    {
        MeshData md;
        for (int z = 0; z < 20; ++z)
            for (int x = 0; x < 20; ++x) {
                Vertex v;
                v.position = {static_cast<f32>(x), 0.0f, static_cast<f32>(z)};
                v.normal = {0, 1, 0};
                md.vertices.push_back(v);
            }
        for (u32 z = 0; z < 19; ++z)
            for (u32 x = 0; x < 19; ++x) {
                const u32 i0 = z * 20 + x, i1 = i0 + 1, i2 = i0 + 20, i3 = i2 + 1;
                md.indices.insert(md.indices.end(), {i0, i2, i1, i1, i2, i3});
            }
        check(uaf::WriteMesh(assets / "mesh.uaf", Model{md}, 3), "write mesh");
    }
    {
        uaf::Audio a;
        a.channels = 1;
        a.sampleRate = 22050;
        a.bitsPerSample = 16;
        a.pcm.assign(22050u * 2u, 0); // 1s of silence (raw-PCM path)
        check(uaf::WriteAudio(assets / "sound.uaf", a, 4), "write audio");
    }

    std::string text;
    const bool ok = Report(assets, dir / "none.uapmanifest", "self-test", text);
    check(ok, "Report must succeed");
    check(text.find("Texture") != std::string::npos, "report mentions textures");
    check(text.find("Mesh") != std::string::npos, "report mentions meshes");
    check(text.find("Audio") != std::string::npos, "report mentions audio");
    check(text.find("dedup") != std::string::npos, "report mentions dedup");
    check(text.find("duplicate slot") != std::string::npos, "the identical texture must dedup");
    if (ok) std::printf("%s\n", text.c_str());

    fs::remove_all(dir, ec);
    if (fails == 0) HBE_INFO("cookstats: passed (report renders, categories + dedup detected).");
    return fails == 0;
}

} // namespace hbe::cookstats
