// Assets/ModelLoader.h - import meshes from disk via Assimp.
#pragma once

#include "Assets/Animation.h"
#include "Assets/Mesh.h"

#include <optional>
#include <string>
#include <vector>

namespace hbe {

// A texture stored *inside* the model file (glTF .glb, embedded FBX media).
// Material texture refs point at these as "*N". `data` is either an encoded
// image file (PNG/JPG/... bytes, when `compressed`) or raw RGBA8 pixels. The
// editor importer decodes/writes these to .uaf; the runtime never sees them.
struct EmbeddedTexture {
    std::string name;        // "*N" index ref (matches material refs)
    std::string filename;    // original basename if any (FBX refs by name)
    bool compressed = false; // true = encoded file bytes; false = raw RGBA8
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> data;
};

// Loads a model file (glTF/GLB/OBJ/FBX) into a list of submeshes. Triangulates,
// generates normals/tangents if missing. Returns std::nullopt on failure
// (errors are logged). Paths are UTF-8.
//
// Rigged models (bones or animations present) keep the node hierarchy alive:
// the skeleton + clips land in `outRig` (when given) and skinned vertices
// carry joint indices/weights. Static models are flattened as before.
//
// `outEmbedded` (when given) receives every texture embedded in the file so the
// importer can extract them instead of dropping "*N" material references.
std::optional<Model> LoadModel(const std::string& path, Rig* outRig = nullptr,
                               std::vector<EmbeddedTexture>* outEmbedded = nullptr);

} // namespace hbe
