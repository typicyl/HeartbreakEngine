// Cinematics/CinematicsTest.cpp - headless self-tests (--test-curve, --test-cinematics).
#include "Cinematics/CinematicsTest.h"

#include "Cinematics/Evaluator.h"
#include "Cinematics/Sequence.h"
#include "Cinematics/SequenceAsset.h"
#include "Cinematics/TrackRegistry.h"
#include "Core/Curve.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <cmath>

namespace hbe::cine {
namespace {
bool Approx(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
f32 SecFloat(const Section& s, const char* key, f32 def = 0.0f) {
    for (const auto& p : s.floatParams)
        if (p.first == key) return p.second;
    return def;
}
} // namespace

bool CurveSelfTest() {
    int fails = 0;
    auto CHECK = [&](bool c, const char* msg) {
        if (!c) { HBE_ERROR("  [FAIL] curve: {}", msg); ++fails; }
    };

    // Linear interpolation + endpoints.
    {
        curve::Curve c;
        curve::Insert(c, 0.0f, 0.0f);
        curve::Insert(c, 2.0f, 10.0f);
        c.keys[0].interp = curve::Interp::Linear;
        CHECK(Approx(curve::Evaluate(c, 0.0f), 0.0f), "linear @0");
        CHECK(Approx(curve::Evaluate(c, 2.0f), 10.0f), "linear @end");
        CHECK(Approx(curve::Evaluate(c, 1.0f), 5.0f), "linear midpoint");
    }
    // Constant / stepped.
    {
        curve::Curve c;
        curve::Insert(c, 0.0f, 3.0f);
        curve::Insert(c, 1.0f, 9.0f);
        c.keys[0].interp = curve::Interp::Constant;
        CHECK(Approx(curve::Evaluate(c, 0.5f), 3.0f), "constant holds");
        CHECK(Approx(curve::Evaluate(c, 1.0f), 9.0f), "constant reaches next key");
    }
    // Cubic passes exactly through its keys (Hermite endpoints).
    {
        curve::Curve c;
        curve::Insert(c, 0.0f, 0.0f);
        curve::Insert(c, 1.0f, 1.0f);
        curve::Insert(c, 2.0f, 0.0f);
        CHECK(Approx(curve::Evaluate(c, 0.0f), 0.0f), "cubic key0");
        CHECK(Approx(curve::Evaluate(c, 1.0f), 1.0f), "cubic key1");
        CHECK(Approx(curve::Evaluate(c, 2.0f), 0.0f), "cubic key2");
        // Auto tangent flattens the local max, so the peak does not overshoot above 1.
        CHECK(curve::Evaluate(c, 1.0f) <= 1.0001f, "cubic no overshoot at peak");
    }
    // Extrapolation.
    {
        curve::Curve c;
        curve::Insert(c, 0.0f, 0.0f);
        curve::Insert(c, 1.0f, 1.0f);
        c.keys[0].interp = curve::Interp::Linear;
        c.preExtrap = curve::Extrap::Constant;
        c.postExtrap = curve::Extrap::Linear;
        CHECK(Approx(curve::Evaluate(c, -1.0f), 0.0f), "pre constant");
        CHECK(curve::Evaluate(c, 2.0f) > 1.5f, "post linear continues");
        c.postExtrap = curve::Extrap::Cycle;
        CHECK(Approx(curve::Evaluate(c, 1.5f), curve::Evaluate(c, 0.5f)), "cycle repeats");
    }
    // Weighted-tangent cubic still hits its endpoints.
    {
        curve::Curve c;
        curve::Insert(c, 0.0f, 0.0f);
        curve::Insert(c, 1.0f, 10.0f);
        c.keys[0].weightMode = curve::WeightMode::Both;
        c.keys[0].leaveWeight = 0.8f;
        c.keys[1].weightMode = curve::WeightMode::Both;
        c.keys[1].arriveWeight = 0.1f;
        CHECK(Approx(curve::Evaluate(c, 0.0f), 0.0f), "weighted key0");
        CHECK(Approx(curve::Evaluate(c, 1.0f), 10.0f, 0.05f), "weighted key1");
        const f32 mid = curve::Evaluate(c, 0.5f);
        CHECK(mid >= -0.1f && mid <= 10.1f, "weighted stays in range");
    }
    // Curve reduction: many colinear keys collapse to the two endpoints.
    {
        curve::Curve c;
        for (int i = 0; i <= 20; ++i) curve::Insert(c, i * 0.1f, i * 0.5f);
        for (auto& k : c.keys) k.interp = curve::Interp::Linear;
        const usize removed = curve::Reduce(c, 0.01f, 60.0f);
        CHECK(removed >= 15, "reduction drops redundant colinear keys");
        CHECK(Approx(curve::Evaluate(c, 1.0f), 5.0f, 0.05f), "reduced curve preserves shape");
    }

    if (fails == 0) HBE_INFO("Curve self-test: PASS");
    else HBE_ERROR("Curve self-test: {} failure(s)", fails);
    return fails == 0;
}

bool SelfTest() {
    RegisterBuiltinTrackKinds();
    int fails = 0;
    auto CHECK = [&](bool c, const char* msg) {
        if (!c) { HBE_ERROR("  [FAIL] cine: {}", msg); ++fails; }
    };

    // --- Registry ---
    CHECK(FindTrackKind("camera") != nullptr, "camera kind registered");
    CHECK(FindTrackKind("transform") != nullptr, "transform kind registered");
    CHECK(FindTrackKind("nonexistent") == nullptr, "unknown kind is null");
    if (const TrackKind* cam = FindTrackKind("camera")) CHECK(cam->drivesCamera, "camera drivesCamera");
    CHECK(TrackKinds().size() >= 15, "built-in kinds registered");

    // --- Rotation sampling (gimbal-free quaternion keys) ---
    {
        Section s;
        QuatKey q0; q0.time = 0.0f; q0.value = glm::quat(1, 0, 0, 0);
        QuatKey q1; q1.time = 1.0f; q1.value = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
        q1.ease = ease::Curve::Linear;
        s.rotationKeys = {q0, q1};
        const glm::quat mid = SampleRotation(s, 0.5f, glm::quat(1, 0, 0, 0));
        const glm::quat expect = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0));
        CHECK(std::fabs(std::fabs(glm::dot(mid, expect)) - 1.0f) < 1e-3f, "quaternion slerp at midpoint");
    }

