// Schematic/SchematicBakedStub.cpp - the "no baked schematics" registration.
//
// Linked into every executable that does NOT bake schematics: the editor/hub/art
// tools (which must run graphs through the interpreter so edits take effect live),
// and the runtime when HBE_GAME_PROJECT is unset at configure time. A baked
// runtime links a HeartbreakBaker-generated file defining the same symbol instead.
#include "Schematic/SchematicSystem.h"

namespace hbe::schematic {

void RegisterBakedSchematics() {} // nothing baked -> Update() uses the interpreter

} // namespace hbe::schematic
