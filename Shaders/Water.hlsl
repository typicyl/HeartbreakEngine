// Shaders/Water.hlsl - Gerstner-wave water surface.
//
// A flat grid mesh (WaterComponent) displaced by summed Gerstner waves in the VS, drawn
// ALPHA-BLENDED over the lit scene in the forward pass (its own waterPSO_, after the
// transparent meshes). The PS shades it as water: Fresnel-weighted sky/IBL reflection over
// a deep-water body colour, a sharp sun specular, fine INTERACTIVE ripples (expanding ring
// sources from rain + object splashes, plus a rain micro-ripple overlay) perturbing the
// normal, and foam on the wave crests. No scene-depth read (that would need new RHI) - the
// see-through is the alpha blend, so shallow water reads by its lower alpha, not a depth cut.
//
// Wave / ripple / colour params are GLOBAL (one scene ocean) in FrameConstants (gWave*/
// gWater*/gRipples), like the weather block; the per-surface transform is gModel (b1),
// reused from the normal mesh draw path.
#include "Common.hlsli"

struct VSInput
{
    float3 positionOS : POSITION;
    float3 normalOS   : NORMAL;
    float4 tangentOS  : TANGENT;
    float2 uv         : TEXCOORD0;
    uint4  joints     : BLENDINDICES;
    float4 weights    : BLENDWEIGHT;
};

struct VSOutput
{
    float4 positionCS : SV_Position;
    float3 positionWS : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
    float  foam       : TEXCOORD2; // coarse whitecap factor (crest fold), NOT ripple-based
    float2 baseXZ     : TEXCOORD3; // UNDISPLACED grid world XZ (for the per-pixel Gerstner normal)
};

// Summed Gerstner waves at world-XZ `xz` and time `t`: horizontal+vertical displacement
// and the analytic surface normal (GPU Gems 1 formulation).
void GerstnerSum(float2 xz, float t, out float3 disp, out float3 normal)
{
    disp = float3(0.0f, 0.0f, 0.0f);
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float wavelength = gWaveA[i].w;
        if (wavelength < 0.01f) continue;
        float2 d = normalize(gWaveA[i].xy + float2(1e-5f, 0.0f));
        float  amp   = gWaveA[i].z;
        float  speed = gWaveB[i].x;
        float  Q     = saturate(gWaveB[i].y); // steepness (0 = rolling, 1 = sharp crests)
        float  k = 6.2831853f / wavelength;
        float  f = k * dot(d, xz) + t * speed * k;
        float  S = sin(f), C = cos(f);
        float  WA = k * amp;
        disp.x += Q * amp * d.x * C;
        disp.z += Q * amp * d.y * C;
        disp.y += amp * S;
        nx -= d.x * WA * C;
        nz -= d.y * WA * C;
        ny -= Q * WA * S;
    }
    normal = normalize(float3(nx, ny, nz));
}

// FFT ocean displacement buffer (Tessendorf; OceanFFT.hlsl output), bound for the water draw
// via SetVertexShaderBuffer on the VS structured-buffer seam. Only read when gWaterParams.w>0.5
// (a UNIFORM branch), so when the Gerstner fallback is active and nothing is bound here, this
// buffer is never accessed. NOTE: shares the t2/space1 seam with GPU-sim particles - a scene
// cannot drive both in the same frame (documented limit).
#define OCEAN_TILE_N 256
[[vk::binding(0, 2)]] StructuredBuffer<float4> gOceanDisp : register(t2, space1);

// Sample the FFT tile (repeats every gFftParams.x metres) at world XZ, offset by (ox,oz)
// texels. Returns (Dx, height, Dz, _). Wrap is power-of-two so neighbours cross tile seams.
float4 OceanSample(float2 xz, int ox, int oz)
{
    const float patch = max(gFftParams.x, 1.0f);
    const float2 uv = frac(xz / patch); // HLSL frac() wraps negatives to [0,1) already
    const int ix = ((int)floor(uv.x * OCEAN_TILE_N) + ox) & (OCEAN_TILE_N - 1);
    const int iz = ((int)floor(uv.y * OCEAN_TILE_N) + oz) & (OCEAN_TILE_N - 1);
    return gOceanDisp[iz * OCEAN_TILE_N + ix];
}

