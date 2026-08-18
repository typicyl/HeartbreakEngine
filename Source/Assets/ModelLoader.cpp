// Assets/ModelLoader.cpp
#include "Assets/ModelLoader.h"
#include "Core/Log.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hbe {
namespace {

glm::mat4 ToGlm(const aiMatrix4x4& m) {
    // Assimp matrices are row-major; glm is column-major.
    return glm::mat4(m.a1, m.b1, m.c1, m.d1,  // column 0
                     m.a2, m.b2, m.c2, m.d2,
                     m.a3, m.b3, m.c3, m.d3,
                     m.a4, m.b4, m.c4, m.d4);
}

Material ConvertMaterial(const aiMaterial* src) {
    Material mat;

    aiString name;
    if (src->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
        mat.name = name.C_Str();
    }

    aiColor4D base;
    if (src->Get(AI_MATKEY_BASE_COLOR, base) == AI_SUCCESS ||
        src->Get(AI_MATKEY_COLOR_DIFFUSE, base) == AI_SUCCESS) {
        mat.baseColor = {base.r, base.g, base.b, base.a};
    }

    ai_real metallic = 0.0f;
    if (src->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
        mat.metallic = metallic;
    }
    ai_real roughness = 0.5f;
    if (src->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
        mat.roughness = roughness;
    }

    aiColor3D emissive;
    if (src->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
        mat.emissive = {emissive.r, emissive.g, emissive.b};
    }
    // KHR_materials_emissive_strength: HDR emitters (>1) fold the multiplier
    // into the factor so the existing emissive path lights correctly.
    ai_real emissiveStrength = 1.0f;
    if (src->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveStrength) == AI_SUCCESS &&
        emissiveStrength > 0.0f) {
        mat.emissive *= emissiveStrength;
    }

    // KHR glTF PBR extensions -> OpenPBR factors. Each key is optional: on a non-glTF file (or a
    // plain metallic-roughness glTF) Get() fails and the OpenPBR-default field is left untouched.
    // IOR is only taken when > 1 so an OBJ's Ni=1.0 (which would zero the specular F0) is ignored.
    ai_real khr;
    if (src->Get(AI_MATKEY_REFRACTI, khr) == AI_SUCCESS && khr > 1.0f) mat.ior = khr;             // ior
    if (src->Get(AI_MATKEY_TRANSMISSION_FACTOR, khr) == AI_SUCCESS) mat.transmission = khr;       // transmission
    if (src->Get(AI_MATKEY_CLEARCOAT_FACTOR, khr) == AI_SUCCESS) mat.clearcoat = khr;             // clearcoat
    if (src->Get(AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, khr) == AI_SUCCESS) mat.clearcoatRoughness = khr;
    if (src->Get(AI_MATKEY_ANISOTROPY_FACTOR, khr) == AI_SUCCESS) mat.anisotropy = khr;           // anisotropy
    if (src->Get(AI_MATKEY_SPECULAR_FACTOR, khr) == AI_SUCCESS) mat.specularFactor = khr;         // specular
    if (src->Get(AI_MATKEY_SHEEN_ROUGHNESS_FACTOR, khr) == AI_SUCCESS) mat.sheenRoughness = khr;  // sheen rough
    if (src->Get(AI_MATKEY_VOLUME_THICKNESS_FACTOR, khr) == AI_SUCCESS) mat.volumeThickness = khr;// volume
    // glTF attenuationDistance defaults to +Infinity ("no absorption"); Assimp reports that verbatim.
    // Keep only finite distances - an infinite one must map to "no volume" (transmission_depth 0), and
    // letting +inf through serialises to a JSON null that throws on reload.
    if (src->Get(AI_MATKEY_VOLUME_ATTENUATION_DISTANCE, khr) == AI_SUCCESS && std::isfinite(khr))
        mat.attenuationDistance = khr;
    aiColor3D khrCol;
    if (src->Get(AI_MATKEY_SHEEN_COLOR_FACTOR, khrCol) == AI_SUCCESS)
        mat.sheenColor = {khrCol.r, khrCol.g, khrCol.b};
    if (src->Get(AI_MATKEY_VOLUME_ATTENUATION_COLOR, khrCol) == AI_SUCCESS)
        mat.attenuationColor = {khrCol.r, khrCol.g, khrCol.b};

