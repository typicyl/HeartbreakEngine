// Vegetation/FastNoise2Field.cpp - FastNoise2 (SIMD) INoiseField backend.
//
// Wrapped so NO FastNoise2 type crosses an engine header (exactly like meshoptimizer /
// Jolt / Detour). The owner chose FastNoise2 (MIT, SIMD, runtime AVX2/NEON dispatch) as
// the default noise; it stays behind INoiseField so it is a swappable backend, never
// load-bearing. When FastNoise2 is not compiled in (HBE_HAVE_FASTNOISE2=0), the maker
// returns nullptr and the built-in value noise remains the default.
#include "Vegetation/VegetationBackends.h"
#include "Vegetation/VegetationInterfaces.h"

#include <memory>

#if HBE_HAVE_FASTNOISE2
#include <FastNoise/FastNoise.h>
#endif

namespace hbe::veg {

#if HBE_HAVE_FASTNOISE2

namespace {

// A fractal (FBm) Simplex field. FastNoise2's SmartNode graph is built once and sampled
// per call with an explicit seed, so it stays deterministic and thread-safe.
class FastNoise2Field final : public INoiseField {
public:
    FastNoise2Field() {
        auto simplex = FastNoise::New<FastNoise::Simplex>();
        auto fractal = FastNoise::New<FastNoise::FractalFBm>();
        fractal->SetSource(simplex);
        fractal->SetOctaveCount(4);
        node_ = fractal;
    }
    const char* Name() const override { return "fastnoise2"; }

    f32 Sample(f32 x, f32 z, i32 seed) const override {
        // FastNoise2 returns roughly [-1,1]; remap to [0,1) to match INoiseField's
        // contract (density fields want a positive amplitude).
        const float v = node_->GenSingle2D(x, z, seed);
        float u = v * 0.5f + 0.5f;
        if (u < 0.0f) u = 0.0f;
        if (u >= 1.0f) u = 0.99999994f;
        return u;
    }

private:
    FastNoise::SmartNode<> node_;
};

} // namespace

std::unique_ptr<INoiseField> MakeFastNoise2Field() {
    return std::make_unique<FastNoise2Field>();
}

#else // !HBE_HAVE_FASTNOISE2

std::unique_ptr<INoiseField> MakeFastNoise2Field() { return nullptr; }

#endif

} // namespace hbe::veg
