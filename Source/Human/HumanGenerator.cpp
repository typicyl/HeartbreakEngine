#include "Human/HumanGenerator.h"

#include <chrono>
#include <cstdio>

namespace hbe::human {

namespace {
using clock = std::chrono::steady_clock;
f64 Since(clock::time_point t) { return std::chrono::duration<f64>(clock::now() - t).count(); }
} // namespace

GeneratedHuman HumanGenerator::Generate(const HumanParameters& p, const SurfaceSettings& s) {
    GeneratedHuman h;
    h.params = p;
    Sanitize(h.params);
    Regenerate(h, Stage::Anatomy, s);
    return h;
}

void HumanGenerator::Regenerate(GeneratedHuman& h, Stage from, const SurfaceSettings& s) {
    // Deliberately a fall-through chain: every stage invalidates the ones after it, and
    // writing it this way makes that ordering impossible to get wrong when stages are added.
    switch (from) {
    case Stage::Anatomy: {
        const auto t = clock::now();
        Sanitize(h.params);
        h.anatomy = Resolve(h.params);
        h.anatomySeconds = Since(t);
    }
        [[fallthrough]];
    case Stage::Field: {
        const auto t = clock::now();
        h.field.Build(h.anatomy);
        h.fieldSeconds = Since(t);
    }
        [[fallthrough]];
    case Stage::Surface: {
        const auto t = clock::now();
        h.surface = Extract(h.field, s);
        h.surfaceSeconds = Since(t);
    }
        [[fallthrough]];
    case Stage::Done:
        break;
    }
    h.contentHash = h.params.ContentHash();
    h.valid = !h.surface.mesh.vertices.empty() && !h.surface.mesh.indices.empty();
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("human FAIL: %s\n", what); ++g_fails; }
}
} // namespace

bool GeneratorSelfTest() {
    g_fails = 0;
    // The layers own their own proofs; this runs them so one flag covers the pipeline.
    if (!AnatomySelfTest()) ++g_fails;
    if (!BodyFieldSelfTest()) ++g_fails;
    if (!SurfaceSelfTest()) ++g_fails;

    SurfaceSettings s;
    s.resolution = 40;

    HumanParameters p;
    p.name = "Test";
    const GeneratedHuman a = HumanGenerator::Generate(p, s);
    Check(a.valid, "a default human must generate");
    Check(a.contentHash == p.ContentHash(), "the human must carry its own identity");

    // END-TO-END DETERMINISM, which is the promise the whole tool rests on.
    const GeneratedHuman b = HumanGenerator::Generate(p, s);
    Check(a.contentHash == b.contentHash, "the same parameters must produce the same identity");
    Check(a.surface.mesh.indices == b.surface.mesh.indices, "...and the same topology");
    bool same = a.surface.mesh.vertices.size() == b.surface.mesh.vertices.size();
    if (same)
        for (usize i = 0; i < a.surface.mesh.vertices.size(); ++i)
            if (a.surface.mesh.vertices[i].position != b.surface.mesh.vertices[i].position) {
                same = false;
                break;
            }
    Check(same, "SAME SEED, SAME HUMAN - bit-identical geometry, or nothing downstream can cache");

    // Regenerate from a later stage must give the same answer as regenerating from the top:
    // if it does not, the incremental path is a second implementation that will drift.
    {
        GeneratedHuman c = HumanGenerator::Generate(p, s);
        HumanGenerator::Regenerate(c, Stage::Field, s);
        Check(c.surface.mesh.indices == a.surface.mesh.indices,
              "re-running from the field must reproduce the same surface");
    }

    // A different human must actually be different.
    {
        HumanParameters q = p;
        q.height = 1.90f;
        q.muscle.arms = 2.0f;
        const GeneratedHuman d = HumanGenerator::Generate(q, s);
        Check(d.contentHash != a.contentHash, "different parameters must be a different human");
        Check(d.surface.mesh.vertices.size() != a.surface.mesh.vertices.size() ||
                  d.surface.mesh.vertices[0].position != a.surface.mesh.vertices[0].position,
              "different parameters must produce different geometry");
    }

    // Renaming must NOT invalidate a bake - the name is a label, not an input.
    {
        HumanParameters r = p;
        r.name = "Someone Else";
        Check(r.ContentHash() == p.ContentHash(),
              "renaming a human must not change its identity or invalidate its bake");
    }

    if (g_fails == 0)
        std::printf("human: parameters -> skeleton -> muscles -> fat -> field -> surface, "
                    "end to end, deterministic, %d verts in %.2fs\n",
                    static_cast<int>(a.surface.mesh.vertices.size()), a.TotalSeconds());
    return g_fails == 0;
}

} // namespace hbe::human
