// Human/HumanGenerator.h - the one entry point: parameters in, a complete human out.
//
//     HumanParameters p;
//     p.height = 1.82f;
//     p.muscle.arms = 1.6f;
//     GeneratedHuman h = HumanGenerator::Generate(p);
//
// The result is the AUTHORING MASTER: the anatomy, the field it composes to, and the surface
// extracted from it, all retained. That is deliberate and it is the difference between an
// authoring tool and a mesh spitter - the human stays editable *as a human*, because the
// thing that produced the mesh is still there to be adjusted and re-run.
//
// The runtime asset is COMPILED from this later, and is a different, smaller thing: mesh,
// skeleton, weights, LODs, materials. The anatomy graph does not need to ship with a game.
#pragma once

#include "Human/Anatomy.h"
#include "Human/BodyField.h"
#include "Human/HumanParameters.h"
#include "Human/SurfaceGen.h"

namespace hbe::human {

// What generation stage produced what, so a UI can show progress and a caller can re-run
// only the part that a given edit invalidated.
enum class Stage : u8 { Anatomy, Field, Surface, Done };

struct GeneratedHuman {
    HumanParameters params;
    Anatomy anatomy;
    BodyField field;
    GeneratedSurface surface;

    u64 contentHash = 0;   // identity: the cache key, and what "unchanged" means
    f64 anatomySeconds = 0.0;
    f64 fieldSeconds = 0.0;
    f64 surfaceSeconds = 0.0;
    bool valid = false;

    f64 TotalSeconds() const { return anatomySeconds + fieldSeconds + surfaceSeconds; }
};

class HumanGenerator {
public:
    // The whole pipeline. Deterministic: identical parameters give an identical human, on
    // any machine, at any thread count.
    static GeneratedHuman Generate(const HumanParameters& p, const SurfaceSettings& s = {});

    // Re-run only what a parameter change actually invalidated. Editing a slider changes the
    // anatomy and therefore everything after it; posing a muscle for inspection changes only
    // the field and the surface. Keeping this explicit is what makes interactive editing
    // affordable without pretending the surface is cheap.
    static void Regenerate(GeneratedHuman& h, Stage from, const SurfaceSettings& s = {});
};

bool GeneratorSelfTest(); // --test-human

} // namespace hbe::human
