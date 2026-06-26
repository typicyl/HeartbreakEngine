// Schematic/SchematicTranspile.h - turns a Graph into native C++ source.
//
// The baker (or the editor's "Bake Schematics" command) runs this over a
// project's `.hbschem` graphs to produce one translation unit that, when compiled
// into the runtime executable, runs each graph as native code instead of through
// the interpreter. The emitted code mirrors SchematicSystem's VM exactly: data
// pins become `Value` expressions, the execution chain becomes nested statements.
#pragma once

#include "Schematic/Schematic.h"

#include <string>
#include <utility>
#include <vector>

namespace hbe::schematic {

// Emits a single C++ function definition `static void <fnName>(CompiledContext&,
// NodeType)` reproducing `g`. (No surrounding namespace/includes - see TranspileUnit.)
std::string TranspileGraph(const Graph& g, const std::string& fnName);

// Emits a complete baked translation unit for a set of (assetKey, graph) pairs:
// includes, one function per graph, and the RegisterBakedSchematics() that binds
// each asset key to its function. This is the file compiled into a baked runtime.
std::string TranspileUnit(const std::vector<std::pair<std::string, Graph>>& graphs);

} // namespace hbe::schematic
