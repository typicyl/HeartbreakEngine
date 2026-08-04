#include "Scene/BodyShape.h"

#include "Assets/Animation.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace hbe::bodyshape {

namespace {

// Mirror of the canonicalisation the joint remap and the seam weld already use. A rig that
// exports "mixamorig:LeftArm" and one that exports "upperarm_l" have to reach the same
// group, or the sliders only work for whoever authored the table.
std::string Canonical(const std::string& name) {
    std::string s = name;
    const usize sep = s.find_last_of(":|");
    if (sep != std::string::npos) s = s.substr(sep + 1);
    if (const usize fbx = s.find("$AssimpFbx$"); fbx != std::string::npos) {
        s = s.substr(0, fbx);
        while (!s.empty() && s.back() == '_') s.pop_back();
    }
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The semantic groups a slider can address. Deliberately coarse: these are the divisions a
// person actually thinks in when making a character, not an anatomical model.
enum Group : u32 {
    G_Pelvis, G_Spine, G_Chest, G_Neck, G_Head,
    G_Clavicle, G_UpperArm, G_Forearm, G_Hand,
    G_Thigh, G_Calf, G_Foot,
    G_Count
};

struct GroupRule {
    Group group;
    const char* include[6];
    const char* exclude[5]; // matched FIRST; "leftforearm" must not land in the arm group
};

// Order matters only through the exclusions. Every widely used rig convention is covered:
// Mixamo (LeftArm/LeftForeArm/LeftUpLeg), Unreal (upperarm_l/lowerarm_l/thigh_l/calf_l),
// HumanIK/Maya (LeftShoulder/LeftElbow), and plain Blender rigs.
constexpr GroupRule kRules[] = {
    {G_Pelvis,   {"pelvis", "hips", "hip", nullptr},                {nullptr}},
    {G_Chest,    {"chest", "spine2", "spine_02", "spine_03", "spine3", nullptr}, {nullptr}},
    {G_Spine,    {"spine", nullptr},                                {"spine2", "spine3", "spine_02", "spine_03", nullptr}},
    {G_Neck,     {"neck", nullptr},                                 {nullptr}},
    {G_Head,     {"head", nullptr},                                 {"headtop", "head_end", "headend", nullptr}},
    {G_Clavicle, {"clavicle", "shoulder", nullptr},                 {nullptr}},
    {G_Forearm,  {"forearm", "foarm", "lowerarm", "elbow", nullptr},{nullptr}},
    {G_UpperArm, {"upperarm", "arm", nullptr},                      {"forearm", "foarm", "lowerarm", "hand", nullptr}},
    {G_Hand,     {"hand", nullptr},                                 {nullptr}},
    {G_Thigh,    {"thigh", "upleg", "upperleg", nullptr},           {nullptr}},
    {G_Calf,     {"calf", "lowerleg", "shin", "leg", nullptr},      {"upleg", "upperleg", "thigh", nullptr}},
    {G_Foot,     {"foot", "ankle", nullptr},                        {nullptr}},
};

// Substring matching alone is WRONG here, and the self-test is what proved it: "prop_handle"
// contains "hand", and "armature" contains "arm" - both are real bone names, and both would
// silently be scaled as body parts. Requiring the match to end a token fixes it without
// giving up the concatenated conventions ("LeftHand", "LeftArm") that have no separator at
// all: "hand" ending "lefthand" counts, "hand" followed by another letter does not.
bool EndsToken(const std::string& s, usize at, usize len) {
    const usize after = at + len;
    if (after >= s.size()) return true;                 // ...ends the name
    const char c = s[after];
    return !(c >= 'a' && c <= 'z');                     // a digit, '_', '.', '-' all end it
}

bool Has(const char* const* list, const std::string& s) {
    for (const char* const* p = list; *p; ++p) {
        const usize len = std::char_traits<char>::length(*p);
        for (usize at = s.find(*p); at != std::string::npos; at = s.find(*p, at + 1))
            if (EndsToken(s, at, len)) return true;
    }
    return false;
}

// -1 when this joint belongs to no group (fingers, twist bones, props, IK helpers). Those
// are left exactly as imported - a slider must never move a bone it does not understand.
i32 GroupOf(const std::string& canonical) {
    for (const GroupRule& r : kRules) {
        if (r.exclude[0] && Has(r.exclude, canonical)) continue;
        if (Has(r.include, canonical)) return static_cast<i32>(r.group);
    }
    return -1;
}

// A slider: how much it lengthens and how much it thickens each group it touches.
// `lengthen` scales ALONG the bone, `thicken` scales across it - which is the difference
// between a tall person and a broad one, and the reason this reads as a character creator
// rather than a zoom control.
struct SliderRule {
    const char* name;
    const char* tip;
    struct Part { Group group; f32 lengthen; f32 thicken; };
    Part parts[8];
};

constexpr SliderRule kSliders[] = {
    {"Overall Size", "Bigger or smaller all over, keeping the same proportions.",
     {{G_Pelvis, 0.22f, 0.22f}}},
    {"Height", "Taller through the legs and back, without getting wider.",
     {{G_Thigh, 0.26f, 0.0f}, {G_Calf, 0.26f, 0.0f}, {G_Spine, 0.16f, 0.0f}, {G_Chest, 0.12f, 0.0f}}},
    {"Build", "Heavier or slighter through the whole body.",
     {{G_Pelvis, 0.0f, 0.20f}, {G_Spine, 0.0f, 0.22f}, {G_Chest, 0.0f, 0.20f},
      {G_UpperArm, 0.0f, 0.18f}, {G_Forearm, 0.0f, 0.15f},
      {G_Thigh, 0.0f, 0.20f}, {G_Calf, 0.0f, 0.16f}}},
    {"Shoulder Width", "Broad or narrow across the shoulders. Carries the arms with it.",
     {{G_Clavicle, 0.35f, 0.0f}}},
    {"Chest", "Fuller or flatter through the ribcage.", {{G_Chest, 0.0f, 0.28f}}},
    {"Waist", "Thicker or narrower at the middle.", {{G_Spine, 0.0f, 0.28f}}},
    {"Hips", "Wider or narrower at the hips.", {{G_Pelvis, 0.0f, 0.22f}}},
    {"Arm Length", "Longer or shorter arms.", {{G_UpperArm, 0.24f, 0.0f}, {G_Forearm, 0.24f, 0.0f}}},
    {"Arm Thickness", "Heavier or thinner arms.", {{G_UpperArm, 0.0f, 0.26f}, {G_Forearm, 0.0f, 0.22f}}},
    {"Leg Length", "Longer or shorter legs.", {{G_Thigh, 0.24f, 0.0f}, {G_Calf, 0.24f, 0.0f}}},
    {"Leg Thickness", "Heavier or thinner legs.", {{G_Thigh, 0.0f, 0.26f}, {G_Calf, 0.0f, 0.22f}}},
    {"Neck", "A longer, thinner neck or a shorter, thicker one.", {{G_Neck, 0.25f, 0.18f}}},
    {"Head Size", "A larger or smaller head.", {{G_Head, 0.16f, 0.16f}}},
    {"Hand Size", "Larger or smaller hands.", {{G_Hand, 0.22f, 0.22f}}},
    {"Foot Size", "Larger or smaller feet.", {{G_Foot, 0.22f, 0.22f}}},
};

// The bone's direction in its OWN local frame, taken from where its child sits. A joint with
// no child (a chain tip: fingertips, head top) has no bone to run along, so it falls back to
// uniform - scaling it "lengthwise" would be meaningless rather than wrong.
glm::vec3 BoneAxis(const Skeleton& sk, usize j) {
    glm::vec3 sum(0.0f);
    u32 n = 0;
    for (usize c = j + 1; c < sk.joints.size(); ++c) {
        if (sk.joints[c].parent != static_cast<i32>(j)) continue;
        const glm::vec3 d = sk.joints[c].bindPosition;
        const f32 len = glm::length(d);
        if (len > 1e-6f) {
            sum += d / len;
            ++n;
        }
    }
    if (n == 0) return glm::vec3(0.0f); // no axis -> treat every slider as uniform
    const glm::vec3 a = sum / static_cast<f32>(n);
    const f32 len = glm::length(a);
    return len > 1e-6f ? a / len : glm::vec3(0.0f);
}

} // namespace

std::vector<SliderDesc> Resolve(const Skeleton& sk) {
    bool found[G_Count] = {};
    for (const Joint& j : sk.joints) {
        const i32 g = GroupOf(Canonical(j.name));
        if (g >= 0) found[g] = true;
    }
    std::vector<SliderDesc> out;
    out.reserve(std::size(kSliders));
    for (const SliderRule& s : kSliders) {
        SliderDesc d;
        d.name = s.name;
        d.tip = s.tip;
        // Present only if the rig actually has something for it to move. Offering a control
        // that silently does nothing is worse than not offering it.
        for (const SliderRule::Part& p : s.parts) {
            if (p.lengthen == 0.0f && p.thicken == 0.0f) break;
            if (found[p.group]) {
                d.present = true;
                break;
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

void Bake(const Skeleton& sk, const std::unordered_map<std::string, f32>& values,
          std::vector<JointShape>& out) {
    out.clear();
    if (sk.joints.empty() || values.empty()) return;

    // Accumulate per group first: several sliders can touch one group (Build and Waist both
    // thicken the spine), and they have to combine rather than the last one winning.
    f32 lengthen[G_Count] = {};
    f32 thicken[G_Count] = {};
    bool any = false;
    for (const SliderRule& s : kSliders) {
        const auto it = values.find(s.name);
        if (it == values.end()) continue;
        const f32 v = std::clamp(it->second, kMin, kMax);
        if (v == 0.0f) continue;
        for (const SliderRule::Part& p : s.parts) {
            if (p.lengthen == 0.0f && p.thicken == 0.0f) break;
            lengthen[p.group] += v * p.lengthen;
            thicken[p.group] += v * p.thicken;
            any = true;
        }
    }
    if (!any) return;

    for (usize j = 0; j < sk.joints.size(); ++j) {
        const i32 g = GroupOf(Canonical(sk.joints[j].name));
        if (g < 0) continue;
        const f32 L = lengthen[g], T = thicken[g];
        if (L == 0.0f && T == 0.0f) continue;

        // Split the scale between "along the bone" and "across it" by how much each local
        // axis points down the bone. |axis| is used because a bone pointing down -Y must
        // lengthen the same way as one pointing +Y.
        const glm::vec3 a = glm::abs(BoneAxis(sk, j));
        glm::vec3 s;
        for (int k = 0; k < 3; ++k) {
            const f32 along = a[k];              // 0 = purely across, 1 = purely along
            s[k] = 1.0f + L * along + T * (1.0f - along);
            // A joint may never invert or collapse: a non-positive scale flips winding and
            // turns the mesh inside out, and the slider range alone does not guarantee it
            // once several sliders stack on one group.
            s[k] = std::max(s[k], 0.05f);
        }
        JointShape js;
        js.joint = static_cast<u16>(j);
        js.scale = s;
        out.push_back(js);
    }
}

// --- self test ----------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("bodyshape FAIL: %s\n", what);
        ++g_fails;
    }
}

Joint MakeJoint(const char* name, i32 parent, glm::vec3 pos) {
    Joint j;
    j.name = name;
    j.parent = parent;
    j.bindPosition = pos;
    return j;
}
} // namespace

bool SelfTest() {
    g_fails = 0;

    // A minimal Mixamo-flavoured rig: the naming convention is half of what is being tested.
    Skeleton sk;
    sk.joints.push_back(MakeJoint("mixamorig:Hips", -1, {0.0f, 1.0f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:Spine", 0, {0.0f, 0.2f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:Spine2", 1, {0.0f, 0.2f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:Neck", 2, {0.0f, 0.15f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:Head", 3, {0.0f, 0.12f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:LeftShoulder", 2, {0.1f, 0.05f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:LeftArm", 5, {0.15f, 0.0f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:LeftForeArm", 6, {0.25f, 0.0f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:LeftHand", 7, {0.22f, 0.0f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:LeftUpLeg", 0, {0.08f, -0.05f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:LeftLeg", 9, {0.0f, -0.4f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:LeftFoot", 10, {0.0f, -0.4f, 0.0f}));
    sk.joints.push_back(MakeJoint("mixamorig:LeftHandIndex1", 8, {0.05f, 0.0f, 0.0f}));

    // GROUPING. The exclusions are the fragile part: "LeftForeArm" contains "arm", and
    // "LeftUpLeg" contains "leg". Getting either wrong silently moves the wrong bone.
    Check(GroupOf(Canonical("mixamorig:LeftArm")) == G_UpperArm, "LeftArm must be the upper arm");
    Check(GroupOf(Canonical("mixamorig:LeftForeArm")) == G_Forearm,
          "LeftForeArm must be the FOREARM - 'arm' must not swallow it");
    Check(GroupOf(Canonical("mixamorig:LeftUpLeg")) == G_Thigh, "LeftUpLeg must be the thigh");
    Check(GroupOf(Canonical("mixamorig:LeftLeg")) == G_Calf,
          "LeftLeg must be the calf - 'leg' must not swallow UpLeg");
    Check(GroupOf(Canonical("mixamorig:Spine2")) == G_Chest, "Spine2 is the chest");
    Check(GroupOf(Canonical("mixamorig:Spine")) == G_Spine,
          "Spine must NOT be the chest - the chest patterns are matched first");
    // A finger belongs to NO group, and that is correct rather than a gap: fingers are
    // children of the hand, so scaling the hand already carries them. Putting them in the
    // hand group too would scale them TWICE.
    Check(GroupOf(Canonical("mixamorig:LeftHandIndex1")) < 0,
          "a finger must not be grouped - the hand it hangs off already scales it");

    // Unreal naming must reach the same groups, from the same table.
    Check(GroupOf(Canonical("upperarm_l")) == G_UpperArm, "upperarm_l");
    Check(GroupOf(Canonical("lowerarm_l")) == G_Forearm, "lowerarm_l");
    Check(GroupOf(Canonical("thigh_r")) == G_Thigh, "thigh_r");
    Check(GroupOf(Canonical("calf_r")) == G_Calf, "calf_r");
    Check(GroupOf(Canonical("clavicle_l")) == G_Clavicle, "clavicle_l");
    Check(GroupOf(Canonical("some_prop_bone")) < 0,
          "a bone in no group must be left ALONE, not silently scaled");
    // Real bone names that a naive substring match scales by mistake.
    Check(GroupOf(Canonical("prop_handle")) < 0, "'handle' is not a HAND");
    Check(GroupOf(Canonical("Armature")) < 0, "'armature' is not an ARM");
    Check(GroupOf(Canonical("door_handle")) < 0, "'handle' is never a hand");
    Check(GroupOf(Canonical("LeftHand")) == G_Hand, "...but 'LeftHand' still is a hand");
    Check(GroupOf(Canonical("LeftArm")) == G_UpperArm, "...and 'LeftArm' is still an arm");

    const std::vector<SliderDesc> sliders = Resolve(sk);
    Check(sliders.size() == std::size(kSliders), "every slider is described");
    for (const SliderDesc& d : sliders)
        Check(d.present, "this rig has every body group, so every slider should be offered");

    // A rig with nothing recognisable offers nothing - and still must not fail to load.
    Skeleton bare;
    bare.joints.push_back(MakeJoint("root", -1, {0.0f, 0.0f, 0.0f}));
    bare.joints.push_back(MakeJoint("prop_handle", 0, {0.0f, 0.1f, 0.0f}));
    bare.joints.push_back(MakeJoint("Armature", 0, {0.0f, 0.1f, 0.0f}));
    for (const SliderDesc& d : Resolve(bare))
        Check(!d.present, "an unrecognised rig must offer no sliders rather than dead ones");

    // BAKING. Zero in = the rig exactly as imported: a character creator must have an
    // identity setting, or every character is subtly wrong the moment it is created.
    std::vector<JointShape> out;
    Bake(sk, {}, out);
    Check(out.empty(), "no values means NO joint changes at all");
    Bake(sk, {{"Height", 0.0f}, {"Build", 0.0f}}, out);
    Check(out.empty(), "all-zero sliders must be exactly identity, not a near-identity scale");
    Bake(sk, {{"Not A Slider", 1.0f}}, out);
    Check(out.empty(), "an unknown slider name is ignored, not an error");

    // LENGTH vs THICKNESS. The left leg runs down -Y, so "Leg Length" must scale Y and leave
    // X/Z alone. This is the claim that makes the sliders rig-agnostic; if the derived bone
    // axis were wrong, a "longer" leg would come out fatter instead.
    Bake(sk, {{"Leg Length", 1.0f}}, out);
    const auto find = [&](const char* name) -> const JointShape* {
        const i32 idx = sk.Find(name);
        for (const JointShape& js : out)
            if (js.joint == static_cast<u16>(idx)) return &js;
        return nullptr;
    };
    const JointShape* thigh = find("mixamorig:LeftUpLeg");
    Check(thigh != nullptr, "Leg Length should touch the thigh");
    if (thigh) {
        Check(thigh->scale.y > 1.15f, "a longer leg must actually lengthen along the bone (-Y)");
        Check(std::abs(thigh->scale.x - 1.0f) < 0.02f,
              "a LONGER leg must not also get THICKER - length and girth are different axes");
        Check(std::abs(thigh->scale.z - 1.0f) < 0.02f, "same for Z");
    }

    Bake(sk, {{"Leg Thickness", 1.0f}}, out);
    thigh = find("mixamorig:LeftUpLeg");
    Check(thigh != nullptr, "Leg Thickness should touch the thigh");
    if (thigh) {
        Check(thigh->scale.x > 1.15f, "a thicker leg must widen across the bone");
        Check(std::abs(thigh->scale.y - 1.0f) < 0.02f,
              "a THICKER leg must not also get LONGER");
    }

    // The arm runs down +X, so the same slider pair must pick a DIFFERENT axis there. This
    // is what proves the axis is derived from the rig rather than hardcoded to Y.
    Bake(sk, {{"Arm Length", 1.0f}}, out);
    const JointShape* arm = find("mixamorig:LeftArm");
    Check(arm != nullptr, "Arm Length should touch the upper arm");
    if (arm) {
        Check(arm->scale.x > 1.15f, "the arm runs along X, so a longer arm must scale X");
        Check(std::abs(arm->scale.y - 1.0f) < 0.02f,
              "the length axis must be DERIVED per bone, not assumed to be Y");
    }

    // Stacking. Two sliders on one group must combine, not overwrite.
    Bake(sk, {{"Waist", 1.0f}, {"Build", 1.0f}}, out);
    const JointShape* spine = find("mixamorig:Spine");
    Check(spine != nullptr, "the waist sliders should touch the spine");
    if (spine)
        Check(spine->scale.x > 1.4f,
              "two sliders on the same group must ADD - the second must not replace the first");

    // A bone in no group is never touched, at any setting.
    Bake(sk, {{"Overall Size", 1.0f}, {"Build", 1.0f}, {"Height", 1.0f}}, out);
    const i32 finger = sk.Find("mixamorig:LeftHandIndex1");
    bool touchedFinger = false;
    for (const JointShape& js : out)
        if (js.joint == static_cast<u16>(finger)) touchedFinger = true;
    Check(!touchedFinger,
          "no slider may write a finger joint directly - it would compound with the hand");
    for (const JointShape& js : out) {
        Check(js.scale.x > 0.0f && js.scale.y > 0.0f && js.scale.z > 0.0f,
              "A JOINT SCALE MUST NEVER REACH ZERO OR INVERT - it turns the mesh inside out");
    }

    // Extremes must stay sane, because a user WILL drag every slider to the end.
    for (f32 v : {kMin, kMax}) {
        std::unordered_map<std::string, f32> all;
        for (const SliderRule& s : kSliders) all[s.name] = v;
        Bake(sk, all, out);
        for (const JointShape& js : out)
            Check(js.scale.x >= 0.05f && js.scale.y >= 0.05f && js.scale.z >= 0.05f,
                  "every slider at its limit at once must still leave a usable skeleton");
    }

    if (g_fails == 0)
        std::printf("bodyshape: %d sliders resolved from joint names alone (Mixamo and Unreal "
                    "conventions both), length and girth split on a bone axis DERIVED from the "
                    "rig, sliders stack, unknown bones untouched, and zero means identity\n",
                    static_cast<int>(std::size(kSliders)));
    return g_fails == 0;
}

} // namespace hbe::bodyshape
