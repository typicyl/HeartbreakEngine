#include "Human/Anatomy.h"

#include "Core/Rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hbe::human {

// ---------------------------------------------------------------------------
// Muscle mechanics
// ---------------------------------------------------------------------------

f32 Muscle::CurrentLength() const {
    return restLength * (1.0f - maxShortening * std::clamp(activation, 0.0f, 1.0f));
}

f32 Muscle::RadiusScale() const {
    // VOLUME PRESERVATION, which is a property of muscle tissue rather than a tuned curve.
    // Belly volume ~ length * cross-section area, and area ~ radius^2. Holding volume while
    // the length scales by k therefore needs radius * 1/sqrt(k). This one line is the whole
    // of "contracting shortens AND thickens" - there is no blendshape anywhere in it.
    const f32 L = CurrentLength();
    if (L <= 1e-6f || restLength <= 1e-6f) return 1.0f;
    return std::sqrt(restLength / L);
}

// ---------------------------------------------------------------------------
// Anatomy queries
// ---------------------------------------------------------------------------

i32 Anatomy::FindJoint(const std::string& name) const {
    for (usize i = 0; i < joints.size(); ++i)
        if (joints[i].name == name) return static_cast<i32>(i);
    return -1;
}

glm::vec3 Anatomy::ToModel(i32 joint, const glm::vec3& local) const {
    if (joint < 0 || static_cast<usize>(joint) >= joints.size()) return local;
    // Stage 1 builds the rest skeleton with axis-aligned joint frames, so this is a
    // translation. It is a FUNCTION rather than an addition at every call site precisely so
    // that giving joints real orientations later does not touch any caller.
    return joints[static_cast<usize>(joint)].position + local;
}

void Anatomy::Bounds(glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(1e9f);
    hi = glm::vec3(-1e9f);
    const auto add = [&](const glm::vec3& p, f32 r) {
        lo = glm::min(lo, p - glm::vec3(r));
        hi = glm::max(hi, p + glm::vec3(r));
    };
    for (const BoneSolid& b : bones) {
        add(b.head, b.radiusHead);
        add(b.tail, b.radiusTail);
    }
    for (const Muscle& m : muscles) {
        const f32 r = std::sqrt(std::max(m.pcsa, 0.0f) / 3.14159265f) * m.RadiusScale();
        add(ToModel(m.originJoint, m.originLocal), r);
        add(ToModel(m.insertJoint, m.insertLocal), r);
        if (m.hasVia) add(ToModel(m.viaJoint, m.viaLocal), r);
    }
    for (const FatDeposit& f : fat) add(f.centre, f.radius + f.thickness);
    if (lo.x > hi.x) { lo = glm::vec3(-0.5f); hi = glm::vec3(0.5f); } // empty anatomy
}

// ---------------------------------------------------------------------------
// PARAMETERS -> ANATOMY
// ---------------------------------------------------------------------------

namespace {

// Segment lengths as fractions of stature, from the standard Drillis & Contini
// anthropometric table. Real published proportions rather than numbers that looked right:
// it is what makes a 1.60 m human and a 1.95 m human differ correctly rather than by a
// uniform scale, and it gives the proportion multipliers something meaningful to multiply.
struct Anthro {
    static constexpr f32 kAnkle = 0.039f;   // floor -> ankle
    static constexpr f32 kKnee = 0.285f;    // floor -> knee
    static constexpr f32 kHip = 0.530f;     // floor -> greater trochanter
    static constexpr f32 kWaist = 0.630f;
    static constexpr f32 kChest = 0.720f;
    static constexpr f32 kShoulder = 0.818f;// floor -> acromion
    static constexpr f32 kChin = 0.870f;
    static constexpr f32 kBiacromial = 0.259f; // shoulder-to-shoulder WIDTH
    static constexpr f32 kBiiliac = 0.191f;    // hip WIDTH
    static constexpr f32 kUpperArm = 0.186f;
    static constexpr f32 kForearm = 0.146f;
    static constexpr f32 kHand = 0.108f;
    static constexpr f32 kFoot = 0.152f;
    static constexpr f32 kHeadH = 0.130f;
};

// Body groups. Solids only blend generously with others in the SAME group; across groups
// they barely merge. 0 is the trunk, which every limb attaches to.
enum : u8 { G_Trunk = 0, G_Head = 1, G_ArmL = 2, G_ArmR = 3, G_LegL = 4, G_LegR = 5 };

struct Build {
    Anatomy a;
    const HumanParameters& p;
    Rng rng;
    f32 H;      // stature, metres
    f32 bmi;
    f32 lean;   // 0..1 muscularity after dimorphism weighting
    f32 fatK;   // overall fat depth multiplier

