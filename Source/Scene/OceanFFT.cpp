// Scene/OceanFFT.cpp - see OceanFFT.h.
#include "Scene/OceanFFT.h"

#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace hbe::ocean {

namespace {
constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 6.28318530717958647692f;
using cf = std::complex<f32>;

// Phillips spectrum P(k): energy per wave vector. Zero at k=0; a wind-direction lobe
// (|k^.w^|^2) plus suppression of waves running against the wind and of sub-cutoff ripples.
f32 Phillips(const OceanParams& p, glm::vec2 k) {
    const f32 k2 = glm::dot(k, k);
    if (k2 < 1e-12f) return 0.0f;
    const f32 L = p.windSpeed * p.windSpeed / p.gravity; // largest wind-driven wave
    const glm::vec2 kh = k / std::sqrt(k2);
    const glm::vec2 w = glm::vec2(std::cos(p.windDirRad), std::sin(p.windDirRad));
    f32 kdotw = glm::dot(kh, w);
    f32 ph = p.amplitude * std::exp(-1.0f / (k2 * L * L)) / (k2 * k2) * (kdotw * kdotw);
    if (kdotw < 0.0f) ph *= 0.07f;                 // waves moving against the wind are damped
    ph *= std::exp(-k2 * p.smallWave * p.smallWave); // kill ripples below the cutoff
    return ph;
}
} // namespace

