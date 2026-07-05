// Shaders/VolumetricParticles.hlsl - raymarched volumetric smoke/fire.
//
// Marches camera -> scene depth through the 3D density/temperature volume the
// compute splat produced (VolumeSplat.hlsl), accumulating absorption
// (Beer-Lambert), sun scatter with a short self-shadow march, ambient, and
// blackbody EMISSION from temperature (so hot cores glow like fire). Outputs
// (rgb = inscatter+emission, a = transmittance); ApplyHalfRes composites it over
// the HDR scene as `scene*a + rgb` (same as VolumetricFog), so it renders at
// reduced resolution and blooms downstream.
//
// The volume is a dedicated Texture3D (the bindless table is Texture2D[], so it
// can't hold a 3D texture); the backend binds it + the linear-clamp sampler.
#include "PostCommon.hlsli"

// Dedicated 3D volume binding (D3D12: t0 space6 via a new root-sig table entry;
// Vulkan: set 0 binding 5 - bindings 0..4 are frame UBO / post UBO / sampler /
// bones / instances). R = density, G = temperature (0..1).
[[vk::binding(5, 0)]] Texture3D<float4> gVolumeTex : register(t0, space6);

// PostParams packing (set by the backend's volumetric raymarch pass):
//   p0 = (boundsMin.xyz, stepCount)
//   p1 = (boundsMax.xyz, densityMul)
//   p2 = (emissionMul, shadowSteps, extinction, ditherFrame)
//   p3 = (timeSeconds, noiseDetail, 0, 0)

// --- Procedural detail noise ------------------------------------------------------
// The compute splat writes SMOOTH spheres (one per particle), which read as "blobs".
// To make continuous, billowing fire/smoke we DOMAIN-WARP the sample position with
// animated fractal noise (big rolling structure) and ERODE the density with a finer
// octave (wispy fringes) - the standard "detail noise on a coarse base" that film /
// AAA (Houdini, Embergen) use. Cost scales with the raymarch step count, so the
// low-end step/resolution knobs bound it; noiseDetail=0 skips it entirely.
float VP_Hash13(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}
float VP_ValueNoise(float3 p) {
    const float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    const float n000 = VP_Hash13(i + float3(0, 0, 0));
    const float n100 = VP_Hash13(i + float3(1, 0, 0));
    const float n010 = VP_Hash13(i + float3(0, 1, 0));
    const float n110 = VP_Hash13(i + float3(1, 1, 0));
    const float n001 = VP_Hash13(i + float3(0, 0, 1));
    const float n101 = VP_Hash13(i + float3(1, 0, 1));
    const float n011 = VP_Hash13(i + float3(0, 1, 1));
    const float n111 = VP_Hash13(i + float3(1, 1, 1));
    const float nx00 = lerp(n000, n100, f.x), nx10 = lerp(n010, n110, f.x);
    const float nx01 = lerp(n001, n101, f.x), nx11 = lerp(n011, n111, f.x);
    return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}
// 3-octave fractal Brownian motion in [0,1].
float VP_FBM(float3 p) {
    float sum = 0.0f, amp = 0.5f;
    [unroll] for (int i = 0; i < 3; ++i) { sum += amp * VP_ValueNoise(p); p *= 2.02f; amp *= 0.5f; }
    return sum;
}

// Interleaved Gradient Noise (matches VolumetricFog): per-pixel march offset that
// TAA integrates into smooth volume; animated by the frame index (0 when TAA off).
float VP_IGN(float2 p) {
    return frac(52.9829189f * frac(0.06711056f * p.x + 0.00583715f * p.y));
}

// Temperature 0..1 -> fire emissive colour: deep red -> orange -> yellow. Tops out
// at warm yellow (not white) so hot cores read as fire, not a blown-out highlight.
float3 Blackbody(float t) {
    t = saturate(t);
    float3 c = lerp(float3(0.7f, 0.06f, 0.01f), float3(1.0f, 0.35f, 0.04f), saturate(t * 2.0f));
    c = lerp(c, float3(1.0f, 0.75f, 0.30f), saturate(t * 2.0f - 1.0f));
    return c;
}

// Henyey-Greenstein phase (g>0 = forward scatter). Gives the bright "silver lining"
// where the smoke faces the sun - the single biggest realism cue for lit gas. Peak
// bounded (as in VolumetricFog) so a strong forward look stays stable.
float VP_PhaseHG(float cosT, float g) {
    g = clamp(g, -0.9f, 0.9f);
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cosT;
    const float p = (1.0f - g2) / (12.566371f * max(pow(max(denom, 1e-4f), 1.5f), 1e-4f));
    return min(p, 6.0f);
}