VSOutput VSMain(VSInput input)
{
    float3 worldPos = mul(gModel, float4(input.positionOS, 1.0f)).xyz;
    VSOutput o;
    o.baseXZ = worldPos.xz; // undisplaced grid XZ (smooth across the mesh) for the PS normal

    if (gWaterParams.w > 0.5f)
    {
        // FFT ocean: displace by the spectral field; normal + fold-foam from neighbour taps.
        const float2 xz = worldPos.xz;
        const float4 c  = OceanSample(xz, 0, 0);
        const float4 xp = OceanSample(xz, 1, 0), xm = OceanSample(xz, -1, 0);
        const float4 zp = OceanSample(xz, 0, 1), zm = OceanSample(xz, 0, -1);
        const float hs = gFftParams.y; // runtime height scale
        worldPos.x += c.x;
        worldPos.y += c.y * hs;
        worldPos.z += c.z;

        const float spacing = max(gFftParams.x, 1.0f) / OCEAN_TILE_N; // metres between texels
        const float inv2 = 1.0f / (2.0f * spacing);
        // Tangents of the DISPLACED surface (base advance 2*spacing over the ±1 texel span).
        const float3 dPx = float3(2.0f * spacing + (xp.x - xm.x), (xp.y - xm.y) * hs, xp.z - xm.z);
        const float3 dPz = float3(zp.x - zm.x, (zp.y - zm.y) * hs, 2.0f * spacing + (zp.z - zm.z));
        o.normalWS = normalize(cross(dPz, dPx)); // +Y up for a flat patch

        // Whitecap foam from the horizontal-displacement Jacobian (< 1 where the surface folds).
        const float Jxx = 1.0f + (xp.x - xm.x) * inv2;
        const float Jzz = 1.0f + (zp.z - zm.z) * inv2;
        const float Jxz = (xp.z - xm.z) * inv2;
        const float Jzx = (zp.x - zm.x) * inv2;
        o.foam = saturate(1.0f - (Jxx * Jzz - Jxz * Jzx));
    }
    else
    {
        // Gerstner (analytic) - the low-end / no-compute fallback.
        float3 disp, nrm;
        GerstnerSum(worldPos.xz, gWeather.w, disp, nrm);
        worldPos += disp;
        o.normalWS = nrm;
        // Whitecap foam from the Gerstner fold (nrm.y drops as crests sharpen); COARSE normal
        // so foam does not track the fine ripple perturbation.
        o.foam = saturate((0.42f - nrm.y) * 3.5f);
    }

    o.positionWS = worldPos;
    o.positionCS = mul(gViewProj, float4(worldPos, 1.0f));
    return o;
}

// --- Fine ripple normal (interactive + rain) ----------------------------------
float WHash(float2 p)
{
    p = frac(p * float2(123.34f, 345.45f));
    p += dot(p, p + 34.345f);
    return frac(p.x * p.y);
}
float WNoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = WHash(i), b = WHash(i + float2(1, 0));
    float c = WHash(i + float2(0, 1)), d = WHash(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Fine surface chop as a sum of DIRECTIONAL wavelets. Isotropic (rotated directions), so it
// never shows the axis-aligned grid that value-noise gradients produce - this is what
// replaced the checkerboard-prone noise overlay. Returns a world-space height gradient.
float2 DetailChop(float2 xz, float t)
{
    float2 g = 0.0f;
    [unroll] for (int i = 0; i < 6; ++i)
    {
        float  a = i * 1.0472f + 0.35f;          // 6 directions ~60 deg apart
        float2 d = float2(cos(a), sin(a));
        float  freq = 2.2f + i * 1.35f;          // rising octaves
        float  amp = 0.06f / (1.0f + i * 0.7f);  // falling amplitude
        g += d * cos(dot(d, xz) * freq + t * (1.1f + i * 0.6f)) * amp;
    }
    return g;
}

// One lattice of concentric RAIN-impact rings, sampled in a ROTATED frame so its cell grid is
// not axis-aligned. World-XZ is tiled into cells; each cell periodically "impacts" at a jittered
// point + random phase, spawning an expanding, decaying ring. R rotates world->lattice, Ri
// rotates the impact point back to world (so distances stay world-correct). `seed` decorrelates
// stacked lattices.
float2 RainLattice(float2 xz, float t, float intensity, float2x2 R, float2x2 Ri, float cell,
                   float seed)
{
    float2 g = 0.0f;
    const float2 rxz = mul(R, xz);
    const float2 baseC = floor(rxz / cell);
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        float2 c = baseC + float2(dx, dy);
        float  r0 = WHash(c + seed);
        if (r0 > intensity * 0.8f + 0.05f) continue; // fewer active cells at low intensity
        float2 impactR = (c + float2(WHash(c + seed + 11.1f), WHash(c + seed + 23.3f))) * cell;
        float2 impact = mul(Ri, impactR);        // rotated cell point -> world
        float  period = 0.7f + r0 * 0.9f;
        float  age = frac(t / period + WHash(c + seed + 37.7f)) * period;
        float  dist = length(xz - impact);
        float  ring = dist - age * 2.0f;         // ring expands at 2 m/s
        float  prof = exp(-ring * ring * 30.0f); // thin ring
        float  amp = exp(-age * 3.4f) * saturate(age * 30.0f);
        float2 dir = (dist > 1e-4f) ? (xz - impact) / dist : float2(0, 0);
        g += dir * (-ring) * prof * amp;
    }
    return g;
}

