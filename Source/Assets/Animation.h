// Assets/Animation.h - skeletal animation data (skeletons, clips, rigs).
//
// A Rig is the animation half of a mesh asset: the skeleton (joint hierarchy
// with inverse-bind matrices) and the animation clips authored against it.
// Clips address joints BY NAME, which is what makes real-time retargeting
// possible: any clip can be sampled onto any skeleton whose joints share
// names (with common DCC prefixes like "mixamorig:" stripped); unmatched
// joints fall back to their bind pose.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace hbe {

// One joint of a skeleton. Parents always precede children in the joint
// array, so a single forward pass composes global transforms.
struct Joint {
    std::string name;
    i32 parent = -1;                 // index into Skeleton::joints (-1 = root)
    glm::mat4 inverseBind{1.0f};     // model space -> joint space at bind time
    // Local bind pose (the rest pose used for unmatched joints).
    glm::vec3 bindPosition{0.0f};
    glm::quat bindRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 bindScale{1.0f};
};

struct Skeleton {
    std::vector<Joint> joints;

    i32 Find(const std::string& name) const {
        for (usize i = 0; i < joints.size(); ++i) {
            if (joints[i].name == name) return static_cast<i32>(i);
        }
        return -1;
    }

    // Average parent->child bone length; the ratio between two skeletons
    // scales translation channels when retargeting.
    f32 AverageBoneLength() const {
        f32 total = 0.0f;
        u32 count = 0;
        for (const Joint& j : joints) {
            if (j.parent < 0) continue;
            total += glm::length(j.bindPosition);
            ++count;
        }
        return count > 0 ? total / count : 1.0f;
    }
};

// Keyframes of one joint (addressed by name; see retargeting note above).
struct AnimChannel {
    struct Vec3Key { f32 time = 0.0f; glm::vec3 value{0.0f}; };
    struct QuatKey { f32 time = 0.0f; glm::quat value{1.0f, 0.0f, 0.0f, 0.0f}; };

    std::string jointName;
    std::vector<Vec3Key> positions;
    std::vector<QuatKey> rotations;
    std::vector<Vec3Key> scales;
};

struct AnimationClip {
    std::string name;
    f32 duration = 0.0f; // seconds
    std::vector<AnimChannel> channels;
};

// The animation payload of a mesh asset.
struct Rig {
    Skeleton skeleton;
    std::vector<AnimationClip> clips;

    bool Valid() const { return !skeleton.joints.empty(); }
};

// Upper bound on joints per skeleton (palette sizing in the backends).
inline constexpr u32 kMaxJoints = 256;

} // namespace hbe