// --- FFT --------------------------------------------------------------------------
void Fft1D(cf* a, u32 n, bool inverse) {
    // Bit-reversal permutation.
    for (u32 i = 1, j = 0; i < n; ++i) {
        u32 bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    // Butterfly stages.
    for (u32 len = 2; len <= n; len <<= 1) {
        const f32 ang = (inverse ? kTwoPi : -kTwoPi) / static_cast<f32>(len);
        const cf wlen(std::cos(ang), std::sin(ang));
        for (u32 i = 0; i < n; i += len) {
            cf w(1.0f, 0.0f);
            for (u32 k = 0; k < (len >> 1); ++k) {
                const cf u = a[i + k];
                const cf v = a[i + k + (len >> 1)] * w;
                a[i + k] = u + v;
                a[i + k + (len >> 1)] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) {
        const f32 inv = 1.0f / static_cast<f32>(n);
        for (u32 i = 0; i < n; ++i) a[i] *= inv;
    }
}

void Fft2D(std::vector<cf>& grid, u32 N, bool inverse) {
    std::vector<cf> line(N);
    // Rows.
    for (u32 r = 0; r < N; ++r) Fft1D(&grid[static_cast<usize>(r) * N], N, inverse);
    // Columns (gather -> transform -> scatter).
    for (u32 c = 0; c < N; ++c) {
        for (u32 r = 0; r < N; ++r) line[r] = grid[static_cast<usize>(r) * N + c];
        Fft1D(line.data(), N, inverse);
        for (u32 r = 0; r < N; ++r) grid[static_cast<usize>(r) * N + c] = line[r];
    }
}

// --- CpuOcean ---------------------------------------------------------------------
void CpuOcean::Init(const OceanParams& p) {
    p_ = p;
    N_ = std::max(2u, p.gridN);
    const u32 N = N_;
    h0_.assign(static_cast<usize>(N) * N, cf(0.0f, 0.0f));
    h0mk_.assign(static_cast<usize>(N) * N, cf(0.0f, 0.0f));
    omega_.assign(static_cast<usize>(N) * N, 0.0f);

    std::mt19937 rng(p.seed);
    std::normal_distribution<f32> gauss(0.0f, 1.0f);
    const f32 invSqrt2 = 0.70710678f;
    const f32 halfN = static_cast<f32>(N) * 0.5f;

    // h0(k): Gaussian amplitude shaped by sqrt(Phillips). Cell (i=x, j=z) -> centred wave
    // number n = i - N/2, m = j - N/2, so k = 2pi (n, m) / L.
    for (u32 j = 0; j < N; ++j) {
        for (u32 i = 0; i < N; ++i) {
            const glm::vec2 k(kTwoPi * (static_cast<f32>(i) - halfN) / p.patchSize,
                              kTwoPi * (static_cast<f32>(j) - halfN) / p.patchSize);
            const f32 amp = std::sqrt(Phillips(p, k)) * invSqrt2;
            h0_[static_cast<usize>(j) * N + i] = cf(gauss(rng) * amp, gauss(rng) * amp);
            omega_[static_cast<usize>(j) * N + i] = std::sqrt(p.gravity * glm::length(k));
        }
    }
    // conj(h0(-k)): -k is the mirrored cell (N-i)%N, (N-j)%N. Taken from the SAME draws as
    // h0 (not an independent draw) so h~(k,t) is exactly Hermitian and the height is real.
    for (u32 j = 0; j < N; ++j)
        for (u32 i = 0; i < N; ++i) {
            const u32 mi = (N - i) % N, mj = (N - j) % N;
            h0mk_[static_cast<usize>(j) * N + i] = std::conj(h0_[static_cast<usize>(mj) * N + mi]);
        }
}

void CpuOcean::Evolve(f32 t, std::vector<glm::vec4>& out) const {
    const u32 N = N_;
    const usize cells = static_cast<usize>(N) * N;
    out.assign(cells, glm::vec4(0.0f));
    if (N < 2) return;

    std::vector<cf> H(cells), DX(cells), DZ(cells);
    const f32 halfN = static_cast<f32>(N) * 0.5f;
    const cf negI(0.0f, -1.0f);

    // Evolve the spectrum + build the height and horizontal-displacement spectra.
    for (u32 j = 0; j < N; ++j) {
        for (u32 i = 0; i < N; ++i) {
            const usize idx = static_cast<usize>(j) * N + i;
            const glm::vec2 k(kTwoPi * (static_cast<f32>(i) - halfN) / p_.patchSize,
                              kTwoPi * (static_cast<f32>(j) - halfN) / p_.patchSize);
            const f32 om = omega_[idx] * t;
            const cf e(std::cos(om), std::sin(om));
            const cf ht = h0_[idx] * e + h0mk_[idx] * std::conj(e); // conj(e) = e^{-i om}
            H[idx] = ht;
            const f32 klen = glm::length(k);
            if (klen > 1e-6f) {
                DX[idx] = negI * (k.x / klen) * ht;
                DZ[idx] = negI * (k.y / klen) * ht;
            }
        }
    }

    Fft2D(H, N, /*inverse=*/true);
    Fft2D(DX, N, /*inverse=*/true);
    Fft2D(DZ, N, /*inverse=*/true);

    // Sign-correct the centred grid ((-1)^(i+j) undoes the fftshift) and scale.
    for (u32 j = 0; j < N; ++j) {
        for (u32 i = 0; i < N; ++i) {
            const usize idx = static_cast<usize>(j) * N + i;
            const f32 s = ((i + j) & 1u) ? -1.0f : 1.0f;
            out[idx].x = DX[idx].real() * s * p_.choppiness;
            out[idx].y = H[idx].real() * s;
            out[idx].z = DZ[idx].real() * s * p_.choppiness;
        }
    }

    // Jacobian foam: the surface folds (whitecaps) where the horizontal displacement
    // compresses, i.e. det(J) of the displacement map drops below 1. Finite-difference the
    // stored dx/dz across neighbours (wrapped) in world space.
    const f32 dW = p_.patchSize / static_cast<f32>(N); // metres between texels
    const f32 inv2d = 1.0f / (2.0f * dW);
    for (u32 j = 0; j < N; ++j) {
        for (u32 i = 0; i < N; ++i) {
            const u32 ip = (i + 1) % N, im = (i + N - 1) % N;
            const u32 jp = (j + 1) % N, jm = (j + N - 1) % N;
            const f32 dDxdx = (out[static_cast<usize>(j) * N + ip].x - out[static_cast<usize>(j) * N + im].x) * inv2d;
            const f32 dDzdz = (out[static_cast<usize>(jp) * N + i].z - out[static_cast<usize>(jm) * N + i].z) * inv2d;
            const f32 dDxdz = (out[static_cast<usize>(jp) * N + i].x - out[static_cast<usize>(jm) * N + i].x) * inv2d;
            const f32 dDzdx = (out[static_cast<usize>(j) * N + ip].z - out[static_cast<usize>(j) * N + im].z) * inv2d;
            const f32 Jxx = 1.0f + dDxdx, Jzz = 1.0f + dDzdz;
            const f32 J = Jxx * Jzz - dDxdz * dDzdx;
            out[static_cast<usize>(j) * N + i].w = std::max(0.0f, 1.0f - J); // fold amount
        }
    }
}

void CpuOcean::PackH0(std::vector<glm::vec4>& out) const {
    out.resize(h0_.size());
    for (usize i = 0; i < h0_.size(); ++i)
        out[i] = glm::vec4(h0_[i].real(), h0_[i].imag(), h0mk_[i].real(), h0mk_[i].imag());
}

// --- GpuOcean ---------------------------------------------------------------------
namespace {
struct SpecCB { u32 N; f32 patch; f32 time; f32 gravity; f32 chop; f32 p0, p1, p2; };
struct FftCB { u32 axis; u32 inverse; u32 p0, p1; };
struct AsmCB { u32 N; f32 chop; u32 p0, p1; };
} // namespace

bool GpuOcean::Init(Renderer& renderer, const OceanParams& p) {
    if (!renderer.SupportsGpuCompute()) {
        HBE_WARN("Ocean: backend has no GPU compute; FFT ocean disabled (Gerstner fallback).");
        return false;
    }
    params_ = p;
    if (params_.gridN != kGpuN) {
        HBE_WARN("Ocean: FFT ocean is fixed at N={}; overriding requested gridN={}.", kGpuN,
                 params_.gridN);
        params_.gridN = kGpuN;
    }
    N_ = kGpuN;
    const u32 cells = N_ * N_;

    // Buffers - all size-fixed at N=256, so they are created ONCE and only recreated after a
    // Shutdown (stream-out) freed them; a parameter change goes through Rebake and reallocates
    // nothing. Guarded on IsValid so re-Init after a stream-out reuses what still exists.
    if (!h0_.IsValid()) {
        rhi::GpuBufferDesc h0d{};
        h0d.elementCount = cells;
        h0d.elementStride = sizeof(glm::vec4);
        h0d.usage = rhi::GpuBufferUsage::ShaderRead | rhi::GpuBufferUsage::CpuWrite;
        h0d.debugName = "OceanH0";
        h0_ = renderer.CreateGpuBuffer(h0d);
    }
    auto makeSpec = [&](rhi::GpuBufferHandle& h, const char* name) {
        if (h.IsValid()) return;
        rhi::GpuBufferDesc d{};
        d.elementCount = cells;
        d.elementStride = sizeof(glm::vec2); // complex float2
        d.usage = rhi::GpuBufferUsage::ShaderWrite | rhi::GpuBufferUsage::ShaderRead;
        d.debugName = name;
        h = renderer.CreateGpuBuffer(d);
    };
    makeSpec(specH_, "OceanSpecH");
    makeSpec(specDX_, "OceanSpecDX");
    makeSpec(specDZ_, "OceanSpecDZ");
    if (!disp_.IsValid()) {
        rhi::GpuBufferDesc dd{};
        dd.elementCount = cells;
        dd.elementStride = sizeof(glm::vec4);
        // Written by the assemble pass, read by the water VS via SetVertexShaderBuffer.
        dd.usage = rhi::GpuBufferUsage::ShaderWrite | rhi::GpuBufferUsage::ShaderRead;
        dd.debugName = "OceanDisplacement";
        disp_ = renderer.CreateGpuBuffer(dd);
    }

    // Pipelines are PARAM-INDEPENDENT: created ONCE and kept for the whole GpuOcean lifetime.
    // The RHI has NO DestroyComputePipeline, so Shutdown must not drop them (that would leak a
    // root-sig/PSO per re-bake) - they are reused across every Shutdown/Init and freed only at
    // device teardown.
    if (!specPipe_.IsValid()) {
        rhi::ComputePipelineDesc sp{};
        sp.shaderName = "OceanSpectrum";
        sp.constantBytes = sizeof(SpecCB);
        sp.uavCount = 3;
        sp.srvCount = 1;
        sp.debugName = "OceanSpectrum";
        specPipe_ = renderer.CreateComputePipeline(sp);
    }
    if (!fftPipe_.IsValid()) {
        rhi::ComputePipelineDesc fp{};
        fp.shaderName = "OceanFFT";
        fp.constantBytes = sizeof(FftCB);
        fp.uavCount = 1;
        fp.srvCount = 0;
        fp.debugName = "OceanFFT";
        fftPipe_ = renderer.CreateComputePipeline(fp);
    }
    if (!asmPipe_.IsValid()) {
        rhi::ComputePipelineDesc ap{};
        ap.shaderName = "OceanAssemble";
        ap.constantBytes = sizeof(AsmCB);
        ap.uavCount = 1;
        ap.srvCount = 3;
        ap.debugName = "OceanAssemble";
        asmPipe_ = renderer.CreateComputePipeline(ap);
    }

    valid_ = h0_.IsValid() && specH_.IsValid() && specDX_.IsValid() && specDZ_.IsValid() &&
             disp_.IsValid() && specPipe_.IsValid() && fftPipe_.IsValid() && asmPipe_.IsValid();
    if (!valid_) {
        HBE_ERROR("Ocean: GPU FFT resources/pipelines failed to create; FFT ocean disabled.");
        Shutdown(renderer);
        return false;
    }
    Rebake(params_); // compute + cache the initial h0
    HBE_INFO("Ocean: GPU FFT ready (N={}, patch={:.0f}m).", N_, params_.patchSize);
    return true;
}

void GpuOcean::Rebake(const OceanParams& p) {
    params_ = p;
    params_.gridN = kGpuN;
    // Recompute h0(k) on the CPU for the new spectrum. NO GPU allocation - Update re-uploads
    // h0Pack_ into the ring each frame anyway, so the change takes effect next frame.
    CpuOcean cpu;
    cpu.Init(params_);
    cpu.PackH0(h0Pack_);
}

void GpuOcean::Shutdown(Renderer& renderer) {
    // Free the BUFFERS (reclaims VRAM on stream-out). The pipelines are param-independent and
    // the RHI cannot free them, so they are intentionally KEPT for reuse - nulling them here
    // would orphan a root-sig/PSO (there is no DestroyComputePipeline).
    for (rhi::GpuBufferHandle* h : {&h0_, &specH_, &specDX_, &specDZ_, &disp_})
        if (h->IsValid()) {
            renderer.DestroyGpuBuffer(*h);
            *h = {};
        }
    valid_ = false;
}

void GpuOcean::Update(Renderer& renderer, f32 time) {
    if (!valid_) return;

    // Upload h0 into this frame's ring slot (static data; cheap write-combined memcpy).
    if (void* dst = renderer.MapGpuBuffer(h0_))
        std::memcpy(dst, h0Pack_.data(), h0Pack_.size() * sizeof(glm::vec4));

    const u32 groups2D = (N_ + 7u) / 8u; // numthreads(8,8) in spectrum/assemble

    // 1. Evolve the spectrum: h0 -> (H, DX, DZ) frequency-domain fields.
    {
        const SpecCB cb{N_, params_.patchSize, time, params_.gravity, params_.choppiness, 0, 0, 0};
        rhi::ComputeDispatch d{};
        d.pipeline = specPipe_;
        d.constants = &cb;
        d.constantBytes = sizeof(cb);
        d.uavs[0] = specH_;
        d.uavs[1] = specDX_;
        d.uavs[2] = specDZ_;
        d.uavCount = 3;
        d.srvs[0] = h0_;
        d.srvCount = 1;
        d.groupsX = groups2D;
        d.groupsY = groups2D;
        renderer.QueueCompute(d);
    }

    // 2. IFFT each field: rows then columns. One threadgroup per line (groupsX = N).
    for (rhi::GpuBufferHandle buf : {specH_, specDX_, specDZ_}) {
        for (u32 axis = 0; axis < 2; ++axis) {
            const FftCB cb{axis, /*inverse=*/1u, 0u, 0u};
            rhi::ComputeDispatch d{};
            d.pipeline = fftPipe_;
            d.constants = &cb;
            d.constantBytes = sizeof(cb);
            d.uavs[0] = buf;
            d.uavCount = 1;
            d.groupsX = N_;
            renderer.QueueCompute(d);
        }
    }

    // 3. Assemble: sign-correct + choppiness -> interleaved displacement buffer.
    {
        const AsmCB cb{N_, params_.choppiness, 0u, 0u};
        rhi::ComputeDispatch d{};
        d.pipeline = asmPipe_;
        d.constants = &cb;
        d.constantBytes = sizeof(cb);
        d.uavs[0] = disp_;
        d.uavCount = 1;
        d.srvs[0] = specH_;
        d.srvs[1] = specDX_;
        d.srvs[2] = specDZ_;
        d.srvCount = 3;
        d.groupsX = groups2D;
        d.groupsY = groups2D;
        renderer.QueueCompute(d);
    }
}

namespace {
OceanParams ParamsFromComponent(const WaterComponent& w) {
    OceanParams p;
    p.gridN = GpuOcean::kGpuN;
    p.patchSize = std::max(w.fftPatchSize, 1.0f);
    p.windSpeed = std::max(w.fftWindSpeed, 0.1f);
    p.windDirRad = glm::radians(w.fftWindDir);
    p.amplitude = std::max(w.fftAmplitude, 0.0f);
    p.choppiness = w.fftChoppiness;
    return p;
}
// h0 depends only on these; a change requires a rebuild. choppiness is applied at assemble
// time, so it can change without re-baking h0 - but Init is cheap enough to rebuild on any.
bool SpectrumEqual(const OceanParams& a, const OceanParams& b) {
    return a.patchSize == b.patchSize && a.windSpeed == b.windSpeed &&
           a.windDirRad == b.windDirRad && a.amplitude == b.amplitude && a.seed == b.seed &&
           a.smallWave == b.smallWave;
}
} // namespace

rhi::GpuBufferHandle UpdateForScene(GpuOcean& ocean, Scene& scene, Renderer& renderer, f32 time) {
    const WaterComponent* wc = nullptr;
    for (const entt::entity e : scene.Registry().view<const WaterComponent>()) {
        const WaterComponent& w = scene.Registry().get<const WaterComponent>(e);
        if (w.fftOcean) {
            wc = &w;
            break; // first FFT water wins (global scene ocean, like the Gerstner path)
        }
    }
    if (!wc) {
        if (ocean.Valid()) ocean.Shutdown(renderer);
        return {};
    }
    const OceanParams want = ParamsFromComponent(*wc);
    if (!ocean.Valid()) {
        if (!ocean.Init(renderer, want)) return {}; // no compute -> Gerstner fallback
    } else if (!SpectrumEqual(want, ocean.Params()) || want.choppiness != ocean.Params().choppiness) {
        // Any parameter change: recompute h0 + params in place. NO GPU realloc / pipeline
        // rebuild / idle stall, so it is safe to fire every frame during a slider drag.
        ocean.Rebake(want);
    }
    ocean.Update(renderer, time);
    return ocean.DisplacementBuffer();
}

// --- Self-test --------------------------------------------------------------------
bool SelfTest(std::string& report) {
    char buf[512];

    // 1. FFT round-trip: Ifft(Fft(a)) == a for random complex data.
    {
        const u32 n = 256;
        std::mt19937 rng(9001);
        std::uniform_real_distribution<f32> uni(-1.0f, 1.0f);
        std::vector<cf> a(n), b;
        for (u32 i = 0; i < n; ++i) a[i] = cf(uni(rng), uni(rng));
        b = a;
        Fft1D(b.data(), n, false);
        Fft1D(b.data(), n, true);
        f32 maxErr = 0.0f;
        for (u32 i = 0; i < n; ++i) maxErr = std::max(maxErr, std::abs(b[i] - a[i]));
        if (maxErr > 1e-3f) {
            std::snprintf(buf, sizeof(buf), "FAIL: 1D FFT round-trip err %.2e (> 1e-3)", maxErr);
            report = buf;
            return false;
        }
    }
    // 2. 2D round-trip.
    {
        const u32 N = 64;
        std::mt19937 rng(4242);
        std::uniform_real_distribution<f32> uni(-1.0f, 1.0f);
        std::vector<cf> a(static_cast<usize>(N) * N), b;
        for (auto& v : a) v = cf(uni(rng), uni(rng));
        b = a;
        Fft2D(b, N, false);
        Fft2D(b, N, true);
        f32 maxErr = 0.0f;
        for (usize i = 0; i < a.size(); ++i) maxErr = std::max(maxErr, std::abs(b[i] - a[i]));
        if (maxErr > 1e-3f) {
            std::snprintf(buf, sizeof(buf), "FAIL: 2D FFT round-trip err %.2e (> 1e-3)", maxErr);
            report = buf;
            return false;
        }
    }
    // 3. Evolved height field must be REAL (Hermitian construction). Re-run the evolution
    //    keeping the imaginary part and confirm it is ~0 relative to the real amplitude.
    {
        OceanParams p;
        p.gridN = 64;
        CpuOcean ocean;
        ocean.Init(p);
        const u32 N = p.gridN;
        // Reproduce the internal height spectrum + IFFT, but inspect the imaginary residual.
        std::vector<glm::vec4> field;
        ocean.Evolve(1.7f, field);
        f32 maxAbsH = 0.0f;
        for (auto& v : field) maxAbsH = std::max(maxAbsH, std::abs(v.y));
        if (!(maxAbsH > 0.0f) || !std::isfinite(maxAbsH)) {
            std::snprintf(buf, sizeof(buf), "FAIL: height field is zero/NaN (maxAbsH=%.3e)", maxAbsH);
            report = buf;
            return false;
        }
        // Real MEASUREMENT of the imaginary residual: rebuild the COMPLEX height spectrum from
        // the public h0 pack, evolve it, IFFT (keeping the imaginary channel Evolve discards),
        // and assert |imag| ~ 0. This actually exercises the Hermitian construction - a wrong
        // mirror index or an independent h0mk draw leaves a large imaginary part here. (Checking
        // that the DFT of the already-realified height is Hermitian would be a tautology: the
        // DFT of ANY real array is Hermitian by identity, so it can't catch a broken partner.)
        std::vector<glm::vec4> pack;
        ocean.PackH0(pack);
        std::vector<cf> H(static_cast<usize>(N) * N);
        const f32 halfN = static_cast<f32>(N) * 0.5f;
        for (u32 j = 0; j < N; ++j)
            for (u32 i = 0; i < N; ++i) {
                const usize idx = static_cast<usize>(j) * N + i;
                const glm::vec2 k(6.2831853f * (static_cast<f32>(i) - halfN) / p.patchSize,
                                  6.2831853f * (static_cast<f32>(j) - halfN) / p.patchSize);
                const f32 om = std::sqrt(p.gravity * glm::length(k)) * 1.7f; // same t as Evolve above
                const cf e(std::cos(om), std::sin(om));
                const cf h0(pack[idx].x, pack[idx].y), h0mk(pack[idx].z, pack[idx].w);
                H[idx] = h0 * e + h0mk * std::conj(e);
            }
        Fft2D(H, N, /*inverse=*/true);
        f32 maxImag = 0.0f, maxReal = 1e-8f;
        for (const cf& c : H) {
            maxImag = std::max(maxImag, std::abs(c.imag()));
            maxReal = std::max(maxReal, std::abs(c.real()));
        }
        if (maxImag / maxReal > 1e-3f) {
            std::snprintf(buf, sizeof(buf), "FAIL: height not real (Hermitian broken), |imag|/|real| %.2e",
                          maxImag / maxReal);
            report = buf;
            return false;
        }
    }
    // 4. Determinism: same seed -> identical field.
    {
        OceanParams p;
        p.gridN = 32;
        CpuOcean a, b;
        a.Init(p);
        b.Init(p);
        std::vector<glm::vec4> fa, fb;
        a.Evolve(2.5f, fa);
        b.Evolve(2.5f, fb);
        f32 maxErr = 0.0f;
        for (usize i = 0; i < fa.size(); ++i)
            maxErr = std::max(maxErr, glm::length(glm::vec3(fa[i]) - glm::vec3(fb[i])));
        if (maxErr > 1e-6f) {
            std::snprintf(buf, sizeof(buf), "FAIL: not deterministic, err %.2e", maxErr);
            report = buf;
            return false;
        }
    }

    report = "ok: FFT round-trip (1D+2D), Hermitian-real height, determinism all pass";
    return true;
}

} // namespace hbe::ocean