// Procedural rain rings. A SINGLE axis-aligned lattice reads as a square ("cubic") grid on the
// water; summing TWO incommensurate ROTATED lattices (different angle + cell size) removes the
// visible periodicity so it reads as scattered rain. (The GPU ripple sim supersedes this; this
// keeps the no-sim fallback from looking gridded.)
float2 RainRipples(float2 xz, float t, float intensity)
{
    if (intensity <= 0.001f) return float2(0.0f, 0.0f);
    const float2x2 Ra  = float2x2(0.80902f, -0.58779f, 0.58779f, 0.80902f);   //  +36 deg
    const float2x2 Rai = float2x2(0.80902f,  0.58779f, -0.58779f, 0.80902f);  //  transpose
    const float2x2 Rb  = float2x2(0.93969f,  0.34202f, -0.34202f, 0.93969f);  //  -20 deg
    const float2x2 Rbi = float2x2(0.93969f, -0.34202f,  0.34202f, 0.93969f);  //  transpose
    float2 g  = RainLattice(xz, t, intensity, Ra, Rai, 1.15f, 0.0f)  * 0.60f;
    g        += RainLattice(xz, t, intensity, Rb, Rbi, 1.90f, 53.7f) * 0.55f;
    return g * 6.0f;
}

// World-space normal-perturbation gradient: always-on isotropic chop + rain rings + the
// discrete interactive splash rings (gRipples: object/player impacts). rippleScale sets the
// chop frequency, rippleStrength the overall amplitude.
float2 RippleGrad(float2 xz, float t)
{
    float2 g = DetailChop(xz * max(gWaterParams.z, 0.05f), t);
    g += RainRipples(xz, t, gWeather2.w);
    [loop] for (uint i = 0; i < gRippleCount; ++i)
    {
        float age = gRipples[i].z; // seconds since impact (CPU-aged)
        if (age <= 0.0f || age > 2.5f) continue;
        float2 c = gRipples[i].xy;
        float  dist = length(xz - c);
        float  ring = dist - age * 3.0f;                 // expands at 3 m/s
        float  prof = exp(-ring * ring * 8.0f);
        float  amp = exp(-age * 1.6f) * saturate(age * 20.0f) * gRipples[i].w;
        float2 dir = (dist > 1e-3f) ? (xz - c) / dist : float2(0, 0);
        g += dir * (-ring) * prof * amp * 3.0f;
    }
    return g * max(gWaterParams.y, 0.0f); // rippleStrength = overall ripple normal amplitude
}

