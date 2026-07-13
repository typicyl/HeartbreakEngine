// Assets/Mesh.h - CPU-side geometry + material data.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace hbe {

// Interleaved vertex. Layout must match the input layout the RHI builds and
// the VSInput in the shaders (POSITION, NORMAL, TANGENT, TEXCOORD0,
// BLENDINDICES, BLENDWEIGHT). Static meshes leave the skinning fields at
// their defaults (all weights zero = unskinned in the shader).
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f}; // xyz dir, w handedness
    glm::vec2 uv{0.0f};
    u16 joints[4] = {0, 0, 0, 0};   // skeleton joint indices
    f32 weights[4] = {0, 0, 0, 0};  // skinning weights (sum to 1 when skinned)
};
static_assert(sizeof(Vertex) == 72, "shader input layouts assume this layout");

// Metallic-roughness material parameters (glTF/PBR conventions).
struct Material {
    glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallic  = 0.0f;
    f32 roughness = 0.5f;
    glm::vec3 emissive{0.0f}; // linear emissive factor
    std::string name;
    // Texture references. During Assimp import these hold the *source* paths;
    // the importer rewrites them to `.uaf` asset references (relative to the
    // project's Assets/ dir) before serializing the mesh.
    std::string baseColorTex;
    std::string normalTex;
    std::string mrTex; // metallic-roughness (glTF: B=metal, G=rough)
    std::string aoTex;
    std::string emissiveTex;
    // Import-only (NOT serialized): when a model ships SEPARATE greyscale
    // metallic and roughness maps, they land here; the importer packs them into
    // `mrTex` (B=metal, G=rough). Cleared after import.
    std::string metallicTex;
    std::string roughnessTex;
    // `.hbmat` asset generated for this material at import time (path relative
    // to Assets/, empty for older assets). When set, spawning/loading applies
    // the material asset and links the entity with a MaterialRef.
    std::string materialAsset;
};

// One named blendshape / morph target: per-vertex position (+ optional normal)
// deltas PARALLEL to MeshData::vertices. Stored out-of-band so the fixed 72-byte
// Vertex layout (and every input layout / .uaf) stays byte-compatible. Applied on
// the GPU before skinning: morphedPos = pos + sum(weight_i * posDelta_i).
struct MorphTarget {
    std::string name;                // "jawOpen", "viseme_AA", "smile", "blink_L"
    std::vector<glm::vec3> posDelta; // size == vertices.size()
    std::vector<glm::vec3> nrmDelta; // size == vertices.size() (may be empty)
};

// One drawable submesh: interleaved vertices + 32-bit indices + a material.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32>    indices;
    Material            material;
    std::string         name;
    std::vector<MorphTarget> morphTargets; // blendshapes (empty for most meshes)

    u32 VertexCount() const { return static_cast<u32>(vertices.size()); }
    u32 IndexCount()  const { return static_cast<u32>(indices.size()); }
    bool Empty() const { return vertices.empty() || indices.empty(); }
};

// A model is a collection of submeshes (one Assimp scene / one procedural shape).
using Model = std::vector<MeshData>;

// One brush-stroke seed scattered on a mesh surface (object space), for the 3D
// painterly renderer. Uploaded as a per-instance vertex stream; the stroke VS
// (StrokeSurface.hlsl) expands each into a camera-facing card in world space and
// lights it with the scene's PBR lighting. GPU-ready layout (== the VS input).
struct StrokeInstance {
    glm::vec3 posOS;     // surface point (object space)
    glm::vec3 normalOS;  // surface normal (object space)
    glm::vec3 tangentOS; // surface tangent (object space), seeds stroke direction
    glm::vec2 uv;        // surface uv (albedo / paint canvas lookup)
    f32       seed;      // per-stroke random (size / angle / colour jitter)
};

// Local-space axis-aligned bounds of a mesh (min, max). Empty mesh -> zero box.
inline void ComputeBounds(const MeshData& m, glm::vec3& outMin, glm::vec3& outMax) {
    if (m.vertices.empty()) {
        outMin = outMax = glm::vec3(0.0f);
        return;
    }
    outMin = glm::vec3(1e30f);
    outMax = glm::vec3(-1e30f);
    for (const Vertex& v : m.vertices) {
        outMin = glm::min(outMin, v.position);
        outMax = glm::max(outMax, v.position);
    }
}

} // namespace hbe
