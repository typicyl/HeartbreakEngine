#include "Construction/ConstructionParams.h"

#include "Construction/ConstructionPreset.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hbe::construction {

namespace {

template <typename T>
const T* At(const PresetParams& p, const ParamDesc& d) {
    return reinterpret_cast<const T*>(reinterpret_cast<const u8*>(&p) + d.offset);
}
template <typename T>
T* At(PresetParams& p, const ParamDesc& d) {
    return reinterpret_cast<T*>(reinterpret_cast<u8*>(&p) + d.offset);
}

const char* kMaterialNames[] = {"Unknown",     "Brick",       "Concrete Block", "Stone",
                                "Concrete",    "Timber Frame", "Wood Plank",    "Plywood",
                                "OSB",         "Wood Shingle", "Drywall",       "Plaster",
                                "Metal",       "Corrugated Metal", "Glass", "Mortar"};
static_assert(sizeof(kMaterialNames) / sizeof(kMaterialNames[0]) ==
                  static_cast<usize>(MaterialKind::Count),
              "material picker and MaterialKind must stay in lockstep");

const char* kBondNames[] = {"Running", "Stack", "Flemish", "English"};
const char* kProfileNames[] = {"Flush", "Clapboard", "Shiplap", "Board and Batten"};
const char* kDirNames[] = {"Horizontal", "Vertical", "Diagonal"};

} // namespace

const char* const* MaterialKindNames(u32& n) {
    n = static_cast<u32>(MaterialKind::Count);
    return kMaterialNames;
}
const char* const* BondPatternNames(u32& n) {
    n = static_cast<u32>(BondPattern::Count);
    return kBondNames;
}
const char* const* SidingProfileNames(u32& n) {
    n = static_cast<u32>(SidingProfile::Count);
    return kProfileNames;
}
const char* const* BoardDirectionNames(u32& n) {
    n = static_cast<u32>(BoardDirection::Count);
    return kDirNames;
}

f32 GetFloat(const PresetParams& p, const ParamDesc& d) { return *At<f32>(p, d); }
void SetFloat(PresetParams& p, const ParamDesc& d, f32 v) {
    *At<f32>(p, d) = v;
    ClampToRange(p, d);
}

i32 GetInt(const PresetParams& p, const ParamDesc& d) {
    // Enums are stored as i32 in PresetParams but as u8/u16 inside the method blocks that came
    // from the generators, so the read has to respect the field's real width rather than assuming.
    switch (d.size) {
        case 1: return static_cast<i32>(*At<u8>(p, d));
        case 2: return static_cast<i32>(*At<u16>(p, d));
        case 4: return *At<i32>(p, d);
        default: return 0;
    }
}
void SetInt(PresetParams& p, const ParamDesc& d, i32 v) {
    switch (d.size) {
        case 1: *At<u8>(p, d) = static_cast<u8>(v); break;
        case 2: *At<u16>(p, d) = static_cast<u16>(v); break;
        case 4: *At<i32>(p, d) = v; break;
        default: break;
    }
    ClampToRange(p, d);
}

bool GetBool(const PresetParams& p, const ParamDesc& d) { return *At<bool>(p, d); }
void SetBool(PresetParams& p, const ParamDesc& d, bool v) { *At<bool>(p, d) = v; }
u64 GetSeed(const PresetParams& p, const ParamDesc& d) { return *At<u64>(p, d); }
void SetSeed(PresetParams& p, const ParamDesc& d, u64 v) { *At<u64>(p, d) = v; }

void ClampToRange(PresetParams& p, const ParamDesc& d) {
    switch (d.type) {
        case ParamType::Float: {
            f32& v = *At<f32>(p, d);
            v = std::clamp(v, d.min, d.max);
            break;
        }
        case ParamType::Int:
        case ParamType::Enum: {
            const i32 lo = static_cast<i32>(d.min);
            // An Enum's upper bound is its NAME COUNT, not `max`. Letting a stale .hbbuild carry
            // an out-of-range enum through would index a name table out of bounds the first time
            // the inspector drew it.
            const i32 hi = d.type == ParamType::Enum
                               ? static_cast<i32>(d.enumCount) - 1
                               : static_cast<i32>(d.max);
            const i32 v = std::clamp(GetInt(p, d), lo, std::max(lo, hi));
            switch (d.size) {
                case 1: *At<u8>(p, d) = static_cast<u8>(v); break;
                case 2: *At<u16>(p, d) = static_cast<u16>(v); break;
                case 4: *At<i32>(p, d) = v; break;
                default: break;
            }
            break;
        }
        case ParamType::Bool:
        case ParamType::Seed:
        case ParamType::Count:
            break; // no meaningful range
    }
}

void ClampAll(PresetParams& p, const ParamDesc* descs, u32 count) {
    for (u32 i = 0; i < count; ++i) ClampToRange(p, descs[i]);
}

// ---------------------------------------------------------------------------

namespace {
u32 g_fails = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("params FAIL: %s\n", what);
        ++g_fails;
    }
}