float4 PSMain(VSOutput input) : SV_Target
{
    float t = gWeather.w;
    // Base surface normal. The VS-interpolated vertex normal is only C0 across the coarse water
    // grid, so the reflection showed the triangle-edge slope kinks as hard "detached" facets. For
    // Gerstner, recompute the normal PER-PIXEL from the smooth undisplaced grid XZ (C-infinity ->
    // no facets); the FFT path keeps its neighbour-tap normal from the VS.
    float3 N;
    if (gWaterParams.w > 0.5f)
    {
        N = normalize(input.normalWS);
    }
    else
    {
        float3 gdisp, gnrm;
        GerstnerSum(input.baseXZ, t, gdisp, gnrm);
        N = gnrm;
    }
    // Perturb the (now smooth) surface normal by the fine ripple gradient.
    float2 rg = RippleGrad(input.positionWS.xz, t);
    N = normalize(N + float3(rg.x, 0.0f, rg.y));

    float3 V = normalize(gCameraPosWS - input.positionWS);
    float NdotV = saturate(dot(N, V));

    // Fresnel (Schlick, water F0 ~ 0.02). gWaterShallow.w = fresnel power/tightness.
    float fres = 0.02f + 0.98f * pow(1.0f - NdotV, max(gWaterShallow.w, 1.0f));

    // Sky / IBL reflection off the reflected view ray.
    float3 R = reflect(-V, N);
    float3 reflection;
    if (gPrefilteredIndex != 0u)
        reflection = SampleBindlessLod(gPrefilteredIndex, EquirectUV(R),
                                       gWaterDeep.w * gPrefilteredMaxLod).rgb;
    else // fall back to a simple sky tint (up = light, horizon = pale)
        reflection = lerp(float3(0.55f, 0.62f, 0.72f), float3(0.30f, 0.45f, 0.72f), saturate(R.y));

    // Water body colour: deep looking straight down, a paler shallow tint at grazing.
    float3 body = lerp(gWaterDeep.rgb, gWaterShallow.rgb, saturate(1.0f - NdotV));

    // Sharp sun specular off the wave normal.
    float3 L = normalize(gLightDirWS);
    float3 H = normalize(L + V);
    float  spec = pow(saturate(dot(N, H)), 800.0f) * saturate(gLightDirWS.y * 4.0f + 0.1f);
    float3 sun = gLightColor * gLightIntensity * spec;

    // Foam: the coarse whitecap factor from the VS (crest fold / wave collision), broken up
    // by animated noise so it reads as churning foam rather than a smooth band, plus a touch
    // where rain agitates the surface. Uses input.foam (COARSE), not N.y, so it never paints
    // the fine ripple pattern. gWaterParams.x = foam amount. (WNoise as a SCALAR mask is fine -
    // only its GRADIENT was grid-aligned.)
    float foamNoise = WNoise(input.positionWS.xz * 3.0f + t * 0.7f) * 0.55f +
                      WNoise(input.positionWS.xz * 7.0f - t * 1.1f) * 0.30f + 0.15f;
    float rainAgitation = gWeather2.w * saturate(length(rg) * 0.4f);
    float foam = saturate((input.foam * foamNoise + rainAgitation) * gWaterParams.x);
    float3 foamCol = float3(0.92f, 0.95f, 0.98f);

    // Depth-based grading: when the scene-depth SRV is bound for the water pass, read the floor
    // depth behind this pixel to get the water-column length, and grade the body colour
    // (Beer-Lambert absorption), shoreline foam and a soft intersection fade by it. The alpha
    // blend already gives see-through; this modulates the water's own colour + coverage by depth.
    // Plain (no-depth) coverage - also the fallback the depth grade blends back to at hard edges.
    const float plainAlpha = saturate(lerp(0.72f, 1.0f, fres) + foam);
    float alpha = plainAlpha;
    if (gSceneDepthIndex != 0u)
    {
        const float2 uv = input.positionCS.xy * gScreenTexel; // SV_Position -> screen UV (both backends)
        const float floorZ = SampleBindless(gSceneDepthIndex, uv).r;
        const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f); // engine convention
        const float4 fw = mul(gInvViewProj, float4(ndc, floorZ, 1.0f));
        const float3 floorWS = fw.xyz / fw.w;
        // Water column the view ray traverses = (camera->floor) - (camera->surface).
        const float waterDepth =
            max(distance(gCameraPosWS, floorWS) - distance(gCameraPosWS, input.positionWS), 0.0f);

        // A depth DISCONTINUITY - a submerged object's silhouette, a terrain step, or the floor
        // dropping away behind an object - makes waterDepth JUMP, which the sharp grade terms below
        // otherwise turn into a hard-edged "detached" patch tracing that geometry. fwidth() spikes
        // exactly at those jumps; fade the grade back to the plain look there so it can never paint
        // a hard edge over geometry. Smooth floor (waves only) -> fwidth ~0 -> full grade.
        const float stable = saturate(1.0f - fwidth(waterDepth) * 0.75f);

        const float absorb = saturate(waterDepth / max(gAbsorptionDepth, 0.01f));
        const float3 gradedBody = lerp(gWaterShallow.rgb, gWaterDeep.rgb, absorb); // deep -> deep colour
        const float shore = 1.0f - saturate(waterDepth / max(gShorelineWidth, 0.01f));
        const float gradedFoam = saturate(foam + shore * shore * gWaterParams.x); // shoreline whitewater
        // Deep water opaque, shallow see-through, grazing reflective; soft fade where the column vanishes.
        const float opacity = max(lerp(0.72f, 1.0f, fres), lerp(0.28f, 0.96f, absorb));
        const float gradedAlpha = saturate(opacity + gradedFoam) * saturate(waterDepth / max(gEdgeFade, 0.01f));

        body  = lerp(body, gradedBody, stable);   // body was the plain grazing tint
        foam  = lerp(foam, gradedFoam, stable);
        alpha = lerp(plainAlpha, gradedAlpha, stable);
    }

    float3 col = lerp(body, reflection, fres) + sun;
    col = lerp(col, foamCol, foam);

    if (gOutputLinear != 0u)
        return float4(col, alpha); // HDR pipeline: linear radiance, alpha = blend coverage

    col = TonemapACES(col * gExposure);
    col = LinearToSRGB(col);
    return float4(col, alpha);
}