    // Texture source paths (as referenced by the model file).
    auto getTex = [&](aiTextureType type, std::string& out) {
        aiString path;
        if (src->GetTexture(type, 0, &path) == AI_SUCCESS) out = path.C_Str();
    };
    getTex(aiTextureType_BASE_COLOR, mat.baseColorTex);
    if (mat.baseColorTex.empty()) getTex(aiTextureType_DIFFUSE, mat.baseColorTex);
    getTex(aiTextureType_NORMALS, mat.normalTex);
    if (mat.normalTex.empty()) getTex(aiTextureType_HEIGHT, mat.normalTex); // OBJ bump
    // Metallic-roughness. glTF packs both into one texture (B=metal, G=rough),
    // exposed under METALNESS and DIFFUSE_ROUGHNESS pointing at the SAME file.
    // Some pipelines (Substance/FBX) ship SEPARATE greyscale maps - those go to
    // metallicTex/roughnessTex and the importer packs them into one mrTex.
    std::string metalPath, roughPath;
    getTex(aiTextureType_METALNESS, metalPath);
    getTex(aiTextureType_DIFFUSE_ROUGHNESS, roughPath);
    if (!metalPath.empty() && metalPath == roughPath) {
        mat.mrTex = metalPath; // combined, both keys point at it
    } else if (metalPath.empty() && roughPath.empty()) {
        getTex(aiTextureType_UNKNOWN, mat.mrTex); // glTF combined MR (single ref)
    } else {
        mat.metallicTex = metalPath; // separate greyscale map(s) -> pack at import
        mat.roughnessTex = roughPath;
    }
    getTex(aiTextureType_AMBIENT_OCCLUSION, mat.aoTex);
    if (mat.aoTex.empty()) getTex(aiTextureType_LIGHTMAP, mat.aoTex);
    getTex(aiTextureType_EMISSIVE, mat.emissiveTex);
    // A bare emissive map with a zero factor would multiply to black.
    if (!mat.emissiveTex.empty() && mat.emissive == glm::vec3(0.0f)) {
        mat.emissive = glm::vec3(1.0f);
    }
    return mat;
}

