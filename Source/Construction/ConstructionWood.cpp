#include "Construction/ConstructionWood.h"

#include "Core/Rng.h"

#include <algorithm>
#include <cmath>

namespace hbe::construction {

namespace {

// One stream per member ROLE, so adding shingles later cannot shift the studs of a wall already
// authored. Named constants rather than literals for exactly that reason.
constexpr u64 kPurposeTimber = 0x54494D42ull;  // "TIMB"
constexpr u64 kPurposePlank = 0x504C4E4Bull;   // "PLNK"
constexpr u64 kPurposeShingle = 0x53484E47ull; // "SHNG"
constexpr u64 kPurposeWeather = 0x57545452ull; // "WTTR" - separate, so ageing does not reshuffle warp

struct Emitter {
    std::vector<BoardPlacement>* out;
    u32 cap;
    const std::vector<BoxPiece>* cutters = nullptr;
    bool hitCap = false;

    // Weathering, applied at the one place every member passes through.
    //
    // STRUCTURE IS SPARED. Studs, plates, joists and rafters never go missing however old the
    // building is: a barn loses its siding and its shingles long before it loses the frame, and a
    // frame with random members deleted would be structurally meaningless - the graph would report
    // a building that cannot stand when the artist only asked for "old".
    const WeatheringParams* weather = nullptr;
    Rng weatherRng;

    static bool IsStructural(MemberRole r) {
        return r == MemberRole::Stud || r == MemberRole::Plate || r == MemberRole::Joist ||
               r == MemberRole::Rafter;
    }

    // A member crossing an opening is CUT, not silently kept and not silently dropped:
    //   * entirely inside the opening -> gone
    //   * partly inside, remainder still one box -> shortened to that box
    //   * partly inside, remainder is two pieces (a stud crossing a window) -> both emitted
    // The third case is why this cannot be a simple skip test: a stud crossing a window leaves a
    // cripple stud above it AND below it, and dropping either would leave the sheathing unbacked.
    bool Add(BoardPlacement b) {
        if (weather) {
            Rng wr = weatherRng.Split(b.ElementId());
            if (!IsStructural(b.role)) {
                const f32 missing = weather->EffectiveMissing();
                // Cladding and roofing are the first things a building loses.
                if (missing > 0.0f && wr.NextFloat() < missing * 1.4f) return true;
            }
            const f32 chip = weather->EffectiveChip();
            if (chip > 0.0f) {
                // Boards shrink and cup, which OPENS THE GAPS between them - the single most
                // recognisable feature of weathered siding, and the reason a derelict barn reads
                // as striped rather than as flat boards.
                const f32 k = 1.0f - wr.NextFloat() * chip * 0.30f;
                b.extent.x *= (b.extent.x < b.extent.y) ? k : 1.0f - wr.NextFloat() * chip * 0.10f;
                b.extent.y *= (b.extent.y <= b.extent.x) ? k : 1.0f - wr.NextFloat() * chip * 0.10f;
            }
            const f32 shove = weather->EffectiveDisplace();
            if (shove > 0.0f) {
                // Fixings let go: boards lift at one end and sag.
                b.pitch += wr.Signed() * shove * 0.12f;
                b.roll += wr.Signed() * shove * 0.10f;
                b.center.z += wr.Signed() * shove * b.extent.z * 1.5f;
            }
        }
        if (!cutters || cutters->empty()) return Push(b);
        if (FullyCut(b.center, b.extent, *cutters)) return true; // consumed by the opening

        glm::vec3 c = b.center, e = b.extent;
        if (ClipToSingleBox(c, e, *cutters)) {
            b.center = c;
            b.extent = e;
            return Push(b);
        }
        std::vector<BoxPiece> pieces;
        SubtractCutters(b.center, b.extent, *cutters, pieces);
        if (pieces.empty()) return true;
        for (const BoxPiece& p : pieces) {
            BoardPlacement part = b;
            part.center = p.center;
            part.extent = p.extent;
            if (!Push(part)) return false;
        }
        return true;
    }