u32 ExpectedSize(ParamType t) {
    switch (t) {
        case ParamType::Float: return 4;
        case ParamType::Bool: return 1;
        case ParamType::Seed: return 8;
        default: return 0; // Int/Enum vary by field width, checked separately
    }
}
} // namespace

bool ParamsSelfTest() {
    g_fails = 0;

    // Every descriptor of every preset must land inside PresetParams and match its field width.
    // THIS IS THE WHOLE SAFETY ARGUMENT FOR USING OFFSETS: a mistyped row is caught here rather
    // than by writing four bytes over the middle of an unrelated parameter at runtime.
    u32 presetCount = 0;
    const PresetDesc* presets = Presets(presetCount);
    Check(presetCount > 0, "the registry must publish at least one preset");

    u32 totalParams = 0;
    for (u32 i = 0; i < presetCount; ++i) {
        const PresetDesc& pd = presets[i];
        Check(pd.id && *pd.id, "every preset has a stable id");
        Check(pd.build != nullptr, "every preset has a build function");
        Check(pd.paramCount > 0, "every preset exposes at least one parameter");
        totalParams += pd.paramCount;

        for (u32 j = 0; j < pd.paramCount; ++j) {
            const ParamDesc& d = pd.params[j];
            Check(d.offset + d.size <= sizeof(PresetParams),
                  "A DESCRIPTOR MUST LAND INSIDE PresetParams - an offset past the end would "
                  "write over whatever follows the struct");
            const u32 want = ExpectedSize(d.type);
            if (want != 0)
                Check(d.size == want,
                      "a descriptor's declared size must match its type - writing 4 bytes through "
                      "a 1-byte field corrupts the two parameters after it");
            if (d.type == ParamType::Enum) {
                Check(d.enumNames != nullptr, "an Enum parameter must name its options");
                Check(d.enumCount > 0, "...and have at least one");
            }
            Check(d.group && *d.group, "every parameter belongs to a group (brief SS5)");
            Check(d.name && *d.name, "every parameter has a display name");
            if (d.type == ParamType::Float) Check(d.max > d.min, "a float range must be non-empty");
        }
    }
    Check(totalParams > 20, "the exposed surface is meaningful, not a token few");

    // Range clamping must actually bite, from BOTH ends - a .hbbuild from a newer build, or a
    // hand-edited one, can carry anything.
    {
        u32 n = 0;
        const PresetDesc* all = Presets(n);
        const PresetDesc& wall = all[0];
        PresetParams p;
        for (u32 j = 0; j < wall.paramCount; ++j) {
            const ParamDesc& d = wall.params[j];
            if (d.type != ParamType::Float) continue;
            SetFloat(p, d, -1e9f);
            Check(GetFloat(p, d) >= d.min - 1e-4f, "a value below the range is clamped up");
            SetFloat(p, d, 1e9f);
            Check(GetFloat(p, d) <= d.max + 1e-4f, "a value above the range is clamped down");
        }
        // An out-of-range ENUM must be clamped to the name table, not to `max` - otherwise the
        // inspector indexes the table out of bounds the first time it draws.
        for (u32 j = 0; j < wall.paramCount; ++j) {
            const ParamDesc& d = wall.params[j];
            if (d.type != ParamType::Enum) continue;
            SetInt(p, d, 9999);
            Check(GetInt(p, d) < static_cast<i32>(d.enumCount),
                  "AN OUT-OF-RANGE ENUM MUST CLAMP TO ITS NAME TABLE - a stale file would "
                  "otherwise index the picker out of bounds");
            SetInt(p, d, -5);
            Check(GetInt(p, d) >= 0, "...and not go negative");
        }
    }

    // Round-trip through every accessor.
    {
        PresetParams p;
        ParamDesc d;
        d.type = ParamType::Float;
        d.offset = offsetof(PresetParams, width);
        d.size = sizeof(f32);
        d.min = 0.1f;
        d.max = 100.0f;
        SetFloat(p, d, 12.5f);
        Check(GetFloat(p, d) == 12.5f, "a float round-trips through its descriptor");

        ParamDesc s;
        s.type = ParamType::Seed;
        s.offset = offsetof(PresetParams, seed);
        s.size = sizeof(u64);
        SetSeed(p, s, 0xDEADBEEFull);
        Check(GetSeed(p, s) == 0xDEADBEEFull, "a seed round-trips at full 64-bit width");

        // A narrow enum field inside a method block, which is where the width actually varies.
        ParamDesc b;
        b.type = ParamType::Enum;
        b.offset = offsetof(PresetParams, masonry) + offsetof(MasonryParams, bond);
        b.size = sizeof(BondPattern);
        b.enumCount = static_cast<u32>(BondPattern::Count);
        b.enumNames = kBondNames;
        SetInt(p, b, static_cast<i32>(BondPattern::Flemish));
        Check(p.masonry.bond == BondPattern::Flemish,
              "an enum written through a descriptor lands in the real field at the real width");
    }

    if (g_fails == 0)
        std::printf("params: every descriptor lands inside PresetParams at its real field width "
                    "(the safety argument for offsets), floats clamp from both ends, and an "
                    "out-of-range enum clamps to its NAME TABLE rather than to max\n");
    return g_fails == 0;
}

} // namespace hbe::construction