MeshData ConvertMesh(const aiMesh* src, const aiScene* scene, bool flipV) {
    MeshData mesh;
    mesh.name = src->mName.C_Str();
    mesh.vertices.reserve(src->mNumVertices);

    const bool hasNormals  = src->HasNormals();
    const bool hasTangents = src->mTangents != nullptr && src->mBitangents != nullptr;
    const bool hasUVs      = src->HasTextureCoords(0);

    for (u32 i = 0; i < src->mNumVertices; ++i) {
        Vertex v;
        v.position = {src->mVertices[i].x, src->mVertices[i].y, src->mVertices[i].z};
        if (hasNormals) {
            v.normal = {src->mNormals[i].x, src->mNormals[i].y, src->mNormals[i].z};
        }
        if (hasTangents) {
            const glm::vec3 t{src->mTangents[i].x, src->mTangents[i].y, src->mTangents[i].z};
            const glm::vec3 b{src->mBitangents[i].x, src->mBitangents[i].y,
                              src->mBitangents[i].z};
            // w = handedness so the shader rebuilds the right bitangent
            // (cross(N,T) * w). Mirrored UVs flip it; hardcoding +1 broke their
            // normal maps. Compare Assimp's bitangent to the reconstructed one.
            f32 w = glm::dot(glm::cross(v.normal, t), b) < 0.0f ? -1.0f : 1.0f;
            // Flipping V (below) mirrors the bitangent, so flip handedness with it -
            // otherwise normal maps light inverted on FBX/OBJ imports.
            if (flipV) w = -w;
            v.tangent = glm::vec4(t, w);
        }
        if (hasUVs) {
            const f32 vy = src->mTextureCoords[0][i].y;
            // This engine samples textures top-left (V-down), like glTF. FBX/OBJ
            // author UVs bottom-left (V-up), so flip V or the texture maps upside-
            // down (a Mixamo character's atlas lands scrambled).
            v.uv = {src->mTextureCoords[0][i].x, flipV ? 1.0f - vy : vy};
        }
        mesh.vertices.push_back(v);
    }

    // Blendshapes / morph targets: aiAnimMesh holds ABSOLUTE morphed positions;
    // subtract the base to get per-vertex deltas (parallel to mesh.vertices). The
    // vertex-count guard skips any target Assimp remapped out of lockstep (the
    // classic JoinIdenticalVertices morph-import footgun).
    for (u32 k = 0; k < src->mNumAnimMeshes; ++k) {
        const aiAnimMesh* am = src->mAnimMeshes[k];
        if (!am || am->mNumVertices != src->mNumVertices || !am->mVertices) continue;
        MorphTarget mt;
        mt.name = am->mName.length ? std::string(am->mName.C_Str()) : ("morph_" + std::to_string(k));
        mt.posDelta.resize(src->mNumVertices);
        for (u32 i = 0; i < src->mNumVertices; ++i)
            mt.posDelta[i] = {am->mVertices[i].x - src->mVertices[i].x,
                              am->mVertices[i].y - src->mVertices[i].y,
                              am->mVertices[i].z - src->mVertices[i].z};
        if (am->mNormals && src->mNormals) {
            mt.nrmDelta.resize(src->mNumVertices);
            for (u32 i = 0; i < src->mNumVertices; ++i)
                mt.nrmDelta[i] = {am->mNormals[i].x - src->mNormals[i].x,
                                  am->mNormals[i].y - src->mNormals[i].y,
                                  am->mNormals[i].z - src->mNormals[i].z};
        }
        mesh.morphTargets.push_back(std::move(mt));
    }

    mesh.indices.reserve(static_cast<usize>(src->mNumFaces) * 3);
    u32 skippedFaces = 0;
    for (u32 f = 0; f < src->mNumFaces; ++f) {
        const aiFace& face = src->mFaces[f];
        // TRIANGLES ONLY. aiProcess_Triangulate splits polygons, but it does NOT remove
        // points and lines - a source file with stray edge or vertex geometry (common in
        // CAD exports and in meshes with construction curves left in) still yields faces
        // with 1 or 2 indices. Appending those to a triangle index buffer does not just
        // add a degenerate: it SHIFTS every subsequent index by one or two, so the entire
        // rest of the mesh is rebuilt from the wrong vertices. The result is a shredded
        // model that looks like a bad export rather than an importer bug.
        if (face.mNumIndices != 3) {
            ++skippedFaces;
            continue;
        }
        for (u32 j = 0; j < 3; ++j) mesh.indices.push_back(face.mIndices[j]);
    }
    if (skippedFaces > 0) {
        HBE_WARN("[import] '{}': skipped {} non-triangle face(s) (points/lines). They "
                 "would have shifted every following triangle.",
                 mesh.name.empty() ? std::string("(unnamed)") : mesh.name, skippedFaces);
    }

    if (scene->mMaterials && src->mMaterialIndex < scene->mNumMaterials) {
        mesh.material = ConvertMaterial(scene->mMaterials[src->mMaterialIndex]);
    }
    return mesh;
}