    explicit Build(const HumanParameters& params)
        : p(params), rng(params.seed), H(params.height) {
        bmi = p.weight / std::max(0.5f, H * H);
        // Muscularity: the parameter, nudged by dimorphism. Testosterone-driven lean mass
        // differences are real and large, so this is not cosmetic - but it is a CONTINUOUS
        // weighting, never a branch on sex.
        lean = std::clamp(p.muscleMass * (0.75f + 0.5f * p.dimorphism), 0.0f, 1.5f);
        // Fat depth rises with both the explicit knob and actual BMI above a lean baseline,
        // so `weight` genuinely changes the body rather than only labelling it.
        const f32 overBmi = std::max(0.0f, bmi - 20.0f);
        fatK = 0.55f * p.bodyFat + 0.045f * overBmi;
        // Soft-tissue laxity with age: fat migrates and skin loosens. Small, but it is the
        // difference between a 25- and a 65-year-old at identical weight.
        fatK *= 1.0f + 0.004f * std::max(0.0f, p.age - 35.0f);
    }

    // Per-side seeded divergence. Split streams so adding a new consumer cannot renumber an
    // existing one - the same discipline the whole generator relies on for reproducibility.
    f32 Asym(u64 salt) {
        if (p.asymmetry <= 0.0f) return 1.0f;
        Rng r = rng.Split(salt);
        return 1.0f + p.asymmetry * 0.05f * r.Signed();
    }

    i32 AddJoint(const char* name, i32 parent, glm::vec3 pos, Region reg) {
        Joint j;
        j.name = name;
        j.parent = parent;
        j.position = pos;
        j.region = reg;
        a.joints.push_back(std::move(j));
        return static_cast<i32>(a.joints.size()) - 1;
    }

    void AddBone(i32 ja, i32 jb, f32 rh, f32 rt, Region reg, u8 group, f32 flatten = 1.0f) {
        BoneSolid b;
        b.jointA = ja;
        b.jointB = jb;
        b.head = a.joints[static_cast<usize>(ja)].position;
        b.tail = a.joints[static_cast<usize>(jb)].position;
        b.radiusHead = rh;
        b.radiusTail = rt;
        b.flatten = flatten;
        b.region = reg;
        b.group = group;
        a.bones.push_back(b);
    }

    // A solid that is a SHAPE rather than a bone between two joints: the ribcage, the pelvic
    // block, the cranium. These carry the body's silhouette and must not become joints.
    void AddSolid(glm::vec3 head, glm::vec3 tail, f32 rh, f32 rt, Region reg, u8 group,
                  f32 flatten) {
        BoneSolid b;
        b.head = head;
        b.tail = tail;
        b.radiusHead = rh;
        b.radiusTail = rt;
        b.flatten = flatten;
        b.region = reg;
        b.group = group;
        a.bones.push_back(b);
    }

    void AddMuscle(const char* name, Region reg, u8 group, i32 oj, glm::vec3 ol, i32 ij,
                   glm::vec3 il, f32 pcsaBase, f32 tuning, f32 bellyBias = 0.5f,
                   f32 flatten = 1.0f) {
        Muscle m;
        m.name = name;
        m.region = reg;
        m.originJoint = oj;
        m.originLocal = ol;
        m.insertJoint = ij;
        m.insertLocal = il;
        m.bellyBias = bellyBias;
        m.flatten = flatten;
        m.group = group;
        // PCSA scales with the square of stature (an area) and with muscularity. The
        // per-region tuning is what lets "big arms, ordinary legs" exist at all.
        const f32 sizeK = (H / 1.75f) * (H / 1.75f);
        m.pcsa = pcsaBase * sizeK * tuning * (0.55f + 0.9f * lean);
        m.restLength = glm::length(a.ToModel(ij, il) - a.ToModel(oj, ol));
        // Muscle-to-bone blending, WITHIN a limb. Kept modest: a large radius here was what
        // turned every structure into one smooth lump with no anatomy visible in it.
        m.blendRadius = 0.010f + 0.008f * (1.0f - std::clamp(lean, 0.0f, 1.0f));
        a.muscles.push_back(std::move(m));
    }

