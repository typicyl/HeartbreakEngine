// OceanSpectrum.hlsl - Tessendorf ocean: evolve the spectrum for this frame.
//
// Pass 1 of the GPU FFT ocean (see OceanFFT.h for the model). Reads the precomputed,
// time-INDEPENDENT initial field h0(k) + conj(h0(-k)) (packed float4 per cell, uploaded
// from the CPU) and produces the three frequency-domain fields the IFFT then transforms
// to the spatial surface:
//   gH  = h~(k,t) = h0 e^{i w t} + conj(h0(-k)) e^{-i w t}   (height)
//   gDX = -i (kx/|k|) h~   (choppy X displacement spectrum)
//   gDZ = -i (kz/|k|) h~   (choppy Z displacement spectrum)
// w(k) = sqrt(g |k|) is the deep-water dispersion. This is the exact math CpuOcean::Evolve
// runs, so the --test-oceanfft-gpu readback diff isolates only the FFT itself.
//
// Binding convention (ComputePipelineDesc / GpuComputeTest.hlsl): cbuffer b0, then UAVs,
// then SRVs, each with an explicit [[vk::binding]].
[[vk::binding(0, 0)]]
cbuffer SpecCB : register(b0) {
    uint  gN;          // grid resolution N (tile is N x N)
    float gPatch;      // patch size L in metres
    float gTime;       // seconds
    float gGravity;    // g
    float gChoppiness; // unused here (assemble applies it); kept for CB symmetry
    float gPad0;
    float gPad1;
    float gPad2;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<float2> gH  : register(u0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float2> gDX : register(u1);
[[vk::binding(3, 0)]] RWStructuredBuffer<float2> gDZ : register(u2);
[[vk::binding(4, 0)]] StructuredBuffer<float4>   gH0 : register(t0); // (h0.re,h0.im,h0mk.re,h0mk.im)

float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gN || id.y >= gN) return;
    const uint idx = id.y * gN + id.x;
    const float halfN = gN * 0.5f;
    const float2 k = float2(6.28318530718f * (id.x - halfN) / gPatch,
                            6.28318530718f * (id.y - halfN) / gPatch);
    const float klen = length(k);

    const float w = sqrt(gGravity * klen) * gTime;
    const float2 e = float2(cos(w), sin(w));      // e^{i w t}
    const float2 ec = float2(e.x, -e.y);          // conj(e) = e^{-i w t}
    const float4 h0p = gH0[idx];
    const float2 ht = CMul(h0p.xy, e) + CMul(h0p.zw, ec);
    gH[idx] = ht;

    // Horizontal displacement spectra: -i * (k/|k|) * ht.  -i*(c) = (c.y, -c.x).
    if (klen > 1e-6f) {
        const float2 tx = ht * (k.x / klen);
        const float2 tz = ht * (k.y / klen);
        gDX[idx] = float2(tx.y, -tx.x);
        gDZ[idx] = float2(tz.y, -tz.x);
    } else {
        gDX[idx] = float2(0.0f, 0.0f);
        gDZ[idx] = float2(0.0f, 0.0f);
    }
}
