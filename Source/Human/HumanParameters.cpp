#include "Human/HumanParameters.h"

#include "Core/Rng.h"

#include <algorithm>

namespace hbe::human {

const char* RegionName(Region r) {
    switch (r) {
    case Region::Pelvis: return "Pelvis";
    case Region::Abdomen: return "Abdomen";
    case Region::Chest: return "Chest";
    case Region::UpperBack: return "Upper back";
    case Region::Neck: return "Neck";
    case Region::Head: return "Head";
    case Region::ShoulderL: return "Shoulder (L)";
    case Region::UpperArmL: return "Upper arm (L)";
    case Region::ForearmL: return "Forearm (L)";
    case Region::HandL: return "Hand (L)";
    case Region::ShoulderR: return "Shoulder (R)";
    case Region::UpperArmR: return "Upper arm (R)";
    case Region::ForearmR: return "Forearm (R)";
    case Region::HandR: return "Hand (R)";
    case Region::ThighL: return "Thigh (L)";
    case Region::CalfL: return "Calf (L)";
    case Region::FootL: return "Foot (L)";
    case Region::ThighR: return "Thigh (R)";
    case Region::CalfR: return "Calf (R)";
    case Region::FootR: return "Foot (R)";
    default: return "?";
    }
}

bool RegionIsLeft(Region r) {
    switch (r) {
    case Region::ShoulderL: case Region::UpperArmL: case Region::ForearmL:
    case Region::HandL: case Region::ThighL: case Region::CalfL: case Region::FootL:
        return true;
    default: return false;
    }
}

Region MirrorRegion(Region r) {
    switch (r) {
    case Region::ShoulderL: return Region::ShoulderR;
    case Region::UpperArmL: return Region::UpperArmR;
    case Region::ForearmL: return Region::ForearmR;
    case Region::HandL:     return Region::HandR;
    case Region::ThighL:    return Region::ThighR;
    case Region::CalfL:     return Region::CalfR;
    case Region::FootL:     return Region::FootR;
    case Region::ShoulderR: return Region::ShoulderL;
    case Region::UpperArmR: return Region::UpperArmL;
    case Region::ForearmR: return Region::ForearmL;
    case Region::HandR:     return Region::HandL;
    case Region::ThighR:    return Region::ThighL;
    case Region::CalfR:     return Region::CalfL;
    case Region::FootR:     return Region::FootL;
    default: return r; // midline regions mirror to themselves
    }
}

u64 HumanParameters::ContentHash() const {
    Hasher h;
    // FIXED ORDER. Append new fields at the END and bump kGeneratorVersion; inserting one
    // in the middle silently changes every existing human's identity.
    h.Mix(kGeneratorVersion);
    h.Mix(seed);
    h.Mix(height);   h.Mix(weight);     h.Mix(muscleMass);
    h.Mix(bodyFat);  h.Mix(age);        h.Mix(dimorphism);
    h.Mix(asymmetry);

    h.Mix(body.legLength);   h.Mix(body.torsoLength); h.Mix(body.armLength);
    h.Mix(body.neckLength);  h.Mix(body.shoulderWidth); h.Mix(body.hipWidth);
    h.Mix(body.ribcageDepth);h.Mix(body.handSize);    h.Mix(body.footSize);
    h.Mix(body.headSize);

    h.Mix(muscle.shoulders); h.Mix(muscle.chest);   h.Mix(muscle.back);
    h.Mix(muscle.arms);      h.Mix(muscle.forearms);h.Mix(muscle.abdomen);
    h.Mix(muscle.glutes);    h.Mix(muscle.thighs);  h.Mix(muscle.calves);
    h.Mix(muscle.neck);

    h.Mix(fat.abdomen); h.Mix(fat.hips);   h.Mix(fat.glutes); h.Mix(fat.chest);
    h.Mix(fat.arms);    h.Mix(fat.thighs); h.Mix(fat.calves); h.Mix(fat.back);
    h.Mix(fat.submental); h.Mix(fat.face);

    h.Mix(face.skullLength); h.Mix(face.skullWidth); h.Mix(face.faceLength);
    h.Mix(face.jawWidth);    h.Mix(face.jawAngle);   h.Mix(face.chinProjection);
    h.Mix(face.cheekbone);   h.Mix(face.browRidge);  h.Mix(face.eyeSpacing);
    h.Mix(face.eyeSize);     h.Mix(face.noseLength); h.Mix(face.noseWidth);
    h.Mix(face.noseProjection); h.Mix(face.lipFullness); h.Mix(face.earSize);

    // The NAME is deliberately NOT hashed: renaming a human must not invalidate its bake.
    return h.Value();
}

void Sanitize(HumanParameters& p) {
    const auto n01 = [](f32& v) { v = std::clamp(v, 0.0f, 1.0f); };
    // Proportion multipliers get a generous but finite range. Wide enough for stylised
    // bodies, narrow enough that the field solver never sees a degenerate limb.
    const auto mul = [](f32& v) { v = std::clamp(v, 0.4f, 2.5f); };

    p.height = std::clamp(p.height, 0.60f, 2.60f);
    p.weight = std::clamp(p.weight, 15.0f, 300.0f);
    p.age = std::clamp(p.age, 1.0f, 120.0f);
    n01(p.muscleMass); n01(p.bodyFat); n01(p.dimorphism);
    p.asymmetry = std::clamp(p.asymmetry, 0.0f, 1.0f);

    mul(p.body.legLength);  mul(p.body.torsoLength); mul(p.body.armLength);
    mul(p.body.neckLength); mul(p.body.shoulderWidth); mul(p.body.hipWidth);
    mul(p.body.ribcageDepth); mul(p.body.handSize); mul(p.body.footSize);
    mul(p.body.headSize);

    for (f32* v : {&p.muscle.shoulders, &p.muscle.chest, &p.muscle.back, &p.muscle.arms,
                   &p.muscle.forearms, &p.muscle.abdomen, &p.muscle.glutes,
                   &p.muscle.thighs, &p.muscle.calves, &p.muscle.neck})
        *v = std::clamp(*v, 0.0f, 3.0f);

    for (f32* v : {&p.fat.abdomen, &p.fat.hips, &p.fat.glutes, &p.fat.chest, &p.fat.arms,
                   &p.fat.thighs, &p.fat.calves, &p.fat.back, &p.fat.submental, &p.fat.face})
        *v = std::clamp(*v, 0.0f, 3.0f);

    mul(p.face.skullLength); mul(p.face.skullWidth); mul(p.face.faceLength);
    mul(p.face.jawWidth);
    n01(p.face.jawAngle); n01(p.face.chinProjection); n01(p.face.cheekbone);
    n01(p.face.browRidge); n01(p.face.eyeSpacing); n01(p.face.eyeSize);
    n01(p.face.noseLength); n01(p.face.noseWidth); n01(p.face.noseProjection);
    n01(p.face.lipFullness); n01(p.face.earSize);

    if (p.seed == 0) p.seed = 1; // 0 is a valid state but reads as "unset"; make it explicit
}

} // namespace hbe::human