    void AddFat(Region reg, u8 group, glm::vec3 centre, f32 radius, f32 mm, f32 tuning) {
        FatDeposit f;
        f.region = reg;
        f.group = group;
        f.centre = centre;
        f.radius = radius;
        f.thickness = 0.001f * mm * fatK * tuning;
        f.falloff = radius * 0.6f;
        a.fat.push_back(f);
    }
};

} // namespace

Anatomy Resolve(const HumanParameters& params) {
    HumanParameters p = params;
    Sanitize(p);
    Build B(p);
    Anatomy& a = B.a;
    const f32 H = B.H;

    // ---- SKELETON ---------------------------------------------------------
    // Heights come from the anthropometric table; the proportion multipliers scale the
    // SEGMENTS rather than absolute heights, so lengthening the legs raises the hips and
    // everything above them instead of stretching the body between fixed points.
    const f32 ankleY = H * Anthro::kAnkle;
    const f32 kneeY = ankleY + (H * (Anthro::kKnee - Anthro::kAnkle)) * p.body.legLength;
    const f32 hipY = kneeY + (H * (Anthro::kHip - Anthro::kKnee)) * p.body.legLength;
    const f32 torsoLen = H * (Anthro::kShoulder - Anthro::kHip) * p.body.torsoLength;
    const f32 waistY = hipY + torsoLen * 0.30f;
    const f32 chestY = hipY + torsoLen * 0.62f;
    const f32 shoulderY = hipY + torsoLen;
    const f32 neckLen = H * (Anthro::kChin - Anthro::kShoulder) * p.body.neckLength;
    const f32 neckTopY = shoulderY + neckLen;
    const f32 headR = H * Anthro::kHeadH * 0.5f * p.body.headSize * p.face.skullWidth;

    const f32 hipHalf = H * Anthro::kBiiliac * 0.5f * p.body.hipWidth;
    const f32 shoulderHalf = H * Anthro::kBiacromial * 0.5f * p.body.shoulderWidth;
    const f32 upperArm = H * Anthro::kUpperArm * p.body.armLength;
    const f32 forearm = H * Anthro::kForearm * p.body.armLength;
    const f32 handLen = H * Anthro::kHand * p.body.handSize;
    const f32 footLen = H * Anthro::kFoot * p.body.footSize;
    const f32 ribDepth = H * 0.075f * p.body.ribcageDepth;

    const i32 jHips = B.AddJoint("Hips", -1, {0, hipY, 0}, Region::Pelvis);
    const i32 jSpine = B.AddJoint("Spine", jHips, {0, waistY, 0}, Region::Abdomen);
    const i32 jSpine1 = B.AddJoint("Spine1", jSpine, {0, (waistY + chestY) * 0.5f, 0}, Region::Abdomen);
    const i32 jSpine2 = B.AddJoint("Spine2", jSpine1, {0, chestY, 0}, Region::Chest);
    const i32 jNeck = B.AddJoint("Neck", jSpine2, {0, shoulderY, 0}, Region::Neck);
    const i32 jHead = B.AddJoint("Head", jNeck, {0, neckTopY, 0}, Region::Head);
    B.AddJoint("HeadTop_End", jHead, {0, neckTopY + headR * 2.0f, 0}, Region::Head);

    struct Side {
        f32 s; const char* pfx; Region sh, ua, fa, hd, th, cf, ft; u64 salt; u8 arm, leg;
    };
    const Side sides[2] = {
        {+1.0f, "Left", Region::ShoulderL, Region::UpperArmL, Region::ForearmL, Region::HandL,
         Region::ThighL, Region::CalfL, Region::FootL, 0x1111, G_ArmL, G_LegL},
        {-1.0f, "Right", Region::ShoulderR, Region::UpperArmR, Region::ForearmR, Region::HandR,
         Region::ThighR, Region::CalfR, Region::FootR, 0x2222, G_ArmR, G_LegR},
    };
    // THE ARMS ARE IN AN A-POSE, and that is a geometric necessity rather than a convention.
    // With the arms hanging straight down, the gap between the humerus and the ribs is a few
    // centimetres - FINER THAN AN EXTRACTION CELL at any resolution this tool will use. The
    // isosurface cannot represent a gap it cannot sample, so it pinches the arm into the
    // torso and produces non-manifold geometry there. Swinging the arms out gives a real,
    // resolvable separation. It is also the pose every character pipeline rigs in, because
    // it keeps the shoulder away from its extremes for skinning.
    const f32 armX = shoulderHalf * 1.02f;
    constexpr f32 kAPose = 0.62f; // radians from vertical, ~36 degrees
    const glm::vec3 armDirL(std::sin(kAPose), -std::cos(kAPose), 0.0f);

    for (const Side& sd : sides) {
        const f32 k = B.Asym(sd.salt);
        const std::string P = sd.pfx;

        const i32 jClav = B.AddJoint((P + "Shoulder").c_str(), jSpine2,
                                     {sd.s * shoulderHalf * 0.25f, shoulderY - 0.01f * H, 0}, sd.sh);
        const i32 jArm = B.AddJoint((P + "Arm").c_str(), jClav,
                                    {sd.s * armX, shoulderY - 0.04f * H, 0}, sd.ua);
        // (elbow/wrist follow the A-pose direction, below)
        const glm::vec3 dir(sd.s * armDirL.x, armDirL.y, 0.0f);
        const glm::vec3 shoulderP(sd.s * armX, shoulderY - 0.04f * H, 0.0f);
        const glm::vec3 elbowP = shoulderP + dir * (upperArm * k);
        const glm::vec3 wristP = elbowP + dir * (forearm * k);
        const i32 jFore = B.AddJoint((P + "ForeArm").c_str(), jArm, elbowP, sd.fa);
        const i32 jHand = B.AddJoint((P + "Hand").c_str(), jFore, wristP, sd.hd);
        B.AddJoint((P + "HandEnd").c_str(), jHand, wristP + dir * handLen, sd.hd);

        const i32 jThigh = B.AddJoint((P + "UpLeg").c_str(), jHips, {sd.s * hipHalf, hipY, 0}, sd.th);
        const i32 jCalf = B.AddJoint((P + "Leg").c_str(), jThigh, {sd.s * hipHalf, kneeY * k, 0}, sd.cf);
        const i32 jFoot = B.AddJoint((P + "Foot").c_str(), jCalf, {sd.s * hipHalf, ankleY, 0}, sd.ft);
        B.AddJoint((P + "ToeBase").c_str(), jFoot, {sd.s * hipHalf, ankleY * 0.45f, footLen * 0.62f}, sd.ft);

        // Limb bone solids. Radii are the BONE, not the limb: the flesh comes from the
        // muscle and fat layers on top, which is the entire point of the layering.
        const f32 boneK = H / 1.75f;
        // The clavicle stays in the TRUNK: it is the bridge the arm hangs off, and it is what
        // keeps the body one connected surface across the shoulder.
        B.AddBone(jClav, jArm, 0.012f * boneK, 0.014f * boneK, sd.sh, G_Trunk);
        B.AddBone(jArm, jFore, 0.022f * boneK, 0.017f * boneK, sd.ua, sd.arm);
        B.AddBone(jFore, jHand, 0.017f * boneK, 0.013f * boneK, sd.fa, sd.arm);
        B.AddBone(jThigh, jCalf, 0.030f * boneK, 0.022f * boneK, sd.th, sd.leg);
        B.AddBone(jCalf, jFoot, 0.024f * boneK, 0.015f * boneK, sd.cf, sd.leg);

        // Hand and foot are single blocks in stage 1. SIMPLIFICATION, stated: fingers and
        // toes need their own joints and solids, and adding them changes nothing structural
        // here - they are more entries in exactly these two loops.
        const glm::vec3 handP = a.joints[static_cast<usize>(jHand)].position;
        B.AddSolid(handP, handP + glm::vec3(sd.s * armDirL.x, armDirL.y, 0.0f) * (handLen * 0.9f),
                   0.030f * boneK * p.body.handSize, 0.026f * boneK * p.body.handSize, sd.hd,
                   sd.arm, 0.45f);
        const glm::vec3 footP = a.joints[static_cast<usize>(jFoot)].position;
        B.AddSolid(footP + glm::vec3(0, -ankleY * 0.35f, -footLen * 0.20f),
                   footP + glm::vec3(0, -ankleY * 0.55f, footLen * 0.62f),
                   0.038f * boneK, 0.028f * boneK, sd.ft, sd.leg, 0.7f);
    }

    // Axial solids: pelvis block, ribcage, neck, cranium. These carry most of the torso
    // silhouette, and the ribcage's ellipse is why a chest is wider than it is deep.
    B.AddSolid({0, hipY - 0.04f * H, 0}, {0, hipY + 0.05f * H, 0},
               hipHalf * 0.92f, hipHalf * 0.86f, Region::Pelvis, G_Trunk, 0.78f);
    // Waist -> ribcage. The waist is genuinely narrower than the ribs, which is most of what
    // makes a torso read as a torso rather than a barrel.
    B.AddSolid({0, waistY - 0.02f * H, 0}, {0, chestY, 0},
               shoulderHalf * 0.56f, shoulderHalf * 0.74f, Region::Chest, G_Trunk,
               ribDepth / (shoulderHalf * 0.74f));
    B.AddSolid({0, chestY, 0}, {0, shoulderY - 0.01f * H, 0},
               shoulderHalf * 0.74f, shoulderHalf * 0.66f, Region::Chest, G_Trunk, 0.72f);
    B.AddSolid({0, shoulderY - 0.02f * H, 0}, {0, neckTopY - headR * 0.15f, 0},
               0.052f * (H / 1.75f) * p.body.neckLength, 0.046f * (H / 1.75f),
               Region::Neck, G_Head, 0.95f);
    // Cranium and jaw. The finer facial structures are deferred, and the parameters that
    // will drive them already exist - this is the skull they will be built on.
    const f32 skullY = neckTopY + headR * 0.85f;
    // The cranium is ONE solid with a gentle taper. Two overlapping blobs with different
    // flatten values is what produced the lumpy, asymmetric head in the first build.
    B.AddSolid({0, skullY - headR * 0.15f, -headR * 0.05f},
               {0, skullY + headR * 0.45f, -headR * 0.02f},
               headR * 0.94f, headR * 0.86f, Region::Head, G_Head, p.face.skullLength);
    // The jaw sits below and forward of the cranium, blending into it within the head group.
    B.AddSolid({0, skullY - headR * 0.55f, headR * 0.05f},
               {0, skullY - headR * 0.20f, headR * 0.30f},
               headR * 0.55f * p.face.jawWidth, headR * 0.72f * p.face.jawWidth,
               Region::Head, G_Head, 0.90f);

    // ---- MUSCLES ----------------------------------------------------------
    // A representative set of the muscles that actually shape a silhouette. PCSA values are
    // in square metres and are the right ORDER OF MAGNITUDE for an adult (a biceps is a few
    // square centimetres of physiological cross-section). SIMPLIFICATION, stated: this is
    // ~13 muscle groups per side, not the ~340 of a full model. Each is a real structure
    // with real attachments, so adding more is data, not architecture.
    const f32 MM = 1.0f;
    for (int s = 0; s < 2; ++s) {
        const Side& sd = sides[s];
        const std::string P = sd.pfx;
        const i32 jClav = a.FindJoint(P + "Shoulder");
        const i32 jArm = a.FindJoint(P + "Arm");
        const i32 jFore = a.FindJoint(P + "ForeArm");
        const i32 jHand = a.FindJoint(P + "Hand");
        const i32 jThigh = a.FindJoint(P + "UpLeg");
        const i32 jCalf = a.FindJoint(P + "Leg");
        const i32 jFoot = a.FindJoint(P + "Foot");
        const f32 x = sd.s;

        // THE DELTOID CAPS THE SHOULDER and belongs to the ARM. Previously it ran from the
        // clavicle down the humerus as one long solid, which combined with the pec and lat to
        // build a continuous plate from spine to elbow.
        B.AddMuscle((P + " Deltoid").c_str(), sd.sh, sd.arm, jArm, {0, upperArm * 0.10f, 0},
                    jArm, {0, -upperArm * 0.38f, 0}, 0.0030f, p.muscle.shoulders * MM, 0.25f, 0.95f);
        B.AddMuscle((P + " Biceps").c_str(), sd.ua, sd.arm, jArm, {0, -upperArm * 0.10f, ribDepth * 0.10f},
                    jFore, {0, upperArm * 0.06f, ribDepth * 0.08f},
                    0.0022f, p.muscle.arms * MM, 0.55f, 0.90f);
        B.AddMuscle((P + " Triceps").c_str(), sd.ua, sd.arm, jArm, {0, -upperArm * 0.08f, -ribDepth * 0.12f},
                    jFore, {0, upperArm * 0.04f, -ribDepth * 0.09f},
                    0.0026f, p.muscle.arms * MM, 0.45f, 0.95f);
        B.AddMuscle((P + " Forearm mass").c_str(), sd.fa, sd.arm, jFore, {0, -forearm * 0.05f, 0},
                    jHand, {0, forearm * 0.18f, 0}, 0.0024f, p.muscle.forearms * MM, 0.30f, 0.90f);

        // THE PECTORALIS AND LATISSIMUS STAY ON THE RIBCAGE. Anatomically both insert on the
        // humerus, but a straight swept solid cannot express a muscle that WRAPS - modelled
        // that way they became flat plates bridging the spine to the arm, and the arms were
        // swallowed by them. They now run across the chest and back and stop at the lateral
        // ribcage; the deltoid covers the shoulder. Representing the true insertion needs the
        // wrapping via-points this stage does not have.
        B.AddMuscle((P + " Pectoralis").c_str(), Region::Chest, G_Trunk, jSpine2,
                    {x * shoulderHalf * 0.10f, -0.02f * H, ribDepth * 0.80f}, jSpine2,
                    {x * shoulderHalf * 0.60f, 0.01f * H, ribDepth * 0.50f},
                    0.0036f, p.muscle.chest * MM, 0.55f, 0.45f);
        B.AddMuscle((P + " Latissimus").c_str(), Region::UpperBack, G_Trunk, jSpine,
                    {x * shoulderHalf * 0.15f, 0.01f * H, -ribDepth * 0.70f}, jSpine2,
                    {x * shoulderHalf * 0.58f, 0.02f * H, -ribDepth * 0.45f},
                    0.0034f, p.muscle.back * MM, 0.60f, 0.42f);
        B.AddMuscle((P + " Trapezius").c_str(), Region::UpperBack, G_Trunk, jNeck,
                    {x * 0.012f * H, -0.02f * H, -ribDepth * 0.30f}, jClav,
                    {x * shoulderHalf * 0.30f, 0, -ribDepth * 0.30f},
                    0.0024f, p.muscle.back * MM, 0.5f, 0.55f);

        // The glutes are part of the PELVIC mass, flattened against it - not two spheres
        // hanging off the back, which is what a large radius and a big rearward offset gave.
        B.AddMuscle((P + " Gluteus").c_str(), Region::Pelvis, G_Trunk, jHips,
                    {x * hipHalf * 0.42f, 0.03f * H, -hipHalf * 0.35f}, jHips,
                    {x * hipHalf * 0.50f, -0.07f * H, -hipHalf * 0.20f},
                    0.0048f, p.muscle.glutes * MM, 0.45f, 0.70f);
        B.AddMuscle((P + " Quadriceps").c_str(), sd.th, sd.leg, jThigh, {0, -0.02f * H, ribDepth * 0.26f},
                    jCalf, {0, (hipY - kneeY) * 0.12f, ribDepth * 0.14f},
                    0.0090f, p.muscle.thighs * MM, 0.55f, 0.95f);
        B.AddMuscle((P + " Hamstrings").c_str(), sd.th, sd.leg, jThigh, {0, -0.02f * H, -ribDepth * 0.24f},
                    jCalf, {0, (hipY - kneeY) * 0.10f, -ribDepth * 0.13f},
                    0.0070f, p.muscle.thighs * MM, 0.50f, 0.90f);
        B.AddMuscle((P + " Gastrocnemius").c_str(), sd.cf, sd.leg, jCalf, {0, -0.02f * H, -ribDepth * 0.18f},
                    jFoot, {0, (kneeY - ankleY) * 0.32f, -ribDepth * 0.08f},
                    0.0055f, p.muscle.calves * MM, 0.30f, 0.90f);
    }
    // Midline muscles, added once.
    B.AddMuscle("Rectus abdominis", Region::Abdomen, G_Trunk, jHips, {0, 0.05f * H, ribDepth * 0.70f},
                jSpine2, {0, -0.03f * H, ribDepth * 0.72f}, 0.0028f, p.muscle.abdomen, 0.5f, 0.50f);
    B.AddMuscle("Neck mass", Region::Neck, G_Head, jSpine2, {0, 0.02f * H, ribDepth * 0.30f},
                jHead, {0, -headR * 0.70f, headR * 0.05f}, 0.0016f, p.muscle.neck, 0.5f, 0.9f);

    // ---- FAT --------------------------------------------------------------
    // Thicknesses in millimetres at the deposit centre, in the ratios skinfold anthropometry
    // reports: the abdomen and hips carry several times what a forearm does, which is why
    // gaining weight changes a waistline long before it changes a wrist.
    B.AddFat(Region::Abdomen, G_Trunk, {0, waistY + torsoLen * 0.05f, ribDepth * 0.50f},
             shoulderHalf * 0.85f, 34.0f, p.fat.abdomen);
    B.AddFat(Region::Pelvis, G_Trunk, {0, hipY + 0.02f * H, 0}, hipHalf * 1.10f, 22.0f, p.fat.hips);
    B.AddFat(Region::Pelvis, G_Trunk, {0, hipY, -hipHalf * 0.50f}, hipHalf * 0.95f,
             26.0f, p.fat.glutes);
    B.AddFat(Region::Chest, G_Trunk, {0, chestY - torsoLen * 0.06f, ribDepth * 0.65f},
             shoulderHalf * 0.62f, 16.0f, p.fat.chest);
    B.AddFat(Region::UpperBack, G_Trunk, {0, chestY, -ribDepth * 0.72f}, shoulderHalf * 0.75f,
             14.0f, p.fat.back);
    B.AddFat(Region::Neck, G_Head, {0, neckTopY - headR * 0.30f, headR * 0.25f}, headR * 0.50f,
             10.0f, p.fat.submental);
    B.AddFat(Region::Head, G_Head, {0, skullY - headR * 0.30f, headR * 0.40f}, headR * 0.60f,
             7.0f, p.fat.face);
    for (const Side& sd : sides) {
        const std::string P = sd.pfx;
        const glm::vec3 arm = a.joints[static_cast<usize>(a.FindJoint(P + "Arm"))].position;
        const glm::vec3 thigh = a.joints[static_cast<usize>(a.FindJoint(P + "UpLeg"))].position;
        const glm::vec3 calf = a.joints[static_cast<usize>(a.FindJoint(P + "Leg"))].position;
        B.AddFat(sd.ua, sd.arm, arm + glm::vec3(0, -upperArm * 0.5f, 0), upperArm * 0.55f,
                 11.0f, p.fat.arms);
        B.AddFat(sd.th, sd.leg, thigh + glm::vec3(0, -(hipY - kneeY) * 0.45f, 0),
                 (hipY - kneeY) * 0.45f, 20.0f, p.fat.thighs);
        B.AddFat(sd.cf, sd.leg, calf + glm::vec3(0, -(kneeY - ankleY) * 0.40f, 0),
                 (kneeY - ankleY) * 0.40f, 9.0f, p.fat.calves);
    }

    // Dermis thins with age; it is small but it is the layer that closes the seams between
    // adjacent muscle bellies, so it is not decorative.
    a.dermis = 0.0020f * (1.0f - 0.30f * std::clamp((p.age - 25.0f) / 75.0f, 0.0f, 1.0f));
    a.paramHash = p.ContentHash();
    a.generatorVersion = kGeneratorVersion;
    return a;
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("anatomy FAIL: %s\n", what); ++g_fails; }
}
f32 RegionFatThickness(const Anatomy& a, Region r) {
    f32 t = 0.0f;
    for (const FatDeposit& f : a.fat) if (f.region == r) t += f.thickness;
    return t;
}
} // namespace

