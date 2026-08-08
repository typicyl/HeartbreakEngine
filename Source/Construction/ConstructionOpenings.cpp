#include "Construction/ConstructionOpenings.h"

#include "Construction/ConstructionGeometry.h"  // WorldMatrix

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace hbe::construction {

namespace {

struct Aabb {
    glm::vec3 mn{0.0f}, mx{0.0f};
    bool Valid() const { return mx.x > mn.x && mx.y > mn.y && mx.z > mn.z; }
};

Aabb ToAabb(const glm::vec3& c, const glm::vec3& e) { return {c - e, c + e}; }

BoxPiece FromAabb(const Aabb& a) {
    BoxPiece p;
    p.center = (a.mn + a.mx) * 0.5f;
    p.extent = (a.mx - a.mn) * 0.5f;
    return p;
}

bool Overlaps(const Aabb& a, const Aabb& b, f32 eps = 1e-5f) {
    return a.mn.x < b.mx.x - eps && a.mx.x > b.mn.x + eps && a.mn.y < b.mx.y - eps &&
           a.mx.y > b.mn.y + eps && a.mn.z < b.mx.z - eps && a.mx.z > b.mn.z + eps;
}

Aabb Intersect(const Aabb& a, const Aabb& b) {
    return {glm::max(a.mn, b.mn), glm::min(a.mx, b.mx)};
}

// Subtracts ONE cutter from ONE box, exactly, as up to six non-overlapping pieces.
//
// The order is deliberate: slab off the X extremes first (full Y and Z), then the Y extremes
// within the remaining X band, then the Z extremes within the remaining X and Y band. Every piece
// is disjoint from the others and their union is exactly box-minus-cutter. Doing it in any
// consistent order works; doing it in NO consistent order produces overlapping pieces that
// z-fight, which is the classic way this goes wrong.
void SubtractOne(const Aabb& box, const Aabb& cut, std::vector<BoxPiece>& out) {
    if (!Overlaps(box, cut)) {
        out.push_back(FromAabb(box));
        return;
    }
    const Aabb o = Intersect(box, cut);

    // X slabs, full height and depth.
    if (o.mn.x > box.mn.x + 1e-5f)
        out.push_back(FromAabb({box.mn, {o.mn.x, box.mx.y, box.mx.z}}));
    if (o.mx.x < box.mx.x - 1e-5f)
        out.push_back(FromAabb({{o.mx.x, box.mn.y, box.mn.z}, box.mx}));

    // Y slabs, inside the cutter's X band, full depth. These are the sill course and the head
    // course of a real opening.
    if (o.mn.y > box.mn.y + 1e-5f)
        out.push_back(FromAabb({{o.mn.x, box.mn.y, box.mn.z}, {o.mx.x, o.mn.y, box.mx.z}}));
    if (o.mx.y < box.mx.y - 1e-5f)
        out.push_back(FromAabb({{o.mn.x, o.mx.y, box.mn.z}, {o.mx.x, box.mx.y, box.mx.z}}));

    // Z slabs, inside the cutter's X and Y band. Empty for an opening that passes right through,
    // which is the normal case - a window hole goes all the way to the other side.
    if (o.mn.z > box.mn.z + 1e-5f)
        out.push_back(FromAabb({{o.mn.x, o.mn.y, box.mn.z}, {o.mx.x, o.mx.y, o.mn.z}}));
    if (o.mx.z < box.mx.z - 1e-5f)
        out.push_back(FromAabb({{o.mn.x, o.mn.y, o.mx.z}, {o.mx.x, o.mx.y, box.mx.z}}));
}

} // namespace

void GatherCutters(const ConstructionDef& def, const ConstructionComponent& host,
                   const DamageState* damage, std::vector<BoxPiece>& out, bool includeDisabled) {
    out.clear();
    // The host's world placement, so a cutter authored anywhere in the building can be expressed
    // in the host's own frame.
    const glm::mat4 hostX = WorldMatrix(def, host.id);
    const glm::mat4 hostInv = glm::inverse(hostX);

    for (const ConstructionComponent& c : def.components) {
        if (c.id == host.id) continue;
        // Either an explicit subtractive brush, or a classic Opening (which is subtractive by
        // definition - it is a hole).
        const bool isCutter = c.subtract || c.kind == ComponentKind::Opening;
        if (!isCutter) continue;
        // A HIDDEN OR DESTROYED OPENING CUTS NOTHING. That single line is the whole of the brief's
        // "disable a cutter without destroying the base construction" - because geometry is
        // derived, the wall simply comes back solid on the next regeneration.
        if (!includeDisabled) {
            if (c.hidden) continue;
            if (damage && damage->IsDestroyed(c.id)) continue;
        }

        BoxPiece p;
        if (c.parent == host.id) {
            // Parented: already in the host's local space.
            p.center = c.position;
            p.extent = c.extent;
        } else {
            // Anywhere else in the building: bring it into the host's frame.
            //
            // THE STATED LIMIT (see this file's header): the subtraction is exact only while the
            // cutter is axis-aligned IN THE HOST'S FRAME. When it is rotated relative to the host,
            // its transformed AABB is used instead - a conservative box that cuts at least the
            // region the artist drew. Reporting a slightly larger hole beats silently cutting
            // nothing, and a real rotated cut needs CSG this engine does not have.
            const glm::mat4 rel = hostInv * WorldMatrix(def, c.id);
            const glm::vec3 centre = glm::vec3(rel[3]);
            const glm::mat3 m = glm::mat3(rel);
            const glm::vec3 ext(
                std::fabs(m[0][0]) * c.extent.x + std::fabs(m[1][0]) * c.extent.y +
                    std::fabs(m[2][0]) * c.extent.z,
                std::fabs(m[0][1]) * c.extent.x + std::fabs(m[1][1]) * c.extent.y +
                    std::fabs(m[2][1]) * c.extent.z,
                std::fabs(m[0][2]) * c.extent.x + std::fabs(m[1][2]) * c.extent.y +
                    std::fabs(m[2][2]) * c.extent.z);
            // Cheap rejection: a cutter nowhere near this part must not cost it any geometry.
            if (std::fabs(centre.x) - ext.x > host.extent.x ||
                std::fabs(centre.y) - ext.y > host.extent.y ||
                std::fabs(centre.z) - ext.z > host.extent.z)
                continue;
            p.center = centre;
            p.extent = ext;
        }
        out.push_back(p);
    }
}

void SubtractCutters(const glm::vec3& center, const glm::vec3& extent,
                     const std::vector<BoxPiece>& cutters, std::vector<BoxPiece>& out) {
    out.clear();
    out.push_back(BoxPiece{center, extent});
    if (cutters.empty()) return;

    std::vector<BoxPiece> next;
    for (const BoxPiece& cut : cutters) {
        const Aabb ca = ToAabb(cut.center, cut.extent);
        next.clear();
        for (const BoxPiece& piece : out) {
            const Aabb pa = ToAabb(piece.center, piece.extent);
            if (!pa.Valid()) continue;
            SubtractOne(pa, ca, next);
        }
        out.swap(next);
        if (out.empty()) return; // entirely consumed; a wall that is all doorway is not a wall
    }
}

bool FullyCut(const glm::vec3& center, const glm::vec3& extent,
              const std::vector<BoxPiece>& cutters) {
    const Aabb b = ToAabb(center, extent);
    for (const BoxPiece& c : cutters) {
        const Aabb ca = ToAabb(c.center, c.extent);
        if (b.mn.x >= ca.mn.x - 1e-5f && b.mx.x <= ca.mx.x + 1e-5f && b.mn.y >= ca.mn.y - 1e-5f &&
            b.mx.y <= ca.mx.y + 1e-5f && b.mn.z >= ca.mn.z - 1e-5f && b.mx.z <= ca.mx.z + 1e-5f)
            return true;
    }
    return false;
}

bool ClipToSingleBox(glm::vec3& center, glm::vec3& extent, const std::vector<BoxPiece>& cutters) {
    Aabb b = ToAabb(center, extent);
    bool changed = false;

    for (const BoxPiece& c : cutters) {
        const Aabb ca = ToAabb(c.center, c.extent);
        if (!Overlaps(b, ca)) continue;

        // The remainder is a single box only when the cutter spans the box completely on two of
        // the three axes - then it is slicing an end off rather than punching a hole through the
        // middle. Anything else needs the full decomposition, so say so instead of guessing.
        const bool spanX = ca.mn.x <= b.mn.x + 1e-5f && ca.mx.x >= b.mx.x - 1e-5f;
        const bool spanY = ca.mn.y <= b.mn.y + 1e-5f && ca.mx.y >= b.mx.y - 1e-5f;
        const bool spanZ = ca.mn.z <= b.mn.z + 1e-5f && ca.mx.z >= b.mx.z - 1e-5f;
        const int spans = (spanX ? 1 : 0) + (spanY ? 1 : 0) + (spanZ ? 1 : 0);
        if (spans < 2) return false;

        // Trim along the one axis the cutter does not span.
        auto trim = [&](f32& lo, f32& hi, f32 clo, f32 chi) {
            const bool fromLow = clo <= lo + 1e-5f;
            const bool fromHigh = chi >= hi - 1e-5f;
            if (fromLow && fromHigh) {
                lo = hi = 0.0f; // consumed
            } else if (fromLow) {
                lo = chi;
            } else if (fromHigh) {
                hi = clo;
            } else {
                lo = hi = std::numeric_limits<f32>::quiet_NaN(); // hole through the middle
            }
        };
        if (!spanX) trim(b.mn.x, b.mx.x, ca.mn.x, ca.mx.x);
        else if (!spanY) trim(b.mn.y, b.mx.y, ca.mn.y, ca.mx.y);
        else if (!spanZ) trim(b.mn.z, b.mx.z, ca.mn.z, ca.mx.z);
        else { // spans all three: fully consumed
            return false;
        }
        if (!std::isfinite(b.mn.x) || !std::isfinite(b.mn.y) || !std::isfinite(b.mn.z))
            return false;
        changed = true;
    }

    if (!changed) return false;
    if (!b.Valid()) return false;
    const BoxPiece p = FromAabb(b);
    center = p.center;
    extent = p.extent;
    return true;
}

void BuildFillingGeometry(const ConstructionDef&, const ConstructionComponent& c,
                          std::vector<BoxPiece>& frame, std::vector<BoxPiece>& panel) {
    frame.clear();
    panel.clear();
    if (c.extent.x <= 1e-4f || c.extent.y <= 1e-4f) return;

    const bool isDoor = c.kind == ComponentKind::Door;
    // Frame section, as a fraction of the opening, clamped so a tiny window still gets a frame
    // thick enough to see rather than a sliver that z-fights the glass.
    const f32 fw = std::min(0.06f, std::min(c.extent.x, c.extent.y) * 0.35f);
    const f32 fd = std::max(c.extent.z, 0.02f);

    // Head and jambs. A door has no sill member - it meets a threshold at the floor instead, so
    // emitting one would put a bar across the doorway the player walks through.
    frame.push_back({{0.0f, c.extent.y - fw, 0.0f}, {c.extent.x, fw, fd}});
    if (isDoor) {
        frame.push_back({{0.0f, -c.extent.y + fw * 0.35f, 0.0f}, {c.extent.x, fw * 0.35f, fd}});
    } else {
        frame.push_back({{0.0f, -c.extent.y + fw, 0.0f}, {c.extent.x, fw, fd}});
    }
    frame.push_back({{-c.extent.x + fw, 0.0f, 0.0f}, {fw, c.extent.y - fw, fd}});
    frame.push_back({{c.extent.x - fw, 0.0f, 0.0f}, {fw, c.extent.y - fw, fd}});

    // The panel: a door leaf fills the frame; glazing is a thin pane centred in the reveal.
    const f32 innerX = std::max(c.extent.x - fw * 2.0f, 1e-3f);
    const f32 innerY = std::max(c.extent.y - fw * 2.0f, 1e-3f);
    const f32 panelDepth = isDoor ? fd * 0.6f : std::min(fd * 0.15f, 0.006f);
    const f32 panelY = isDoor ? (-c.extent.y + fw * 0.7f + innerY) : 0.0f;
    panel.push_back({{0.0f, isDoor ? panelY - innerY * 0.0f : 0.0f, 0.0f},
                     {innerX, innerY, panelDepth}});
}

} // namespace hbe::construction
