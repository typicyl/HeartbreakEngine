// Tools/SchematicBaker.cpp - bakes a project's .hbschem graphs into a C++ TU.
//
// Usage: HeartbreakBaker <projectRoot|assetsDir> <output.cpp>
//   The first arg is a project root (a folder containing Assets/) or an Assets/
//   directory directly. Every .hbschem under it is transpiled and registered by
//   its Assets-relative key (matching SchematicComponent.asset). The output file
//   is compiled into HeartbreakRuntime so the graphs run as native code.
#include "Assets/VFS.h"
#include "Schematic/Schematic.h"
#include "Schematic/SchematicTranspile.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc < 3) {
        std::cerr << "usage: HeartbreakBaker <projectRoot|assetsDir> <output.cpp>\n";
        return 2;
    }
    const fs::path root = argv[1];
    std::error_code ec;
    const fs::path assets = fs::exists(root / "Assets", ec) ? root / "Assets" : root;
    const fs::path out = argv[2];

    hbe::vfs::SetSearchRoot(assets); // pack-free loose reads for LoadGraph

    std::vector<std::pair<std::string, hbe::schematic::Graph>> graphs;
    if (fs::exists(assets, ec)) {
        for (auto it = fs::recursive_directory_iterator(assets, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file() || it->path().extension() != ".hbschem") continue;
            hbe::schematic::Graph g;
            if (!hbe::schematic::LoadGraph(it->path(), g)) {
                std::cerr << "  skip (parse failed): " << it->path().string() << "\n";
                continue;
            }
            const std::string key = fs::relative(it->path(), assets, ec).generic_string();
            graphs.emplace_back(key, std::move(g));
            std::cout << "  baked " << key << " (" << graphs.back().second.nodes.size() << " nodes)\n";
        }
    }

    const std::string tu = hbe::schematic::TranspileUnit(graphs);
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::cerr << "cannot write " << out.string() << "\n";
        return 1;
    }
    f << tu;
    std::cout << "Baked " << graphs.size() << " schematic(s) -> " << out.string() << "\n";
    return 0;
}
