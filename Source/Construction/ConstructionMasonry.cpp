#include "Construction/ConstructionMasonry.h"

#include "Core/Rng.h"

#include <algorithm>
#include <cmath>

namespace hbe::construction {

namespace {

// Independent seed streams. Named constants rather than literals so a later phase adding a new
// consumer cannot accidentally reuse one and shift every existing wall's variation.
constexpr u64 kPurposeMasonry = 0x4D41534Full; // "MASO"
// A SEPARATE stream from the unit jitter. Turning the age dial must not reshuffle the dimensional
// variation of the units that survive - otherwise nudging weathering visibly rebuilds the wall.
constexpr u64 kPurposeWeather = 0x57545452ull; // "WTTR"

// One unit as it sits in a course, BEFORE jitter. `exposed` is the face length along the wall;
// `depth` is how far it reaches into the wall.
struct UnitFace {
    f32 exposed;
    f32 depth;
};

// A header is laid end-on: its DEPTH faces out, so it exposes the short face and reaches further
// into the wall. That single swap is what distinguishes English and Flemish bonds from running
// bond, and it is why those walls read as thicker and more woven.
UnitFace Stretcher(const MasonryParams& p) { return {p.unitLength, p.unitDepth}; }
UnitFace Header(const MasonryParams& p) { return {p.unitDepth, p.unitLength}; }

// Is this course laid entirely in headers? English bond alternates whole courses.
bool IsHeaderCourse(BondPattern bond, u32 course) {
    return bond == BondPattern::English && (course % 2) == 1;
}

// The horizontal start offset for a course, which is the whole of what a bond pattern IS.
f32 CourseOffset(const MasonryParams& p, u32 course) {
    const f32 stride = p.unitLength + p.joint;
    switch (p.bond) {
        case BondPattern::Stack:
            return 0.0f; // deliberately none: every perpend lines up
        case BondPattern::Running:
            return (course % 2) ? stride * 0.5f : 0.0f;
        case BondPattern::English:
            // Header courses start on a quarter so the joints break against the stretchers above
            // and below, which is the entire structural point of the bond.
            return IsHeaderCourse(p.bond, course) ? (p.unitDepth + p.joint) * 0.5f : 0.0f;
        case BondPattern::Flemish:
            // Alternate courses shift by half a stretcher-plus-header pair, so headers stack in
            // vertical columns and stretchers centre over them.
            return (course % 2) ? (p.unitLength + p.unitDepth + p.joint * 2.0f) * 0.5f : 0.0f;
        default:
            return 0.0f;
    }
}

// The face for the n-th unit of a course. Flemish alternates WITHIN the course; English does not.
UnitFace FaceAt(const MasonryParams& p, u32 course, u32 indexInCourse) {
    if (p.bond == BondPattern::Flemish)
        return (indexInCourse % 2) ? Header(p) : Stretcher(p);
    return IsHeaderCourse(p.bond, course) ? Header(p) : Stretcher(p);
}

bool ParamsUsable(const MasonryParams& p) {
    return p.unitLength > 1e-4f && p.unitHeight > 1e-4f && p.unitDepth > 1e-4f && p.joint >= 0.0f;
}

// Walks the courses, calling `emit(course, indexInCourse, xStart, face)` for every unit that has
// any length inside the wall.
//
// ONE traversal, shared by generation and counting. If these were two loops they would drift, and
// a count one unit short of the geometry means a permanently refused UpdateMesh.
template <typename F>
MasonryResult WalkUnits(const ConstructionComponent& c, F&& emit) {
    const MasonryParams& p = c.masonry;
    if (c.extent.x <= 1e-5f || c.extent.y <= 1e-5f || c.extent.z <= 1e-5f)
        return MasonryResult::DegenerateExtent;
    if (!ParamsUsable(p)) return MasonryResult::DegenerateUnit;

    const f32 wallLength = c.extent.x * 2.0f;
    const f32 wallHeight = c.extent.y * 2.0f;
    const f32 courseStride = p.unitHeight + p.joint;
    if (courseStride <= 1e-5f) return MasonryResult::DegenerateUnit;

    const u32 courses = static_cast<u32>(std::floor(wallHeight / courseStride + 1e-4f));
    u32 emitted = 0;

    for (u32 course = 0; course < courses; ++course) {
        // Courses start at the BOTTOM and are laid upward, as they are built. A wall that
        // regenerates from the top down would shift every course when its height changed.
        const f32 y = -c.extent.y + static_cast<f32>(course) * courseStride + p.unitHeight * 0.5f;
        f32 x = -c.extent.x - CourseOffset(p, course);
        u32 idx = 0;

        while (x < c.extent.x - 1e-5f) {
            const UnitFace face = FaceAt(p, course, idx);
            const f32 unitStart = x;
            const f32 unitEnd = x + face.exposed;

            // CLIP AT THE WALL ENDS - a real bricklayer cuts the closer rather than letting it
            // overhang, and the offset courses of a running bond guarantee a cut at one end of
            // every other course.
            const f32 clippedStart = std::max(unitStart, -c.extent.x);
            const f32 clippedEnd = std::min(unitEnd, c.extent.x);
            const f32 clippedLen = clippedEnd - clippedStart;

            if (clippedLen > 1e-4f) {
                if (!emit(course, idx, clippedStart, clippedLen, face, y))
                    return MasonryResult::UnitCapReached;
                ++emitted;
                if (emitted >= p.maxUnits) return MasonryResult::UnitCapReached;
            }
            x = unitEnd + p.joint;
            ++idx;
            if (idx > 100000u) break; // paranoia: a zero-stride unit would spin forever
        }
    }
    return MasonryResult::Ok;
}

} // namespace

u32 MasonryUnitCount(const ConstructionDef&, const ConstructionComponent& c) {
    u32 n = 0;
    WalkUnits(c, [&](u32, u32, f32, f32, const UnitFace&, f32) {
        ++n;
        return true;
    });
    return n;
}

MasonryResult LayoutMasonry(const ConstructionDef& def, const ConstructionComponent& c,
                            std::vector<BrickPlacement>& out) {
    out.clear();
    const MasonryParams& p = c.masonry;
    const Rng base(def.SeedFor(c.id, kPurposeMasonry));
    const Rng weather(def.SeedFor(c.id, kPurposeWeather));
    const WeatheringParams& w = c.weathering;
    const f32 missing = w.EffectiveMissing();
    const f32 chip = w.EffectiveChip();
    const f32 shove = w.EffectiveDisplace();

    const MasonryResult r =
        WalkUnits(c, [&](u32 course, u32 idx, f32 xStart, f32 length, const UnitFace& face, f32 y) {
            BrickPlacement b;
            b.course = course;
            b.indexInCourse = idx;

            // THE STREAM IS KEYED ON (course, indexInCourse), NOT on emission order. A running
            // counter would reshuffle every unit's variation the moment a course was added, an
            // opening was cut, or one brick was destroyed - and the reshuffle would be invisible
            // in code review and obvious to an artist as "the whole wall changed".
            Rng rng = base.Split(b.ElementId());

            // MISSING UNITS. Drawn first and independently, so the wall that survives is the same
            // wall whatever else is tuned. Higher courses go first: gravity, frost and the fact
            // that nobody repoints a gable.
            if (missing > 0.0f) {
                Rng wr = weather.Split(b.ElementId());
                const f32 height = c.extent.y > 1e-4f
                                       ? (y + c.extent.y) / (c.extent.y * 2.0f)
                                       : 0.0f;
                if (wr.NextFloat() < missing * (0.45f + height * 0.55f)) return true;
            }

            // UNITS FILL THE WALL DEPTH. They used to be `min(unitDepth, wallDepth*2)/2`, which
            // for the default 0.25 m wall gave a 0.051 half-depth against a 0.120 mortar backing -
            // the units sat ENTIRELY INSIDE the backing slab and the wall rendered as a flat
            // coloured box with the brickwork invisible behind it.
            //
            // Filling the depth also makes the wall read from BOTH sides, which a veneer would
            // not. `face.depth` still does its real job: it is the EXPOSED LENGTH of a header in
            // a Flemish or English bond, set above in FaceAt.
            const f32 halfDepth = c.extent.z;
            b.extent = glm::vec3(length * 0.5f, p.unitHeight * 0.5f, halfDepth);

            // EROSION. Units shrink from their edges; the joints around them open up as a result,
            // which is exactly how weathered masonry reads from a distance.
            if (chip > 0.0f) {
                Rng wr = weather.Split(b.ElementId() ^ 0x5A5Au);
                const f32 k = 1.0f - wr.NextFloat() * chip * 0.35f;
                b.extent.x *= k;
                b.extent.y *= k;
                b.extent.z *= 1.0f - wr.NextFloat() * chip * 0.20f;
            }

            if (p.sizeJitter > 0.0f) {
                // Units only ever shrink. Growing them would push neighbours into each other and
                // close the joints that make the bond legible.
                const f32 s = 1.0f - rng.NextFloat() * p.sizeJitter;
                b.extent.x *= s;
                b.extent.y *= 1.0f - rng.NextFloat() * p.sizeJitter;
                b.extent.z *= s;
            }

            f32 z = 0.0f;
            if (p.depthJitter > 0.0f) z = rng.Signed() * p.depthJitter * face.depth * 0.5f;
            // DISPLACEMENT. Settlement and frost push individual units out of the wall plane and
            // rock them off square - the single most legible sign of an old wall.
            if (shove > 0.0f) {
                Rng wr = weather.Split(b.ElementId() ^ 0xA5A5u);
                z += wr.Signed() * shove * p.unitHeight * 0.6f;
                b.yaw += wr.Signed() * shove * 0.08f;
                b.roll += wr.Signed() * shove * 0.06f;
            }
            if (p.rotJitter > 0.0f) {
                b.yaw = rng.Signed() * p.rotJitter;
                b.roll = rng.Signed() * p.rotJitter;
            }

            // Centred on the CLIPPED span, so a cut closer sits flush with the wall end rather
            // than straddling it.
            b.center = glm::vec3(xStart + length * 0.5f, y, z);
            out.push_back(b);
            return true;
        });

    return r;
}

} // namespace hbe::construction