// Builds the skeleton for an animated scene: every node that is a bone (or an
// ancestor of one) becomes a joint, in parent-before-child order.
Skeleton BuildSkeleton(const aiScene* scene,
                       std::unordered_map<std::string, i32>& outJointOf) {
    // Names of actual bones (they carry the authoritative offset matrices).
    std::unordered_map<std::string, const aiBone*> boneOf;
    for (u32 m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        for (u32 b = 0; b < mesh->mNumBones; ++b) {
            boneOf.emplace(mesh->mBones[b]->mName.C_Str(), mesh->mBones[b]);
        }
    }

    // Mark bones + all of their ancestors as joints.
    std::unordered_set<const aiNode*> marked;
    const auto markUp = [&](const aiNode* node) {
        for (const aiNode* n = node; n; n = n->mParent) {
            if (!marked.insert(n).second) break;
        }
    };
    const auto findAndMark = [&](auto&& self, const aiNode* node) -> void {
        if (boneOf.count(node->mName.C_Str())) markUp(node);
        for (u32 c = 0; c < node->mNumChildren; ++c) self(self, node->mChildren[c]);
    };
    findAndMark(findAndMark, scene->mRootNode);

    Skeleton skeleton;
    const auto build = [&](auto&& self, const aiNode* node, i32 parent) -> void {
        i32 myIndex = parent;
        if (marked.count(node)) {
            Joint j;
            j.name = node->mName.C_Str();
            j.parent = parent;
            aiVector3D s, t;
            aiQuaternion q;
            node->mTransformation.Decompose(s, q, t);
            j.bindPosition = {t.x, t.y, t.z};
            j.bindRotation = glm::quat(q.w, q.x, q.y, q.z);
            j.bindScale = {s.x, s.y, s.z};
            if (const auto it = boneOf.find(j.name); it != boneOf.end()) {
                j.inverseBind = ToGlm(it->second->mOffsetMatrix);
            } else {
                // Helper joints: derive the inverse bind from the bind-pose
                // global so the palette is identity at rest.
                glm::mat4 global = glm::translate(glm::mat4(1.0f), j.bindPosition) *
                                   glm::mat4_cast(j.bindRotation);
                global = glm::scale(global, j.bindScale);
                if (parent >= 0) {
                    glm::mat4 parentGlobal = glm::inverse(
                        skeleton.joints[static_cast<usize>(parent)].inverseBind);
                    global = parentGlobal * global;
                }
                j.inverseBind = glm::inverse(global);
            }
            myIndex = static_cast<i32>(skeleton.joints.size());
            outJointOf[j.name] = myIndex;
            skeleton.joints.push_back(std::move(j));
        }
        for (u32 c = 0; c < node->mNumChildren; ++c) {
            self(self, node->mChildren[c], myIndex);
        }
    };
    build(build, scene->mRootNode, -1);
    return skeleton;
}

// Extracts every animation as a clip of per-joint-name TRS channels.
std::vector<AnimationClip> ConvertClips(const aiScene* scene) {
    std::vector<AnimationClip> clips;
    for (u32 a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        const f64 tps = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;
        AnimationClip clip;
        clip.name = anim->mName.length > 0 ? anim->mName.C_Str()
                                           : "Clip " + std::to_string(a);
        clip.duration = static_cast<f32>(anim->mDuration / tps);
        for (u32 c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* na = anim->mChannels[c];
            AnimChannel ch;
            ch.jointName = na->mNodeName.C_Str();
            ch.positions.reserve(na->mNumPositionKeys);
            for (u32 k = 0; k < na->mNumPositionKeys; ++k) {
                const aiVectorKey& key = na->mPositionKeys[k];
                ch.positions.push_back({static_cast<f32>(key.mTime / tps),
                                        {key.mValue.x, key.mValue.y, key.mValue.z}});
            }
            ch.rotations.reserve(na->mNumRotationKeys);
            for (u32 k = 0; k < na->mNumRotationKeys; ++k) {
                const aiQuatKey& key = na->mRotationKeys[k];
                ch.rotations.push_back(
                    {static_cast<f32>(key.mTime / tps),
                     glm::quat(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z)});
            }
            ch.scales.reserve(na->mNumScalingKeys);
            for (u32 k = 0; k < na->mNumScalingKeys; ++k) {
                const aiVectorKey& key = na->mScalingKeys[k];
                ch.scales.push_back({static_cast<f32>(key.mTime / tps),
                                     {key.mValue.x, key.mValue.y, key.mValue.z}});
            }
            clip.channels.push_back(std::move(ch));
        }
        clips.push_back(std::move(clip));
    }
    return clips;
}