    // --- Serialization round-trip ---
    {
        Sequence seq;
        seq.name = "Test";
        seq.guid = 0x0123456789abcdefull;
        seq.duration = 7.5f;
        Binding b; b.id = 1; b.label = "Hero"; b.name = "Hero"; b.guid = 0xdeadbeefull;
        seq.bindings.push_back(b);
        Track tr; tr.id = 42; tr.kind = "transform"; tr.name = "Hero/Transform"; tr.binding = 1;
        Section sec; sec.id = 7; sec.start = 0.0f; sec.duration = 5.0f;
        Channel ch; ch.target = "location.x";
        curve::Insert(ch.curve, 0.0f, 0.0f);
        curve::Insert(ch.curve, 5.0f, 100.0f);
        sec.channels.push_back(ch);
        sec.floatParams.push_back({"clip", 2.0f});
        tr.sections.push_back(sec);
        seq.tracks.push_back(tr);
        seq.markers.push_back({2.0f, "ACTION", "cue", "note", 0xFF00FF00u, true});
        seq.shots.push_back(Shot{9, "SHOT_0010", "shots/a.hbseq", 0.0f, 5.0f, 0.0f, 1.0f, "", "t1", true});

        const std::string json = ToJson(seq);
        auto back = FromJson(json, "test");
        CHECK(back.has_value(), "round-trip parses");
        if (back) {
            CHECK(back->name == "Test", "name round-trips");
            CHECK(back->guid == seq.guid, "guid round-trips");
            CHECK(Approx(back->duration, 7.5f), "duration round-trips");
            CHECK(back->bindings.size() == 1 && back->bindings[0].guid == 0xdeadbeefull, "binding round-trips");
            CHECK(back->tracks.size() == 1 && back->tracks[0].kind == "transform", "track round-trips");
            CHECK(!back->tracks.empty() && back->tracks[0].sections.size() == 1, "section round-trips");
            if (!back->tracks.empty() && !back->tracks[0].sections.empty()) {
                const Section& s2 = back->tracks[0].sections[0];
                CHECK(s2.channels.size() == 1 && s2.channels[0].target == "location.x", "channel round-trips");
                CHECK(s2.channels.size() == 1 && s2.channels[0].curve.keys.size() == 2, "curve keys round-trip");
                CHECK(Approx(SecFloat(s2, "clip"), 2.0f), "section float param round-trips");
            }
            CHECK(back->markers.size() == 1 && back->markers[0].name == "ACTION", "marker round-trips");
            CHECK(back->shots.size() == 1 && back->shots[0].shotId == "SHOT_0010", "shot round-trips");
            CHECK(back->IsMaster(), "sequence with shots is a master");
        }
    }

    // --- Deterministic evaluation against a real scene ---
    {
        Scene scene;
        entt::entity hero = scene.CreateEntity("Hero");
        scene.Registry().emplace_or_replace<Transform>(hero);

        Sequence seq;
        seq.duration = 2.0f;
        Binding b; b.id = 1; b.kind = BindingKind::Possessable; b.name = "Hero";
        seq.bindings.push_back(b);
        Track tr; tr.kind = "transform"; tr.binding = 1;
        Section sec; sec.start = 0.0f; sec.duration = 2.0f;
        Channel ch; ch.target = "location.x";
        curve::Insert(ch.curve, 0.0f, 0.0f);
        curve::Insert(ch.curve, 2.0f, 10.0f);
        ch.curve.keys[0].interp = curve::Interp::Linear;
        sec.channels.push_back(ch);
        tr.sections.push_back(sec);
        seq.tracks.push_back(tr);

        SequenceInstance inst;
        EvalContext ctx;
        ctx.scene = &scene;
        ctx.applyCamera = false;
        ctx.t = 1.0f;
        Evaluate(seq, inst, ctx);
        Transform* tf = scene.Registry().try_get<Transform>(hero);
        CHECK(tf && Approx(tf->position.x, 5.0f, 0.02f), "transform track poses location.x=5 at t=1");

        // Idempotent: a second Evaluate at the same time yields the same pose.
        const f32 first = tf ? tf->position.x : -1.0f;
        Evaluate(seq, inst, ctx);
        CHECK(tf && Approx(tf->position.x, first, 1e-4f), "evaluation is idempotent");

        // Binding cache resolved the possessable by Name.
        CHECK(inst.bindings.Resolve(seq, 1, scene, {}) == hero, "binding resolves by name");
    }

    if (fails == 0) HBE_INFO("Cinematics self-test: PASS");
    else HBE_ERROR("Cinematics self-test: {} failure(s)", fails);
    return fails == 0;
}

} // namespace hbe::cine