    bool Push(const BoardPlacement& b) {
        if (out->size() >= cap) {
            hitCap = true;
            return false;
        }
        out->push_back(b);
        return true;
    }
};

// Bow and twist, drawn from a stream keyed on (role, index). Real lumber is never straight, and a
// wall of perfectly parallel boxes reads as a texture no matter how good the texture is.
void ApplyWarp(BoardPlacement& b, Rng& rng, f32 warp) {
    if (warp <= 0.0f) return;
    b.yaw += rng.Signed() * warp * 0.03f;
    b.roll += rng.Signed() * warp * 0.03f;
    b.pitch += rng.Signed() * warp * 0.02f;
}

// ---------------------------------------------------------------------------
// Timber framing: plates + studs
// ---------------------------------------------------------------------------

WoodResult LayoutStudWall(const ConstructionDef& def, const ConstructionComponent& c,
                          Emitter& em) {
    const TimberParams& t = c.timber;
    if (t.memberWidth <= 1e-4f || t.memberDepth <= 1e-4f || t.spacing <= 1e-3f)
        return WoodResult::DegenerateMember;

    const Rng base(def.SeedFor(c.id, kPurposeTimber));
    const f32 halfW = t.memberWidth * 0.5f;
    // The frame is as deep as the lumber, centred in the wall - sheathing and cladding sit on
    // either side of it, which is what makes a layered wall (Phase 6) possible without moving the
    // framing.
    const f32 halfD = std::min(t.memberDepth, c.extent.z * 2.0f) * 0.5f;

    u32 plateIndex = 0;
    auto addPlate = [&](f32 y) {
        BoardPlacement p;
        p.role = MemberRole::Plate;
        p.index = plateIndex++;
        p.center = glm::vec3(0.0f, y, 0.0f);
        p.extent = glm::vec3(c.extent.x, halfW, halfD);
        Rng rng = base.Split(p.ElementId());
        ApplyWarp(p, rng, t.warp);
        return em.Add(p);
    };

    f32 studBottom = -c.extent.y;
    f32 studTop = c.extent.y;
    if (t.bottomPlate) {
        if (!addPlate(-c.extent.y + halfW)) return WoodResult::MemberCapReached;
        studBottom += t.memberWidth;
    }
    for (u32 i = 0; i < t.topPlates; ++i) {
        if (!addPlate(c.extent.y - halfW - static_cast<f32>(i) * t.memberWidth))
            return WoodResult::MemberCapReached;
        studTop -= t.memberWidth;
    }

    const f32 studHeight = studTop - studBottom;
    if (studHeight <= 1e-3f) return WoodResult::Ok; // a wall shorter than its own plates

    const f32 wallLength = c.extent.x * 2.0f;
    const u32 bays = static_cast<u32>(std::floor(wallLength / t.spacing + 1e-4f));

    u32 studIndex = 0;
    auto addStud = [&](f32 x) {
        BoardPlacement s;
        s.role = MemberRole::Stud;
        s.index = studIndex++;
        s.center = glm::vec3(x, studBottom + studHeight * 0.5f, 0.0f);
        s.extent = glm::vec3(halfW, studHeight * 0.5f, halfD);
        Rng rng = base.Split(s.ElementId());
        ApplyWarp(s, rng, t.warp);
        return em.Add(s);
    };

    for (u32 i = 0; i <= bays; ++i) {
        const f32 x = -c.extent.x + static_cast<f32>(i) * t.spacing + halfW;
        if (x + halfW > c.extent.x) break;
        if (!addStud(x)) return WoodResult::MemberCapReached;
    }
    // THE END STUD IS NOT OPTIONAL. On-centre spacing almost never divides the wall exactly, so
    // the loop above leaves a gap at the far end. Real framing always closes it - a wall whose
    // last bay is open is not a wall, and the sheathing would have nothing to fix to.
    const f32 endX = c.extent.x - halfW;
    bool haveEnd = false;
    for (const BoardPlacement& b : *em.out)
        if (b.role == MemberRole::Stud && std::fabs(b.center.x - endX) < halfW) haveEnd = true;
    if (!haveEnd && !addStud(endX)) return WoodResult::MemberCapReached;

    // FRAMING AROUND THE OPENINGS. This is the difference between a wall with a hole in it and a
    // wall with a window in it. The studs an opening crosses were carrying load; cutting them
    // without replacing that path leaves the top plate spanning thin air.
    //
    // Emitted AFTER the regular studs and deliberately NOT through em.Add - these members are
    // built to fit around the opening, so running them back through the cutter would carve away
    // the very pieces that exist to frame it.
    if (em.cutters && !em.cutters->empty()) {
        u32 headerIndex = 0;
        u32 jackIndex = 1000; // a distinct index band, so a jack stud can never collide with a
                              // regular stud's element id
        for (const BoxPiece& cut : *em.cutters) {
            const f32 cutTop = cut.center.y + cut.extent.y;
            const f32 cutBottom = cut.center.y - cut.extent.y;
            if (cutTop >= studTop - 1e-4f) continue; // reaches the plates; nothing to span

            // The header: spans the opening, sitting directly on top of it.
            BoardPlacement hdr;
            hdr.role = MemberRole::Plate; // a header is a horizontal load-carrying member
            hdr.index = 500 + headerIndex++;
            const f32 hdrHalf = std::min(t.memberWidth, studTop - cutTop) * 0.5f;
            if (hdrHalf > 1e-4f) {
                hdr.center = glm::vec3(cut.center.x, cutTop + hdrHalf, 0.0f);
                hdr.extent = glm::vec3(cut.extent.x + t.memberWidth, hdrHalf, halfD);
                Rng rng = base.Split(hdr.ElementId());
                ApplyWarp(hdr, rng, t.warp);
                if (!em.Push(hdr)) return WoodResult::MemberCapReached;
            }

            // Jack studs: short studs each side carrying the header down to the bottom plate.
            const f32 jackTop = cutTop;
            const f32 jackHeight = jackTop - studBottom;
            if (jackHeight > 1e-3f) {
                for (int side = 0; side < 2; ++side) {
                    const f32 x = cut.center.x +
                                  (side ? 1.0f : -1.0f) * (cut.extent.x + halfW);
                    if (std::fabs(x) > c.extent.x) continue;
                    BoardPlacement jack;
                    jack.role = MemberRole::Stud;
                    jack.index = jackIndex++;
                    jack.center = glm::vec3(x, studBottom + jackHeight * 0.5f, 0.0f);
                    jack.extent = glm::vec3(halfW, jackHeight * 0.5f, halfD);
                    Rng rng = base.Split(jack.ElementId());
                    ApplyWarp(jack, rng, t.warp);
                    if (!em.Push(jack)) return WoodResult::MemberCapReached;
                }
            }

            // A sill under a window. Suppressed for anything reaching the floor - a door does not
            // get a bar across the threshold the player walks through.
            if (cutBottom > studBottom + t.memberWidth) {
                BoardPlacement sill;
                sill.role = MemberRole::Plate;
                sill.index = 700 + headerIndex;
                sill.center = glm::vec3(cut.center.x, cutBottom - halfW, 0.0f);
                sill.extent = glm::vec3(cut.extent.x, halfW, halfD);
                Rng rng = base.Split(sill.ElementId());
                ApplyWarp(sill, rng, t.warp);
                if (!em.Push(sill)) return WoodResult::MemberCapReached;
            }
        }
    }

    return WoodResult::Ok;
}

// ---------------------------------------------------------------------------
// Floor / ceiling framing: joists + a deck
// ---------------------------------------------------------------------------

WoodResult LayoutJoistDeck(const ConstructionDef& def, const ConstructionComponent& c,
                           Emitter& em) {
    const TimberParams& t = c.timber;
    if (t.memberWidth <= 1e-4f || t.memberDepth <= 1e-4f || t.spacing <= 1e-3f)
        return WoodResult::DegenerateMember;

    const Rng base(def.SeedFor(c.id, kPurposeTimber));
    const f32 halfW = t.memberWidth * 0.5f;
    const f32 joistDepth = std::min(t.memberDepth, c.extent.y * 2.0f);
    const f32 halfDepth = joistDepth * 0.5f;

    // Joists run along X and repeat across Z, hung below the top surface so the deck sits on them.
    const f32 span = c.extent.z * 2.0f;
    const u32 count = static_cast<u32>(std::floor(span / t.spacing + 1e-4f));
    const f32 joistY = c.extent.y - joistDepth * 0.5f;

    u32 idx = 0;
    auto addJoist = [&](f32 z) {
        BoardPlacement j;
        j.role = MemberRole::Joist;
        j.index = idx++;
        j.center = glm::vec3(0.0f, joistY - halfDepth, z);
        j.extent = glm::vec3(c.extent.x, halfDepth, halfW);
        Rng rng = base.Split(j.ElementId());
        ApplyWarp(j, rng, t.warp);
        return em.Add(j);
    };

    for (u32 i = 0; i <= count; ++i) {
        const f32 z = -c.extent.z + static_cast<f32>(i) * t.spacing + halfW;
        if (z + halfW > c.extent.z) break;
        if (!addJoist(z)) return WoodResult::MemberCapReached;
    }
    const f32 endZ = c.extent.z - halfW;
    bool haveEnd = false;
    for (const BoardPlacement& b : *em.out)
        if (b.role == MemberRole::Joist && std::fabs(b.center.z - endZ) < halfW) haveEnd = true;
    if (!haveEnd && !addJoist(endZ)) return WoodResult::MemberCapReached;

    // The subfloor deck: one sheet sitting on top of the joists.
    BoardPlacement deck;
    deck.role = MemberRole::Board;
    deck.index = 0;
    const f32 deckHalf = std::max(c.plank.boardThickness, 0.001f) * 0.5f;
    deck.center = glm::vec3(0.0f, c.extent.y - deckHalf, 0.0f);
    deck.extent = glm::vec3(c.extent.x, deckHalf, c.extent.z);
    if (!em.Add(deck)) return WoodResult::MemberCapReached;

    return WoodResult::Ok;
}

// ---------------------------------------------------------------------------
// Boards: cladding, decking, flooring
// ---------------------------------------------------------------------------

WoodResult LayoutBoards(const ConstructionDef& def, const ConstructionComponent& c, Emitter& em) {
    const PlankParams& p = c.plank;
    if (p.boardWidth <= 1e-4f || p.boardThickness <= 1e-5f) return WoodResult::DegenerateMember;

    const Rng base(def.SeedFor(c.id, kPurposePlank));
    const bool vertical = p.direction == BoardDirection::Vertical ||
                          p.profile == SidingProfile::BoardAndBatten;
    const f32 halfThick = p.boardThickness * 0.5f;

    // A lapped profile advances by LESS than a board width - that is what an overlap is. Flush and
    // board-and-batten advance by a full width plus the gap.
    f32 step = p.boardWidth + p.gap;
    if (p.profile == SidingProfile::Clapboard || p.profile == SidingProfile::Shiplap)
        step = std::max(p.boardWidth - p.overlap, 0.005f);

    // Boards sit ON the face, not inside it, so cladding stands proud of the wall it covers.
    const f32 faceZ = c.extent.z + halfThick;
    const f32 axisExtent = vertical ? c.extent.x : c.extent.y;
    const f32 lengthExtent = vertical ? c.extent.y : c.extent.x;
    const u32 count = static_cast<u32>(std::floor((axisExtent * 2.0f) / step + 1e-4f)) + 1;

    for (u32 i = 0; i < count; ++i) {
        BoardPlacement b;
        b.role = MemberRole::Board;
        b.index = i;
        Rng rng = base.Split(b.ElementId());

        const f32 along = -axisExtent + static_cast<f32>(i) * step + p.boardWidth * 0.5f;
        if (along - p.boardWidth * 0.5f > axisExtent) break;

        // Clip the last board rather than letting it overhang the wall it clads.
        const f32 top = std::min(along + p.boardWidth * 0.5f, axisExtent);
        const f32 bottom = std::max(along - p.boardWidth * 0.5f, -axisExtent);
        const f32 half = (top - bottom) * 0.5f;
        if (half <= 1e-4f) continue;
        const f32 mid = (top + bottom) * 0.5f;

        f32 len = lengthExtent;
        if (p.lengthJitter > 0.0f) len *= 1.0f - rng.NextFloat() * p.lengthJitter * 0.25f;

        if (vertical) {
            b.center = glm::vec3(mid, 0.0f, faceZ);
            b.extent = glm::vec3(half, len, halfThick);
        } else {
            b.center = glm::vec3(0.0f, mid, faceZ);
            b.extent = glm::vec3(len, half, halfThick);
        }

        // A CLAPBOARD IS WEDGE-SHAPED IN SECTION: it is thin at the top where it tucks under the
        // course above and thick at the bottom where it laps over the one below. Tilting it is
        // what produces the shadow line that makes clapboard read as clapboard rather than as
        // flat planks with grooves.
        if (p.profile == SidingProfile::Clapboard && !vertical) {
            b.pitch = -std::atan2(p.overlap * 0.5f, std::max(p.boardWidth, 1e-3f));
            b.center.z += halfThick * 0.5f;
        }
        if (p.direction == BoardDirection::Diagonal) b.roll = 0.7853981f; // 45 degrees

        ApplyWarp(b, rng, p.warp);
        if (!em.Add(b)) return WoodResult::MemberCapReached;
    }

    // Battens cover every seam between the wide boards. Without them board-and-batten is just
    // vertical siding, which is a different building.
    if (p.profile == SidingProfile::BoardAndBatten && p.battenWidth > 1e-4f) {
        const u32 seams = count;
        for (u32 i = 0; i < seams; ++i) {
            BoardPlacement bt;
            bt.role = MemberRole::Batten;
            bt.index = i;
            const f32 x = -axisExtent + static_cast<f32>(i) * step;
            if (std::fabs(x) > axisExtent) continue;
            Rng rng = base.Split(bt.ElementId());
            bt.center = glm::vec3(x, 0.0f, faceZ + halfThick);
            bt.extent = glm::vec3(p.battenWidth * 0.5f, c.extent.y, halfThick);
            ApplyWarp(bt, rng, p.warp);
            if (!em.Add(bt)) return WoodResult::MemberCapReached;
        }
    }

    return WoodResult::Ok;
}

// ---------------------------------------------------------------------------
// Shingles on a gable roof
// ---------------------------------------------------------------------------

WoodResult LayoutShingles(const ConstructionDef& def, const ConstructionComponent& c,
                          Emitter& em) {
    const ShingleParams& s = c.shingle;
    if (s.width <= 1e-4f || s.length <= 1e-4f || s.exposure <= 1e-4f)
        return WoodResult::DegenerateMember;

    const Rng base(def.SeedFor(c.id, kPurposeShingle));
    // The gable's slope: rise = extent.y, run = extent.z, so the rafter length is the hypotenuse.
    const f32 rise = c.extent.y * 2.0f;
    const f32 run = c.extent.z;
    const f32 slope = std::sqrt(rise * rise + run * run);
    if (slope <= 1e-3f) return WoodResult::DegenerateExtent;

    const f32 pitch = std::atan2(rise, run);
    const u32 courses = static_cast<u32>(std::floor(slope / s.exposure + 1e-4f));
    const u32 perCourse = static_cast<u32>(std::floor((c.extent.x * 2.0f) / s.width + 1e-4f)) + 1;

    u32 idx = 0;
    // Both slopes. A roof shingled on one side is a bug nobody notices until the camera moves.
    for (int side = 0; side < 2; ++side) {
        const f32 zSign = side == 0 ? 1.0f : -1.0f;
        for (u32 course = 0; course < courses; ++course) {
            // Laid from the eave upward, as a roofer works, so each course laps the one below.
            const f32 d = static_cast<f32>(course) * s.exposure + s.length * 0.5f;
            if (d - s.length * 0.5f > slope) break;
            const f32 t = d / slope;
            const f32 z = zSign * run * (1.0f - t);
            const f32 y = -c.extent.y + rise * t;

            for (u32 i = 0; i < perCourse; ++i) {
                BoardPlacement sh;
                sh.role = MemberRole::Shingle;
                sh.index = idx++;
                Rng rng = base.Split(sh.ElementId());

                f32 x = -c.extent.x + static_cast<f32>(i) * s.width + s.width * 0.5f;
                // Stagger alternate courses so the vertical joints do not line up - the same
                // reason masonry offsets its courses, and here it is what stops water tracking
                // straight down a seam.
                if (course % 2) x += s.width * 0.5f;
                if (x - s.width * 0.5f > c.extent.x) break;

                if (s.jitter > 0.0f) {
                    x += rng.Signed() * s.jitter * s.width * 0.15f;
                    sh.yaw = rng.Signed() * s.jitter * 0.05f;
                }
                sh.center = glm::vec3(x, y, z);
                sh.extent = glm::vec3(s.width * 0.5f, s.thickness * 0.5f, s.length * 0.5f);
                sh.pitch = zSign > 0.0f ? pitch : -pitch;
                if (!em.Add(sh)) return WoodResult::MemberCapReached;
            }
        }
    }

    // THE RIDGE CAP. Courses climb from the eave and the last one stops a shingle-length short of
    // the peak, so the two slopes never meet - leaving a visible slot straight down the ridge with
    // the deck edge showing through it. Every real pitched roof is capped for exactly this reason:
    // the ridge is the one joint water would otherwise run straight into.
    {
        const f32 capWidth = s.width * 0.9f;
        const u32 capCount =
            static_cast<u32>(std::floor((c.extent.x * 2.0f) / capWidth + 1e-4f)) + 1;
        for (u32 i = 0; i < capCount; ++i) {
            const f32 x = -c.extent.x + static_cast<f32>(i) * capWidth + capWidth * 0.5f;
            if (x - capWidth * 0.5f > c.extent.x) break;
            BoardPlacement cap;
            cap.role = MemberRole::Shingle;
            cap.index = idx++;
            Rng rng = base.Split(cap.ElementId());
            f32 jx = x;
            if (s.jitter > 0.0f) {
                jx += rng.Signed() * s.jitter * capWidth * 0.1f;
                cap.roll = rng.Signed() * s.jitter * 0.04f;
            }
            // Sits ON the peak and spans both slopes, so neither slope's top edge is exposed.
            cap.center = glm::vec3(jx, c.extent.y - s.thickness * 0.5f, 0.0f);
            cap.extent = glm::vec3(capWidth * 0.5f, s.thickness,
                                   std::max(s.exposure, s.thickness * 2.0f));
            if (!em.Add(cap)) return WoodResult::MemberCapReached;
        }
    }
    return WoodResult::Ok;
}

} // namespace

bool IsWoodConstruction(const ConstructionComponent& c) {
    switch (c.kind) {
        // SINGLE PIECES OF LUMBER. A beam is a beam; generating it as a stack of boards would be
        // the same error as generating poured concrete as bricks.
        case ComponentKind::Beam:
        case ComponentKind::Column:
        case ComponentKind::Header:
        case ComponentKind::Stud:
        case ComponentKind::Plate:
        case ComponentKind::Joist:
        case ComponentKind::Rafter:
        case ComponentKind::Brace:
            return false;
        case ComponentKind::Wall:
            return c.material == MaterialKind::TimberFrame || IsPlankMaterial(c.material);
        case ComponentKind::Floor:
        case ComponentKind::Ceiling:
            return c.material == MaterialKind::TimberFrame || IsPlankMaterial(c.material);
        case ComponentKind::Siding:
        case ComponentKind::Sheathing:
        case ComponentKind::FloorSurface:
            return IsPlankMaterial(c.material);
        case ComponentKind::Roof:
        case ComponentKind::RoofSurface:
            return c.material == MaterialKind::WoodShingle;
        default:
            return false;
    }
}

WoodResult LayoutWood(const ConstructionDef& def, const ConstructionComponent& c,
                      std::vector<BoardPlacement>& out, const std::vector<BoxPiece>* cutters) {
    out.clear();
    if (!IsWoodConstruction(c)) return WoodResult::NotWood;
    if (c.extent.x <= 1e-5f || c.extent.y <= 1e-5f || c.extent.z <= 1e-5f)
        return WoodResult::DegenerateExtent;

    u32 cap = c.timber.maxMembers;
    if (c.material == MaterialKind::WoodShingle) cap = c.shingle.maxShingles;
    else if (IsPlankMaterial(c.material)) cap = c.plank.maxBoards;

    Emitter em{&out, cap, cutters};
    if (c.weathering.age > 0.0f || c.weathering.missingChance > 0.0f ||
        c.weathering.chipAmount > 0.0f || c.weathering.displacement > 0.0f) {
        em.weather = &c.weathering;
        em.weatherRng = Rng(def.SeedFor(c.id, kPurposeWeather));
    }
    WoodResult r = WoodResult::Ok;

    if (c.material == MaterialKind::WoodShingle) {
        r = LayoutShingles(def, c, em);
    } else if (c.material == MaterialKind::TimberFrame) {
        r = (c.kind == ComponentKind::Floor || c.kind == ComponentKind::Ceiling)
                ? LayoutJoistDeck(def, c, em)
                : LayoutStudWall(def, c, em);
    } else {
        r = LayoutBoards(def, c, em);
    }

    if (em.hitCap) return WoodResult::MemberCapReached;
    return r;
}

} // namespace hbe::construction