// Ray vs AABB slab test; returns (tNear, tFar). tFar < tNear => the ray misses.
float2 RayBox(float3 ro, float3 rd, float3 bmin, float3 bmax) {
    const float3 inv = 1.0f / rd;
    const float3 a = (bmin - ro) * inv;
    const float3 b = (bmax - ro) * inv;
    const float3 tmin = min(a, b), tmax = max(a, b);
    return float2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

float SampleVolume(float3 wp, float3 bmin, float3 bmax, out float temp) {
    const float3 uvw = (wp - bmin) / max(bmax - bmin, 1e-4f);
    if (any(uvw < 0.0f) || any(uvw > 1.0f)) { temp = 0.0f; return 0.0f; }
    const float4 s = gVolumeTex.SampleLevel(gClampSampler, uvw, 0.0f);
    temp = s.g;
    return s.r;
}

// Detailed density: domain-warp the sample position so the smooth blob isosurfaces
// billow, then erode the fringe with a finer octave so edges break into wisps. Scale
// is derived from the volume extent (resolution-independent); animated by `time`.
float SampleVolumeDetailed(float3 wp, float3 bmin, float3 bmax, float time, float detail,
                           float scale, out float temp) {
    if (detail <= 0.001f) return SampleVolume(wp, bmin, bmax, temp);
    const float invScale = 1.0f / max(scale, 0.05f); // world-anchored (stable when moving)
    const float3 wind = float3(0.06f, 0.22f, 0.04f) * time; // gentle rise + drift churn
    // FINE warp only: the splat compute already baked the big billows, so here we
    // add higher-frequency, lower-amplitude sub-voxel detail on top (analytic, so it
    // stays crisp regardless of the volume resolution).
    const float3 np = wp * (invScale * 2.5f) + wind;
    const float3 warp = float3(VP_FBM(np + 13.7f), VP_FBM(np + 51.3f), VP_FBM(np + 97.1f)) * 2.0f - 1.0f;
    const float3 swp = wp + warp * (scale * (0.3f * detail));
    float d = SampleVolume(swp, bmin, bmax, temp);
    if (d <= 0.0f) return 0.0f;
    const float fine = VP_FBM(wp * (invScale * 4.5f) + wind * 1.7f); // finest octave for wisps
    return saturate(d - (1.0f - fine) * (detail * 0.5f));           // erode the low-density fringe
}

float4 PSMain(FSOutput input) : SV_Target {
    const float2 uv = input.positionCS.xy * gOutTexel;
    const float3 bmin = gPostParams0.xyz;
    const float3 bmax = gPostParams1.xyz;
    const int   steps = max(1, (int)gPostParams0.w);
    const float densityMul = gPostParams1.w;
    const float emissionMul = gPostParams2.x;
    const int   shadowSteps = max(0, (int)gPostParams2.y);
    const float extinction = max(gPostParams2.z, 1e-3f);
    const float noiseTime = gPostParams3.x;
    const float noiseDetail = saturate(gPostParams3.y);
    const float noiseScale = max(gPostParams3.z, 0.05f);

    // Ray from the camera toward the scene surface at this pixel.
    const float depth = SamplePost(gInput2, uv).r;
    const float3 ro = gCameraPosWS;
    const float3 worldEnd = WorldFromDepthPost(uv, min(depth, 0.999999f));
    const float3 rd = normalize(worldEnd - ro);
    const float sceneDist = (depth >= 1.0f) ? 1e7f : length(worldEnd - ro);

    // Clip the march to the volume's AABB (skip all empty space) and the scene.
    float2 box = RayBox(ro, rd, bmin, bmax);
    box.x = max(box.x, 0.0f);
    box.y = min(box.y, sceneDist);
    if (box.y <= box.x) return float4(0.0f, 0.0f, 0.0f, 1.0f); // no volume on this ray

    const float marchLen = box.y - box.x;
    const float stepLen = marchLen / steps;
    const float dither = VP_IGN(input.positionCS.xy + 5.588238f * gPostParams2.w);
    const float3 L = normalize(gLightDirWS); // toward the sun
    const float3 ambient = gLightColor * gAmbientIntensity;

    float transmittance = 1.0f;
    float3 inscatter = 0.0f.xxx;
    [loop]
    for (int i = 0; i < steps; ++i) {
        const float t = box.x + (i + dither) * stepLen;
        const float3 pos = ro + rd * t;
        float temp;
        float d = SampleVolumeDetailed(pos, bmin, bmax, noiseTime, noiseDetail, noiseScale, temp) * densityMul;
        // Soft intersection: fade density as the sample nears the scene surface so the
        // volume hugs geometry (wraps AROUND objects) instead of hard-clipping a sharp
        // edge where it meets a floor/wall. sceneDist is 1e7 for sky, so open air is
        // unaffected. ~0.35 world-unit contact softening.
        d *= saturate((sceneDist - t) * 2.857f);
        if (d < 1e-4f) continue;

        // Self-shadow: a short optical-depth march toward the light through the volume.
        float shadow = 1.0f;
        if (shadowSteps > 0) {
            const float2 lb = RayBox(pos, L, bmin, bmax);
            const float lFar = max(min(lb.y, marchLen), 0.0f);
            const float ls = lFar / shadowSteps;
            float od = 0.0f;
            [loop]
            for (int j = 0; j < shadowSteps; ++j) {
                float tt;
                od += SampleVolume(pos + L * ((j + 0.5f) * ls), bmin, bmax, tt) * densityMul * ls;
            }
            shadow = exp(-od * extinction);
        }

        // Scattered light. Sun in-scatter is weighted by the Henyey-Greenstein phase
        // (bright forward-facing silver lining) and the self-shadow, plus a small
        // unshadowed base so shadowed sides aren't dead flat. A cheap multi-scatter
        // fill (transmittance-independent ambient) keeps thick interiors reading as
        // lit gas instead of pure black. Emission is concentrated at hot cores
        // (temp^2) so cool smoke stays dark and only the fire base glows.
        const float phase = VP_PhaseHG(dot(rd, L), 0.55f);
        const float3 sun = gLightColor * (gLightIntensity * (shadow * phase * 0.28f + 0.05f));
        const float3 multiScatter = gLightColor * (gLightIntensity * 0.05f); // soft fill
        const float3 emission = Blackbody(temp) * (emissionMul * temp * temp);
        const float3 scat = (sun + multiScatter + ambient * 0.4f) * d + emission * d;
        const float stepTrans = exp(-d * stepLen * extinction);
        inscatter += transmittance * scat * stepLen;
        transmittance *= stepTrans;
        if (transmittance < 0.003f) break; // fully occluded
    }
    return float4(inscatter, transmittance);
}
