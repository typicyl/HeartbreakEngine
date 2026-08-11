// Scene/OceanFFT.h - Tessendorf FFT ocean: CPU reference + shared parameters.
//
// This header owns the WATER-SPECTRUM MATH that both the CPU reference oracle and the
// GPU compute path (OceanSpectrum.hlsl / OceanFFT.hlsl) implement. The CPU class here is
// the ground truth: `ocean::SelfTest` validates the FFT + the Hermitian construction
// numerically, and `--test-oceanfft` later diffs the GPU output against `CpuOcean` (via
// ReadGpuBuffer). Because the engine is headless-verified only, this oracle is how a
// blind GPU FFT is proven correct rather than merely "boots clean".
//
// Model (Tessendorf 2001, "Simulating Ocean Water"; the fftWater / Keith-Lantz lineage the
// user referenced):
//   - Phillips spectrum P(k) with a wind direction + small-wave cutoff.
//   - Initial field h0(k) = (1/sqrt2)(xi_r + i xi_i) sqrt(P(k)), xi ~ N(0,1).
//   - Time evolution h~(k,t) = h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t}, w = sqrt(g|k|)
//     (deep-water dispersion). Built to be Hermitian, so the IFFT height field is REAL.
//   - IFFT h~ -> height; IFFT of (-i kx/|k|) h~, (-i kz/|k|) h~ -> choppy XZ displacement.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"

#include <glm/glm.hpp>

#include <complex>
#include <string>
#include <vector>

namespace hbe {
class Renderer;
class Scene;
}

namespace hbe::ocean {

// One ocean patch's spectrum parameters. gridN must be a power of two. These map 1:1 to
// the GPU spectrum CB so the CPU oracle and the GPU kernel evaluate the same field.
struct OceanParams {
    u32 gridN = 128;          // FFT resolution N (power of two; the tile is N x N)
    f32 patchSize = 128.0f;   // world metres the tile spans (L); wave numbers = 2pi n / L
    f32 windSpeed = 18.0f;    // wind speed V (m/s) - sets the dominant wavelength V^2/g
    f32 windDirRad = 0.6f;    // wind direction in the XZ plane (radians)
    f32 amplitude = 4.0f;     // overall wave-height scale (Phillips A, post-normalised)
    f32 choppiness = 1.0f;    // lambda: horizontal-displacement strength (0 = round swells)
    f32 gravity = 9.81f;      // g
    f32 smallWave = 0.4f;     // small-wave cutoff length l (m) - suppresses tiny ripples
    u32 seed = 1337u;         // RNG seed for h0 (deterministic)
};

// CPU reference ocean. Init() precomputes h0(k), conj(h0(-k)) and the dispersion; Evolve()
// produces the per-texel displacement field for a given time. This is the ORACLE - it
// favours clarity over speed (the runtime path is the GPU compute FFT, not this).
class CpuOcean {
public:
    void Init(const OceanParams& p);

    // Fill `out` (size N*N, row-major idx = z*N + x) with the surface field at time t:
    //   .x = horizontal displacement X (metres, already * choppiness)
    //   .y = vertical displacement / height (metres)
    //   .z = horizontal displacement Z (metres, already * choppiness)
    //   .w = Jacobian foam factor (0 = flat, ->1 where the surface folds / whitecaps)
    void Evolve(f32 t, std::vector<glm::vec4>& out) const;

    const OceanParams& Params() const { return p_; }
    u32 N() const { return N_; }

    // Pack the initial field for GPU upload: out[idx] = (h0.re, h0.im, h0mk.re, h0mk.im),
    // row-major idx = z*N + x. This is what OceanSpectrum.hlsl reads (t0).
    void PackH0(std::vector<glm::vec4>& out) const;

private:
    OceanParams p_{};
    u32 N_ = 0;
    std::vector<std::complex<f32>> h0_;   // h0(k)
    std::vector<std::complex<f32>> h0mk_; // conj(h0(-k)) - the Hermitian partner
    std::vector<f32> omega_;              // dispersion w(k) per cell
};

// GPU compute FFT ocean driver. Owns the compute buffers + pipelines and queues the
// per-frame dispatch chain (evolve spectrum -> IFFT rows -> IFFT cols -> assemble). The
// resulting displacement buffer (float4 Dx/height/Dz per texel, row-major) is what the
// water VS samples via SetVertexShaderBuffer. The GPU FFT is FIXED at N=256 (the groupshared
// kernel is compile-time sized); Init forces it and warns if params ask for another size.
//
// Correctness is proven headless: --test-oceanfft-gpu runs this against CpuOcean and diffs
// the readback. The kernels replicate CpuOcean's math exactly so that diff is meaningful.
class GpuOcean {
public:
    static constexpr u32 kGpuN = 256; // OceanFFT.hlsl OCEAN_N

    bool Init(Renderer& renderer, const OceanParams& p);
    void Shutdown(Renderer& renderer);
    // Recompute h0 for new spectrum parameters WITHOUT reallocating any GPU resources (the
    // buffers are size-fixed at N=256 and the pipelines are param-independent). This is the
    // per-parameter-change path - cheap enough to run on a slider drag, unlike Init.
    void Rebake(const OceanParams& p);
    // Fill h0 + queue this frame's compute chain. Call before Renderer::RenderScene (like
    // QueueCompute). No-op until Init succeeds.
    void Update(Renderer& renderer, f32 time);

    rhi::GpuBufferHandle DisplacementBuffer() const { return disp_; }
    u32 N() const { return N_; }
    bool Valid() const { return valid_; }
    const OceanParams& Params() const { return params_; }

private:
    OceanParams params_{};
    u32 N_ = 0;
    bool valid_ = false;
    std::vector<glm::vec4> h0Pack_; // cached (h0, h0mk); re-uploaded each frame (static data)
    rhi::GpuBufferHandle h0_, specH_, specDX_, specDZ_, disp_;
    rhi::ComputePipelineHandle specPipe_, fftPipe_, asmPipe_;
};

// --- Shared FFT primitives (exposed for the self-test) ----------------------------
// In-place iterative radix-2 Cooley-Tukey. `inverse` uses e^{+i} and divides by n, so
// Ifft(Fft(a)) == a. n must be a power of two.
void Fft1D(std::complex<f32>* a, u32 n, bool inverse);
// Separable 2D transform over a row-major NxN grid (rows then columns).
void Fft2D(std::vector<std::complex<f32>>& grid, u32 N, bool inverse);

// Drive the FFT ocean for a scene: find the first WaterComponent with fftOcean on, (re)build
// the GpuOcean when its parameters change, queue this frame's compute, and return the
// displacement buffer to bind for the water VS. Returns an invalid handle (and shuts the
// GpuOcean down) when no water wants the FFT or GPU compute is unavailable - the caller then
// binds nothing and the water falls back to Gerstner. `time` is the shared wave clock.
rhi::GpuBufferHandle UpdateForScene(GpuOcean& ocean, Scene& scene, Renderer& renderer, f32 time);

// Numerical self-test (no GPU): FFT round-trip, that the evolved height field is real
// (Hermitian construction), and determinism. Returns true on pass; `report` gets a
// human-readable line either way.
bool SelfTest(std::string& report);

} // namespace hbe::ocean