// Writes joint indices/weights into `mesh` from the aiMesh's bones (top 4 by
// weight, renormalized).
void ApplyVertexWeights(MeshData& mesh, const aiMesh* src,
                        const std::unordered_map<std::string, i32>& jointOf) {
    struct Influence { u16 joint; f32 weight; };
    std::vector<std::vector<Influence>> per(mesh.vertices.size());
    for (u32 b = 0; b < src->mNumBones; ++b) {
        const aiBone* bone = src->mBones[b];
        const auto it = jointOf.find(bone->mName.C_Str());
        if (it == jointOf.end()) continue;
        const u16 joint = static_cast<u16>(it->second);
        for (u32 w = 0; w < bone->mNumWeights; ++w) {
            const aiVertexWeight& vw = bone->mWeights[w];
            if (vw.mVertexId < per.size() && vw.mWeight > 0.0f) {
                per[vw.mVertexId].push_back({joint, vw.mWeight});
            }
        }
    }
    for (usize v = 0; v < per.size(); ++v) {
        auto& inf = per[v];
        if (inf.empty()) continue;
        std::sort(inf.begin(), inf.end(),
                  [](const Influence& a, const Influence& b) { return a.weight > b.weight; });
        if (inf.size() > 4) inf.resize(4);
        f32 total = 0.0f;
        for (const Influence& i : inf) total += i.weight;
        Vertex& vert = mesh.vertices[v];
        for (usize i = 0; i < inf.size(); ++i) {
            vert.joints[i] = inf[i].joint;
            vert.weights[i] = total > 0.0f ? inf[i].weight / total : 0.0f;
        }
    }
}

// Bakes a node-global transform into unskinned vertices (animated scenes keep
// the hierarchy, so placement isn't pre-baked by Assimp).
void BakeTransform(MeshData& mesh, const glm::mat4& m) {
    const glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(m)));
    for (Vertex& v : mesh.vertices) {
        v.position = glm::vec3(m * glm::vec4(v.position, 1.0f));
        v.normal = glm::normalize(normalMat * v.normal);
        const glm::vec3 t = glm::normalize(glm::mat3(m) * glm::vec3(v.tangent));
        v.tangent = glm::vec4(t, v.tangent.w);
    }
}

} // namespace

