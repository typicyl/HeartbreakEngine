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
//   p3 = (timeSeconds, noiseDetail, noiseScale, 0)

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
// 3-octave fractal Brownian motion in [0,1] (value noise - used for the domain WARP,
// where a lattice bias in a displacement is far less visible than in the density).
float VP_FBM(float3 p) {
    float sum = 0.0f, amp = 0.5f;
    [unroll] for (int i = 0; i < 3; ++i) { sum += amp * VP_ValueNoise(p); p *= 2.02f; amp *= 0.5f; }
    return sum;
}

// --- Gradient + cellular noise (for the visible DENSITY detail) --------------------
// Stable 3D->3D hash (Dave Hoskins) - no sin(), deterministic across drivers.
float3 VP_Hash33(float3 p) {
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yzz) * p.zyx) * 2.0f - 1.0f;
}
// Gradient (Perlin) noise in [0,1] - no axis-aligned lattice signature (unlike value
// noise). Used where the noise drives DENSITY directly, so the grid must not show.
float VP_GradNoise(float3 p) {
    const float3 i = floor(p);
    const float3 f = frac(p);
    const float3 u = f * f * (3.0f - 2.0f * f);
    #define VPG(o) dot(VP_Hash33(i + o), f - o)
    const float n =
        lerp(lerp(lerp(VPG(float3(0, 0, 0)), VPG(float3(1, 0, 0)), u.x),
                  lerp(VPG(float3(0, 1, 0)), VPG(float3(1, 1, 0)), u.x), u.y),
             lerp(lerp(VPG(float3(0, 0, 1)), VPG(float3(1, 0, 1)), u.x),
                  lerp(VPG(float3(0, 1, 1)), VPG(float3(1, 1, 1)), u.x), u.y), u.z);
    #undef VPG
    return n * 0.5f + 0.5f;
}
// Worley / cellular F1 in ~[0,1]: distance to the nearest jittered feature point.
float VP_Worley(float3 p) {
    const float3 i = floor(p);
    const float3 f = frac(p);
    float d = 1.0f;
    [unroll] for (int x = -1; x <= 1; ++x)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int z = -1; z <= 1; ++z) {
        const float3 g = float3(x, y, z);
        const float3 fp = g + (VP_Hash33(i + g) * 0.5f + 0.5f) - f;
        d = min(d, dot(fp, fp));
    }
    return sqrt(d);
}
// Inverted-Worley fBM (2 octaves) in [0,1]: rounded "cauliflower" lobes - THE canonical
// cloud/smoke detail noise (Guerrilla "Nubis"). High inside billows, low in the gaps.
float VP_WorleyFBM(float3 p) {
    float w = (1.0f - VP_Worley(p)) * 0.667f;
    w += (1.0f - VP_Worley(p * 2.03f + 17.3f)) * 0.333f;
    return saturate(w);
}

// Interleaved Gradient Noise (matches VolumetricFog): per-pixel march offset that
// TAA integrates into smooth volume; animated by the frame index (0 when TAA off).
float VP_IGN(float2 p) {
    return frac(52.9829189f * frac(0.06711056f * p.x + 0.00583715f * p.y));
}

