// Shaders/VolumeSplat.hlsl - volumetric density/temperature splat (compute).
//
// One thread per voxel; each accumulates a smooth density + density-weighted
// temperature from every blob (particle) whose radius covers it. No atomics /
// no separate clear pass (each voxel writes its own final value), so it's
// deterministic and race-free. Cost = voxels x blobs, kept in budget by lowering
// the volume resolution + blob count on lower-end GPUs (the quality knob, VV5).
// Output: R = density, G = temperature (0..1). Read by the raymarch (VV4).
struct Blob {
    float3 pos;
    float  radius;
    float  density;
    float  temperature;
    float2 pad;
};

// [[vk::binding(b,set)]] pins the Vulkan bindings (set 0) to match the compute
// descriptor-set layout; DXC ignores these for the DXIL (D3D12) compile.
[[vk::binding(0, 0)]] cbuffer VolumeCB : register(b0) {
    float3 gBoundsMin; float _p0;   // world-space AABB the volume covers
    float3 gBoundsMax; float _p1;
    uint3  gDim;       uint  gCount; // volume resolution (voxels) + blob count
    float  gDensityScale; float gNoiseDetail; float gNoiseScale; float _p2; // detail 0 = raw
};
[[vk::binding(1, 0)]] RWTexture3D<float4> gVolume : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<Blob> gBlobs : register(t0);

// --- Procedural turbulence -------------------------------------------------------
// Without this each blob is written as a clean sphere, so the field reads as a pile
// of balls. We DOMAIN-WARP the voxel's sample position by fractal noise BEFORE the
// blob falloff, so every blob deforms coherently into billowing cauliflower shapes
// (and the self-shadow march, which samples this same baked field, matches). The
// finer analytic detail is added on top at raymarch time.
float VS_Hash13(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}
float VS_ValueNoise(float3 p) {
    const float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    const float n000 = VS_Hash13(i + float3(0, 0, 0)), n100 = VS_Hash13(i + float3(1, 0, 0));
    const float n010 = VS_Hash13(i + float3(0, 1, 0)), n110 = VS_Hash13(i + float3(1, 1, 0));
    const float n001 = VS_Hash13(i + float3(0, 0, 1)), n101 = VS_Hash13(i + float3(1, 0, 1));
    const float n011 = VS_Hash13(i + float3(0, 1, 1)), n111 = VS_Hash13(i + float3(1, 1, 1));
    const float nx00 = lerp(n000, n100, f.x), nx10 = lerp(n010, n110, f.x);
    const float nx01 = lerp(n001, n101, f.x), nx11 = lerp(n011, n111, f.x);
    return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}
float VS_FBM(float3 p) {
    float s = 0.0f, a = 0.5f;
    [unroll] for (int i = 0; i < 3; ++i) { s += a * VS_ValueNoise(p); p *= 2.03f; a *= 0.5f; }
    return s;
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gDim.x || id.y >= gDim.y || id.z >= gDim.z) return;
    const float3 uvw = (float3(id) + 0.5f) / float3(gDim);
    float3 wp = lerp(gBoundsMin, gBoundsMax, uvw); // this voxel's world centre

    // Fractal domain warp (medium-scale billows) - the primary "de-sphere". Frequency
    // and amplitude are anchored to gNoiseScale (avg blob radius, a WORLD-space constant
    // that's stable frame-to-frame) - NOT the AABB extent, which jumps when the emitter
    // moves fast and made the noise crawl/pop. World-anchored => the smoke flows through
    // a fixed turbulence field, which is both stable and natural-looking.
    if (gNoiseDetail > 0.001f) {
        const float scale = max(gNoiseScale, 0.05f);
        const float3 np = wp * (1.2f / scale);   // ~1.2 warp cycles per blob radius
        const float3 warp =
            float3(VS_FBM(np + 21.3f), VS_FBM(np + 61.7f), VS_FBM(np + 113.9f)) * 2.0f - 1.0f;
        wp += warp * (scale * (0.9f * gNoiseDetail)); // displace up to ~0.9 blob-radii
    }

    float dRaw = 0.0f;      // summed density (pre-scale)
    float tW = 0.0f;        // density-weighted temperature
    [loop]
    for (uint i = 0; i < gCount; ++i) {
        const Blob b = gBlobs[i];
        const float r = max(b.radius, 1e-4f);
        const float x = saturate(1.0f - length(wp - b.pos) / r); // 0 at edge, 1 at centre
        const float g = x * x * (3.0f - 2.0f * x);               // smoothstep falloff
        const float contrib = b.density * g;
        dRaw += contrib;
        tW += b.temperature * contrib;
    }
    const float density = dRaw * gDensityScale;
    const float temp = (dRaw > 1e-4f) ? (tW / dRaw) : 0.0f;
    gVolume[id] = float4(density, temp, 0.0f, 0.0f);
}