std::optional<Model> LoadModel(const std::string& path, Rig* outRig,
                               std::vector<EmbeddedTexture>* outEmbedded) {
    Assimp::Importer importer;

    // FBX: collapse $AssimpFbx$ pivot helper nodes (Mixamo rigs otherwise
    // explode from ~65 joints to several hundred, blowing the GPU palette and
    // polluting channel names), and honor the file's unit scale so cm-
    // authored content lands at meter scale like everything else.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    const unsigned int baseFlags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenUVCoords |
        aiProcess_ImproveCacheLocality |
        aiProcess_GlobalScale |     // apply UnitScaleFactor (FBX cm -> m)
        aiProcess_LimitBoneWeights; // max 4 influences per vertex

    const aiScene* scene = importer.ReadFile(path, baseFlags);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        HBE_ERROR("Assimp failed to load '{}': {}", path, importer.GetErrorString());
        return std::nullopt;
    }

    bool animated = scene->mNumAnimations > 0;
    for (u32 i = 0; !animated && i < scene->mNumMeshes; ++i) {
        animated = scene->mMeshes[i]->HasBones();
    }

    // Texture-coordinate origin: the engine samples top-left (V-down), which glTF
    // already uses, but FBX/OBJ/DAE/etc. author V-up (bottom-left). Flip V on those
    // so imported textures (e.g. a Mixamo character's atlas) map the right way up.
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool flipV = !(ext == ".gltf" || ext == ".glb");

    // Build the rig up front when present. We deliberately do NOT use
    // aiProcess_PreTransformVertices: instead we walk the node hierarchy and
    // bake each node's global transform into its (non-skinned) meshes ourselves.
    // That preserves per-node names and instancing (each placement is its own
    // named submesh), whereas PreTransformVertices merges meshes by material and
    // discards the authored object names.
    std::unordered_map<std::string, i32> jointOf;
    Rig rig;
    if (animated) {
        rig.skeleton = BuildSkeleton(scene, jointOf);
        rig.clips = ConvertClips(scene);
        // The GPU palette is fixed-size; a skeleton beyond it would make
        // vertices index past the uploaded matrices (garbage transforms can
        // hang the GPU). Strip the rig and import as a static mesh instead.
        if (rig.skeleton.joints.size() > kMaxJoints) {
            HBE_WARN("Model '{}': skeleton has {} joints (max {}); importing as a "
                     "static mesh (no animation). Split or decimate the rig to "
                     "keep it animated.",
                     path, rig.skeleton.joints.size(), kMaxJoints);
            rig = Rig{};
            jointOf.clear();
            animated = false;
        }
    }

    // Walk the scene graph, emitting one submesh per (node, mesh) instance.
    Model model;
    model.reserve(scene->mNumMeshes);
    const auto walk = [&](auto&& self, const aiNode* node, const glm::mat4& parent) -> void {
        const glm::mat4 global = parent * ToGlm(node->mTransformation);
        for (u32 i = 0; i < node->mNumMeshes; ++i) {
            const aiMesh* src = scene->mMeshes[node->mMeshes[i]];
            MeshData mesh = ConvertMesh(src, scene, flipV);
            if (!mesh.Empty()) {
                if (animated && src->HasBones()) {
                    // Skinned: vertices stay in bind space; the joint palette
                    // (global * inverseBind) places them at runtime.
                    ApplyVertexWeights(mesh, src, jointOf);
                } else {
                    // Static mesh (or a static prop inside a rigged file): bake
                    // the node's world transform into the vertices.
                    BakeTransform(mesh, global);
                }
                // Prefer the authored NODE name; fall back to the mesh name.
                if (node->mName.length > 0) mesh.name = node->mName.C_Str();
                model.push_back(std::move(mesh));
            }
        }
        for (u32 c = 0; c < node->mNumChildren; ++c) self(self, node->mChildren[c], global);
    };
    walk(walk, scene->mRootNode, glm::mat4(1.0f));

    if (model.empty()) {
        HBE_WARN("Model '{}' contained no drawable meshes.", path);
        return std::nullopt;
    }

    usize totalVerts = 0, totalTris = 0;
    for (const auto& m : model) {
        totalVerts += m.vertices.size();
        totalTris += m.indices.size() / 3;
    }
    HBE_INFO("Loaded model '{}': {} mesh(es), {} verts, {} tris{}",
             path, model.size(), totalVerts, totalTris,
             animated ? " (rigged)" : "");
    if (animated) {
        HBE_INFO("  rig: {} joints, {} clip(s)", rig.skeleton.joints.size(),
                 rig.clips.size());
    }
    if (outRig && animated && rig.Valid()) {
        *outRig = std::move(rig);
    }

    // Extract embedded textures (glb / embedded FBX) so the importer can write
    // them out instead of dropping "*N" references.
    if (outEmbedded && scene->mNumTextures > 0) {
        outEmbedded->reserve(scene->mNumTextures);
        for (u32 i = 0; i < scene->mNumTextures; ++i) {
            const aiTexture* t = scene->mTextures[i];
            EmbeddedTexture e;
            e.name = "*" + std::to_string(i);
            if (t->mFilename.length > 0) e.filename = t->mFilename.C_Str();
            if (t->mHeight == 0) {
                // Compressed image file (PNG/JPG/...); mWidth is the byte count.
                e.compressed = true;
                const u8* bytes = reinterpret_cast<const u8*>(t->pcData);
                e.data.assign(bytes, bytes + t->mWidth);
            } else {
                // Raw aiTexel array (BGRA8) -> RGBA8.
                e.width = t->mWidth;
                e.height = t->mHeight;
                const usize count = static_cast<usize>(t->mWidth) * t->mHeight;
                e.data.resize(count * 4);
                for (usize p = 0; p < count; ++p) {
                    const aiTexel& tx = t->pcData[p];
                    e.data[p * 4 + 0] = tx.r;
                    e.data[p * 4 + 1] = tx.g;
                    e.data[p * 4 + 2] = tx.b;
                    e.data[p * 4 + 3] = tx.a;
                }
            }
            outEmbedded->push_back(std::move(e));
        }
        HBE_INFO("  embedded textures: {}", scene->mNumTextures);
    }

    return model;
}

} // namespace hbe