// Temperature 0..1 -> emissive colour along a stylised Planckian ramp: near-black ember
// -> deep red -> orange -> yellow -> (only at the very top) white-hot. Most of the range
// stays in the fire palette so cores read as flame; the near-white tip gives the hottest
// base its intensity without washing the whole plume to a highlight.
float3 Blackbody(float t) {
    t = saturate(t);
    float3 c = lerp(float3(0.05f, 0.004f, 0.0f), float3(0.85f, 0.16f, 0.02f), saturate(t * 3.0f));   // black -> deep red
    c = lerp(c, float3(1.0f, 0.48f, 0.06f), saturate(t * 2.2f - 0.7f));                              //       -> orange
    c = lerp(c, float3(1.0f, 0.83f, 0.38f), saturate(t * 2.0f - 1.0f));                              //       -> yellow
    c = lerp(c, float3(1.0f, 0.96f, 0.85f), saturate(t * 6.0f - 5.2f));                              //       -> white-hot tip
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

    // Swirling turbulence: rotate the noise DOMAIN about Y so the whole detail field CHURNS
    // like a rising vortex (a cheap stand-in for curl-noise advection) instead of scrolling
    // rigidly, plus a gentle buoyant rise. The rotation pivots on the VOLUME CENTRE, NOT the
    // world origin: an origin pivot makes the swept sample path have radius |wp.xz|, so a plume
    // far from origin (the norm in a streamed world) would orbit through thousands of noise
    // cells/sec and "swim". Centring bounds the swirl radius to the volume half-extent at any
    // world position, so a plume animates identically wherever it is placed.
    const float3 vc = 0.5f * (bmin + bmax);
    const float3 q = wp - vc;
    const float ang = time * 0.15f + q.y * (invScale * 0.6f);
    const float ca = cos(ang), sa = sin(ang);
    const float3 rp = float3(q.x * ca - q.z * sa, q.y, q.x * sa + q.z * ca) + vc;
    const float3 rise = float3(0.05f, 0.24f, 0.03f) * time;

    // FINE warp: the splat compute already baked the big billows, so here we add
    // higher-frequency, lower-amplitude sub-voxel displacement on top (analytic, so it
    // stays crisp regardless of the volume resolution). Warp reads the noise at the
    // swirled domain but displaces the ACTUAL sample position.
    const float3 np = rp * (invScale * 2.5f) + rise;
    const float3 warp = float3(VP_FBM(np + 13.7f), VP_FBM(np + 51.3f), VP_FBM(np + 97.1f)) * 2.0f - 1.0f;
    const float3 swp = wp + warp * (scale * (0.32f * detail));
    float d = SampleVolume(swp, bmin, bmax, temp);
    if (d <= 0.0f) return 0.0f;

    // Perlin-Worley erosion: inverted-Worley "cauliflower" lobes modulated by a large
    // gradient-noise swell, carving the low-density FRINGE into billows while leaving the
    // dense core intact. Both noises here drive density directly, so both are lattice-free.
    const float3 ep = rp * (invScale * 4.5f) + rise * 1.7f;
    const float lobes = VP_WorleyFBM(ep);         // 0..1 cauliflower detail
    const float swell = VP_GradNoise(ep * 0.35f); // 0..1 large-scale variation (no grid)
    const float detailN = saturate(lobes * 0.7f + swell * 0.3f);
    return saturate(d - (1.0f - detailN) * (detail * 0.6f)); // erode the low-density fringe
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

        // Multiple-scattering approximation (Wrenninge / Häggström "Oz"): sum a few
        // octaves where each successive bounce sees LESS extinction (light spreads
        // deeper), scatters LESS, and grows MORE isotropic (the phase flattens). This is
        // the cheap stand-in for true multi-scatter that gives thick smoke its soft,
        // bright, translucent interior instead of a dead-black core - a far better cue
        // than the old flat ambient fill. Reuses the one self-shadow value (no extra
        // volume samples): reduced extinction == shadow^a.
        const float cosT = dot(rd, L);
        float msSun = 0.0f;
        [unroll] for (int o = 0; o < 3; ++o) {
            const float a = exp2(-(float)o);       // 1, 0.5, 0.25  extinction attenuation
            const float b = pow(0.6f, (float)o);   // phase eccentricity -> isotropic
            const float c = pow(0.55f, (float)o);  // scatter weight falloff
            msSun += c * pow(max(shadow, 1e-3f), a) * VP_PhaseHG(cosT, 0.6f * b);
        }
        msSun += 0.06f; // isotropic floor so deep-shadowed sides aren't dead flat
        const float3 sun = gLightColor * (gLightIntensity * (msSun * 0.28f));
        // Emission concentrated at hot cores (temp^3) so cool smoke stays dark and only
        // the fire base glows; the Planckian ramp only reaches white at the very hottest.
        const float3 emission = Blackbody(temp) * (emissionMul * temp * temp * temp);
        const float3 scat = (sun + ambient * 0.4f) * d + emission * d;
        const float stepTrans = exp(-d * stepLen * extinction);
        inscatter += transmittance * scat * stepLen;
        transmittance *= stepTrans;
        if (transmittance < 0.003f) break; // fully occluded
    }
    return float4(inscatter, transmittance);
}
