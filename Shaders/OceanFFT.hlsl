// OceanFFT.hlsl - one 1D radix-2 IFFT per threadgroup, in shared memory.
//
// Pass 2/3 of the GPU FFT ocean. A separable 2D IFFT = an IFFT along every ROW, then along
// every COLUMN. Each threadgroup transforms ONE line (row or column) of N complex samples
// entirely in groupshared memory - the whole log2(N) butterfly cascade in a SINGLE dispatch
// - so a full N x N IFFT costs just 2 dispatches, not 2*log2(N). This is what keeps the
// ocean under the 16-dispatch/frame budget (kMaxQueuedComputeDispatches).
//
// The convention MUST match CpuOcean's Fft1D exactly (decimation-in-time: bit-reversed load,
// then stages len=2..N with twiddle e^{+i 2pi k/len} for the inverse, and a final 1/N on the
// inverse) or the --test-oceanfft-gpu readback diff against the CPU oracle fails.
//
// Fixed size: N = 256 (LOGN = 8). numthreads = N/2 = 128, one butterfly per thread per stage.
#define OCEAN_N 256u
#define OCEAN_LOGN 8u

[[vk::binding(0, 0)]]
cbuffer FftCB : register(b0) {
    uint gAxis;    // 0 = transform rows, 1 = transform columns
    uint gInverse; // 1 = inverse (e^{+i}, 1/N); 0 = forward
    uint gPad0;
    uint gPad1;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<float2> gData : register(u0);

groupshared float2 sh[OCEAN_N];

float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

// Reverse the low 8 bits (N = 256).
uint BitRev8(uint x) {
    x = ((x & 0x55u) << 1) | ((x & 0xAAu) >> 1);
    x = ((x & 0x33u) << 2) | ((x & 0xCCu) >> 2);
    x = ((x & 0x0Fu) << 4) | ((x & 0xF0u) >> 4);
    return x & 0xFFu;
}

// Global buffer index of position p along this group's line (`line` is a reserved HLSL
// geometry-primitive keyword, so the line index is `ln`).
uint GIndex(uint ln, uint p) { return (gAxis == 0u) ? (ln * OCEAN_N + p) : (p * OCEAN_N + ln); }

[numthreads(OCEAN_N / 2, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint ln = gid.x; // which row/column (0..N-1)
    const uint t = tid.x;  // 0..N/2-1

    // Decimation-in-time load: element at position p goes to shared slot bitrev(p).
    const uint p0 = 2u * t, p1 = 2u * t + 1u;
    sh[BitRev8(p0)] = gData[GIndex(ln, p0)];
    sh[BitRev8(p1)] = gData[GIndex(ln, p1)];
    GroupMemoryBarrierWithGroupSync();

    // log2(N) butterfly stages; each thread does exactly one butterfly per stage.
    const float twoPi = gInverse != 0u ? 6.28318530718f : -6.28318530718f;
    [unroll] for (uint s = 1u; s <= OCEAN_LOGN; ++s) {
        const uint m = 1u << s;   // butterfly span
        const uint hm = m >> 1;   // half span
        const uint grp = t / hm;  // which butterfly group
        const uint kk = t % hm;   // index within the group
        const uint i0 = grp * m + kk;
        const uint i1 = i0 + hm;
        const float ang = twoPi * (float)kk / (float)m;
        const float2 w = float2(cos(ang), sin(ang));
        const float2 a = sh[i0];
        const float2 b = CMul(w, sh[i1]);
        sh[i0] = a + b;
        sh[i1] = a - b;
        GroupMemoryBarrierWithGroupSync();
    }

    // Natural-order output; inverse divides by N (per 1D pass, so a 2D IFFT gets 1/N^2).
    const float norm = (gInverse != 0u) ? (1.0f / (float)OCEAN_N) : 1.0f;
    gData[GIndex(ln, p0)] = sh[p0] * norm;
    gData[GIndex(ln, p1)] = sh[p1] * norm;
}
