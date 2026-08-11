// OceanAssemble.hlsl - pack the three IFFT'd spatial fields into the water VS's buffer.
//
// Pass 4 (final) of the GPU FFT ocean. After the row+column IFFTs, gH/gDX/gDZ hold the
// spatial height and horizontal-displacement fields (real part in .x; the imaginary part is
// ~0 by the Hermitian construction). Apply the fftshift sign correction (-1)^(x+z) - the
// wave-number grid is centred on 0 - scale the horizontal displacement by choppiness, and
// write the interleaved float4 the water VS samples: (Dx, height, Dz, 0). The foam/Jacobian
// term is left 0 here and reconstructed in the VS from neighbour taps.
//
// Matches CpuOcean::Evolve's final loop exactly (same sign, same choppiness).
[[vk::binding(0, 0)]]
cbuffer AsmCB : register(b0) {
    uint  gN;
    float gChoppiness;
    uint  gPad0;
    uint  gPad1;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gDisp : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float2>   gH    : register(t0);
[[vk::binding(3, 0)]] StructuredBuffer<float2>   gDX   : register(t1);
[[vk::binding(4, 0)]] StructuredBuffer<float2>   gDZ   : register(t2);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gN || id.y >= gN) return;
    const uint idx = id.y * gN + id.x;
    const float sgn = ((id.x + id.y) & 1u) ? -1.0f : 1.0f;
    const float h = gH[idx].x * sgn;
    const float dx = gDX[idx].x * sgn * gChoppiness;
    const float dz = gDZ[idx].x * sgn * gChoppiness;
    gDisp[idx] = float4(dx, h, dz, 0.0f);
}
