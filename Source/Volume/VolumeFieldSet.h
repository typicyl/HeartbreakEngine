// Source/Volume/VolumeFieldSet.h - the wiring between a solver's internal fields and what it EXPOSES.
//
// A solver declares every field it holds (velocity/pressure/divergence are internal; density/
// temperature are usually exposed). This one small registry then DERIVES both:
//   - AvailableFields() : the FieldMask capability summary (all standard fields it can produce), and
//   - the EXPOSED subset : the fields ReadbackFrame copies into VolumeFrame (config.bakeFields).
// So exposing/hiding a field is a data change (config.bakeFields) - it never alters how the solver
// steps, and internal fields never leak into the .hbvol. Header-only; both CPU + GPU solvers reuse it.
#pragma once

#include "Volume/VolumeFrame.h"

#include <string>
#include <string_view>
#include <vector>

namespace hbe::volume {

// Map a standard field NAME to its FieldMask bit (None for custom/non-standard names, which are
// still exposable by name - the mask is only a cheap capability summary, per VolumeFrame.h).
inline FieldMask MaskForFieldName(std::string_view name) {
    if (name == "density")     return FieldMask::Density;
    if (name == "temperature") return FieldMask::Temperature;
    if (name == "velocity")    return FieldMask::Velocity;
    if (name == "fuel")        return FieldMask::Fuel;
    if (name == "flame")       return FieldMask::Flame;
    if (name == "vorticity")   return FieldMask::Vorticity;
    return FieldMask::None;
}

struct VolumeFieldDecl {
    std::string name;                        // interchange key: "density", "velocity", ...
    FieldType   type       = FieldType::Scalar;
    f32         background  = 0.0f;           // "empty" value the baker prunes to sparsity
    bool        exposed     = false;          // true => copied into VolumeFrame by ReadbackFrame
};

class VolumeFieldSet {
public:
    // Declare a field the solver holds. `exposed` is the default; SetExposedByName() (driven by
    // config.bakeFields) can override which are copied out.
    void Declare(std::string name, FieldType type, f32 background, bool exposed) {
        decls_.push_back(VolumeFieldDecl{std::move(name), type, background, exposed});
    }

    // Apply config.bakeFields: a declared field is exposed iff its name is in `names`. Unknown names
    // are ignored (a solver simply cannot bake a field it does not hold).
    void SetExposedByName(const std::vector<std::string>& names) {
        for (VolumeFieldDecl& d : decls_) {
            bool on = false;
            for (const std::string& n : names)
                if (n == d.name) { on = true; break; }
            d.exposed = on;
        }
    }

    // Capability summary: OR of the standard-field bits for ALL declared fields (not just exposed).
    FieldMask AvailableFields() const {
        FieldMask m = FieldMask::None;
        for (const VolumeFieldDecl& d : decls_) m = m | MaskForFieldName(d.name);
        return m;
    }

    const std::vector<VolumeFieldDecl>& Decls() const { return decls_; }

    // Find a declared field by name (nullptr if absent).
    const VolumeFieldDecl* Find(std::string_view name) const {
        for (const VolumeFieldDecl& d : decls_)
            if (d.name == name) return &d;
        return nullptr;
    }

private:
    std::vector<VolumeFieldDecl> decls_;
};

} // namespace hbe::volume