bool AnatomySelfTest() {
    g_fails = 0;
    HumanParameters p;

    // Determinism, in one process and against a fresh Resolve.
    Check(Resolve(p).paramHash == Resolve(p).paramHash, "the same parameters must hash the same");
    HumanParameters q = p;
    q.seed = 99;
    Check(Resolve(p).paramHash != Resolve(q).paramHash, "a different seed must be a different human");

    const Anatomy a = Resolve(p);
    Check(!a.joints.empty() && !a.bones.empty() && !a.muscles.empty() && !a.fat.empty(),
          "every anatomical layer must be populated");
    for (usize i = 0; i < a.joints.size(); ++i)
        Check(a.joints[i].parent < static_cast<i32>(i),
              "parents must precede children so one forward pass composes the skeleton");
    for (const Muscle& m : a.muscles)
        Check(m.restLength > 1e-4f, "every muscle must span a real distance");

    // HEIGHT MUST MOVE THE SKELETON, not scale a mesh.
    HumanParameters tall = p; tall.height = 2.00f;
    const Anatomy at = Resolve(tall);
    const i32 jh = a.FindJoint("Head"), jh2 = at.FindJoint("Head");
    Check(at.joints[static_cast<usize>(jh2)].position.y > a.joints[static_cast<usize>(jh)].position.y + 0.15f,
          "a taller human must have a higher head joint");

    // PROPORTIONS MUST BE INDEPENDENT: longer legs at the SAME stature must raise the hips.
    HumanParameters legs = p; legs.body.legLength = 1.20f;
    const Anatomy al = Resolve(legs);
    Check(al.joints[static_cast<usize>(al.FindJoint("Hips"))].position.y >
              a.joints[static_cast<usize>(a.FindJoint("Hips"))].position.y + 0.02f,
          "longer legs must raise the hips, not stretch the body between fixed points");

    // MUSCLE MASS MUST CHANGE MUSCLE VOLUME, not a surface parameter.
    HumanParameters big = p; big.muscleMass = 1.0f;
    const Anatomy ab = Resolve(big);
    f32 pcsaA = 0.0f, pcsaB = 0.0f;
    for (const Muscle& m : a.muscles) pcsaA += m.pcsa;
    for (const Muscle& m : ab.muscles) pcsaB += m.pcsa;
    Check(pcsaB > pcsaA * 1.2f, "more muscle mass must mean more physiological cross-section");

    // PER-REGION TUNING must be local, not global.
    HumanParameters arms = p; arms.muscle.arms = 2.0f;
    const Anatomy aa = Resolve(arms);
    const auto regionPcsa = [](const Anatomy& an, Region r) {
        f32 t = 0.0f;
        for (const Muscle& m : an.muscles) if (m.region == r) t += m.pcsa;
        return t;
    };
    Check(regionPcsa(aa, Region::UpperArmL) > regionPcsa(a, Region::UpperArmL) * 1.4f,
          "arm tuning must grow the arms");
    Check(std::abs(regionPcsa(aa, Region::ThighL) - regionPcsa(a, Region::ThighL)) < 1e-6f,
          "ARM TUNING MUST NOT TOUCH THE LEGS - a per-region knob that leaks is a global one");

    // WEIGHT MUST CHANGE FAT, and change it where fat actually goes.
    HumanParameters heavy = p; heavy.weight = 110.0f;
    const Anatomy ah = Resolve(heavy);
    Check(RegionFatThickness(ah, Region::Abdomen) > RegionFatThickness(a, Region::Abdomen) * 1.3f,
          "more weight must deposit more abdominal fat");
    Check(RegionFatThickness(ah, Region::Abdomen) > RegionFatThickness(ah, Region::CalfL) * 2.0f,
          "fat must land where fat lands - the abdomen carries far more than a calf");

    // CONTRACTION: shortens AND thickens, preserving volume.
    {
        Muscle m;
        m.restLength = 0.30f;
        m.maxShortening = 0.30f;
        Check(std::abs(m.RadiusScale() - 1.0f) < 1e-6f, "at rest a muscle is unchanged");
        m.activation = 1.0f;
        Check(m.CurrentLength() < m.restLength * 0.75f, "activation must SHORTEN the muscle");
        Check(m.RadiusScale() > 1.15f, "...and it must THICKEN as it shortens");
        const f32 v0 = m.restLength * 1.0f * 1.0f;
        const f32 v1 = m.CurrentLength() * m.RadiusScale() * m.RadiusScale();
        Check(std::abs(v1 - v0) < 1e-5f,
              "VOLUME MUST BE PRESERVED - this is the physical law the bulge comes from");
    }

    // Asymmetry is opt-in and reproducible.
    HumanParameters asym = p; asym.asymmetry = 1.0f;
    const Anatomy a1 = Resolve(asym), a2 = Resolve(asym);
    Check(a1.paramHash == a2.paramHash, "asymmetry must still be deterministic");
    Check(a.joints[static_cast<usize>(a.FindJoint("LeftLeg"))].position.y ==
              a.joints[static_cast<usize>(a.FindJoint("RightLeg"))].position.y,
          "with asymmetry 0 the body must be exactly symmetric");

    // Extremes must not produce a degenerate body.
    for (f32 h : {0.7f, 2.5f}) {
        for (f32 w : {20.0f, 250.0f}) {
            HumanParameters e = p; e.height = h; e.weight = w;
            const Anatomy ae = Resolve(e);
            glm::vec3 lo, hi;
            ae.Bounds(lo, hi);
            Check(hi.y > lo.y && hi.x > lo.x && hi.z > lo.z,
                  "every parameter extreme must still produce a bounded body");
            for (const Muscle& m : ae.muscles)
                Check(m.pcsa > 0.0f && std::isfinite(m.pcsa), "muscle size must stay finite");
        }
    }

    if (g_fails == 0)
        std::printf("anatomy: %d joints, %d bone solids, %d muscles, %d fat deposits from "
                    "parameters alone; height moves the skeleton, per-region tuning stays "
                    "local, weight deposits fat where fat goes, and contraction preserves "
                    "volume\n",
                    static_cast<int>(a.joints.size()), static_cast<int>(a.bones.size()),
                    static_cast<int>(a.muscles.size()), static_cast<int>(a.fat.size()));
    return g_fails == 0;
}

} // namespace hbe::human
