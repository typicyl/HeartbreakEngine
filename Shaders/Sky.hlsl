// Shaders/Sky.hlsl - real-time analytic sky (atmosphere + day/night + clouds).
//
// A fullscreen triangle at the far plane shades only the pixels with no scene
// coverage. The view ray is reconstructed from gInvViewProj, then the sky is
// computed ANALYTICALLY each frame from the sun direction (gLightDirWS) by a
// Rayleigh+Mie single-scattering ray-march. As the sun drops below the horizon
// the daytime scattering fades into a night sky with sharp procedural stars and a
// moon. A procedural cloud layer (FBM on a sky plane, sun-lit) + an overcast
// gray-out are driven by gWeather. No baked equirect is sampled, so the sun can
// move freely (day/night cycle) and weather changes with no re-bake.
#include "Common.hlsli"

static const float SKY_PI = 3.14159265358979f;

struct VSOutput
{
    float4 positionCS : SV_Position;
    float2 ndc        : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    float2 ndc = uv * 2.0f - 1.0f;
    VSOutput o;
    o.positionCS = float4(ndc, 1.0f, 1.0f); // far plane ([0,1] depth)
    o.ndc = ndc;
    return o;
}

// --- Rayleigh + Mie single-scattering atmosphere (HLSL port of IBL.cpp) -------
float2 RaySphere(float3 r0, float3 rd, float sr)
{
    float a = dot(rd, rd);
    float b = 2.0f * dot(rd, r0);
    float c = dot(r0, r0) - sr * sr;
    float d = b * b - 4.0f * a * c;
    if (d < 0.0f) return float2(1e9f, -1e9f);
    d = sqrt(d);
    return float2((-b - d) / (2.0f * a), (-b + d) / (2.0f * a));
}

float3 Atmosphere(float3 r, float3 r0, float3 pSun, float iSun,
                  float rPlanet, float rAtmos, float3 kRlh, float kMie,
                  float shRlh, float shMie, float g)
{
    pSun = normalize(pSun);
    r = normalize(r);
    const int iSteps = 16, jSteps = 8;

    float2 p = RaySphere(r0, r, rAtmos);
    if (p.x > p.y) return float3(0.0f, 0.0f, 0.0f);
    float2 pg = RaySphere(r0, r, rPlanet);
    if (pg.x <= pg.y && pg.y > 0.0f) p.y = min(p.y, max(pg.x, 0.0f));
    p.x = max(p.x, 0.0f);
    float iStepSize = (p.y - p.x) / (float)iSteps;

    float iTime = p.x;
    float3 totalRlh = 0.0f, totalMie = 0.0f;
    float iOdRlh = 0.0f, iOdMie = 0.0f;

    float mu = dot(r, pSun);
    float mumu = mu * mu;
    float gg = g * g;
    float pRlh = 3.0f / (16.0f * SKY_PI) * (1.0f + mumu);
    float pMie = 3.0f / (8.0f * SKY_PI) * ((1.0f - gg) * (mumu + 1.0f)) /
                 (pow(abs(1.0f + gg - 2.0f * mu * g), 1.5f) * (2.0f + gg));

    [loop] for (int i = 0; i < iSteps; ++i)
    {
        float3 iPos = r0 + r * (iTime + iStepSize * 0.5f);
        float iHeight = length(iPos) - rPlanet;
        float odStepRlh = exp(-iHeight / shRlh) * iStepSize;
        float odStepMie = exp(-iHeight / shMie) * iStepSize;
        iOdRlh += odStepRlh;
        iOdMie += odStepMie;

        float jStepSize = RaySphere(iPos, pSun, rAtmos).y / (float)jSteps;
        float jTime = 0.0f, jOdRlh = 0.0f, jOdMie = 0.0f;
        [loop] for (int j = 0; j < jSteps; ++j)
        {
            float3 jPos = iPos + pSun * (jTime + jStepSize * 0.5f);
            float jHeight = length(jPos) - rPlanet;
            jOdRlh += exp(-jHeight / shRlh) * jStepSize;
            jOdMie += exp(-jHeight / shMie) * jStepSize;
            jTime += jStepSize;
        }
        float3 attn = exp(-(kMie * (iOdMie + jOdMie) + kRlh * (iOdRlh + jOdRlh)));
        totalRlh += odStepRlh * attn;
        totalMie += odStepMie * attn;
        iTime += iStepSize;
    }
    return iSun * (pRlh * kRlh * totalRlh + pMie * kMie * totalMie);
}

// --- Hash / noise -------------------------------------------------------------
float Hash13(float3 p3)
{
    p3 = frac(p3 * 0.1031f);
    p3 += dot(p3, p3.zyx + 31.32f);
    return frac((p3.x + p3.y) * p3.z);
}
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.x, p.y, p.x) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}
float Noise2(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = Hash12(i), b = Hash12(i + float2(1, 0));
    float c = Hash12(i + float2(0, 1)), d = Hash12(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
float FBM(float2 p)
{
    float v = 0.0f, a = 0.5f;
    [unroll] for (int i = 0; i < 5; ++i) { v += a * Noise2(p); p *= 2.02f; a *= 0.5f; }
    return v;
}

// 3D value noise + fBm (for the volumetric cloud density field).
float Noise3(float3 p)
{
    float3 i = floor(p), f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float n000 = Hash13(i), n100 = Hash13(i + float3(1, 0, 0));
    float n010 = Hash13(i + float3(0, 1, 0)), n110 = Hash13(i + float3(1, 1, 0));
    float n001 = Hash13(i + float3(0, 0, 1)), n101 = Hash13(i + float3(1, 0, 1));
    float n011 = Hash13(i + float3(0, 1, 1)), n111 = Hash13(i + float3(1, 1, 1));
    float nx00 = lerp(n000, n100, f.x), nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x), nx11 = lerp(n011, n111, f.x);
    return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}
float FBM3(float3 p)
{
    float v = 0.0f, a = 0.5f;
    [unroll] for (int i = 0; i < 4; ++i) { v += a * Noise3(p); p *= 2.03f; a *= 0.5f; }
    return v; // ~[0, 0.9375]
}

// Henyey-Greenstein phase (bounded peak, like the fog shader).
float HGphase(float c, float g)
{
    float gg = g * g;
    return (1.0f - gg) / (4.0f * SKY_PI * pow(abs(1.0f + gg - 2.0f * g * c), 1.5f));
}

// --- Stars: tiny, sharp, sparse points (most faint, a few bright) -------------
float StarLayer(float3 dir, float freq, float density, float sharp, float seed)
{
    float3 p = dir * freq;
    float3 cell = floor(p);
    if (Hash13(cell + seed) > density) return 0.0f;       // most cells empty
    float3 off = float3(Hash13(cell + seed + 1.7f),
                        Hash13(cell + seed + 3.3f),
                        Hash13(cell + seed + 5.9f)) - 0.5f;
    float3 f = frac(p) - 0.5f - off * 0.7f;
    float d2 = dot(f, f);
    float bright = pow(Hash13(cell + seed + 9.1f), 5.0f); // mostly faint
    return bright * exp(-d2 * sharp);                     // tight point
}

float3 StarField(float3 dir, float time)
{
    if (dir.y < 0.02f) return 0.0f;
    float fade = saturate((dir.y - 0.02f) * 4.0f);
    // Three layers: a dense faint dust, a medium field, and sparse bright stars.
    float s = StarLayer(dir, 340.0f, 0.45f, 1500.0f, 0.0f) * 0.7f
            + StarLayer(dir, 220.0f, 0.28f, 1000.0f, 19.0f) * 1.2f
            + StarLayer(dir, 120.0f, 0.10f, 600.0f, 53.0f) * 2.2f;
    float tw = 0.7f + 0.3f * sin(time * 3.0f + Hash13(floor(dir * 300.0f)) * 30.0f);
    return float3(0.88f, 0.92f, 1.0f) * (s * tw * fade * 8.0f);
}

// --- Night sky: very dark gradient + stars + a soft moon ----------------------
float3 NightSky(float3 dir, float3 sunDir, float time)
{
    float up = saturate(dir.y);
    // Deep-blue night: a real night sky is NOT black - it keeps a clear blue glow,
    // brighter (a touch of teal) near the horizon, deep blue overhead. Kept bright
    // enough to survive the night exposure (~0.6x) + tonemap + painterly and still
    // read as blue instead of crushing to black.
    float3 night = lerp(float3(0.060f, 0.105f, 0.190f), float3(0.032f, 0.055f, 0.120f), up);
    night += StarField(dir, time);

    float3 moonDir = normalize(float3(-sunDir.x, abs(sunDir.y) * 0.5f + 0.32f, -sunDir.z));
    float cm = dot(dir, moonDir);
    float moon = smoothstep(0.9975f, 0.9992f, cm);
    float halo = pow(saturate(cm), 1200.0f) * 0.08f;
    night += float3(0.90f, 0.93f, 1.0f) * (moon * 2.0f + halo);
    return night;
}

// --- Clouds: FBM on a sky plane, sun-lit, with coverage/density ---------------
// Returns the lit cloud colour; `alpha` is coverage at this direction.
float3 Clouds(float3 dir, float3 sunDir, float coverage, float density, float time,
              out float alpha)
{
    alpha = 0.0f;
    if (dir.y < 0.03f || coverage <= 0.001f) return 0.0f;
    // Project the ray onto a sky plane; clouds drift along the wind (gWeather1.xy).
    float2 uv = dir.xz / dir.y * 0.55f + gWeather1.xy * time;
    float n = FBM(uv * 1.1f);
    n = FBM(uv * 1.1f + n * 0.6f); // domain warp -> puffier shapes
    float thr = lerp(0.62f, 0.04f, saturate(coverage)); // more coverage -> lower threshold
    float c = smoothstep(thr, thr + 0.28f, n);
    alpha = saturate(c * lerp(0.6f, 1.0f, density));
    alpha *= saturate((dir.y - 0.03f) * 3.0f);           // thin out at the horizon

    // Lighting: bright toward the sun (forward scatter / silver lining) over a
    // darker base, tinted + dimmed by the sun's elevation. The warm sunset glow is
    // confined to the horizon band (dusk/dawn); at true night the clouds take a cool
    // moonlit blue-grey so they don't read as orange against the navy night sky.
    float sunAmt = saturate(dot(dir, sunDir));
    float lit = 0.5f + 0.5f * pow(sunAmt, 4.0f);
    float elev = sunDir.y;                              // sun elevation (-1..1)
    float day = saturate(elev * 4.0f + 0.1f);          // 0 night -> 1 day
    float warmth = saturate(1.0f - abs(elev) * 6.0f);  // glow only near the horizon
    float3 dayTint   = float3(1.0f, 0.98f, 0.92f);
    float3 warmTint  = float3(1.0f, 0.55f, 0.30f);     // sunset orange
    float3 nightTint = float3(0.42f, 0.50f, 0.66f);    // moonlit, cool blue-grey
    float3 tint = lerp(nightTint, dayTint, day);                // night -> day
    tint = lerp(tint, warmTint, warmth * (1.0f - 0.5f * day));  // dusk/dawn glow at the horizon
    float3 base = lerp(float3(0.20f, 0.22f, 0.26f), float3(0.95f, 0.96f, 1.0f), lit);
    float3 col = base * tint * (0.12f + 1.1f * day);
    return col;
}

// --- Volumetric clouds: raymarched density slab, sun light-marched --------------
// The cloud density field at world position `p`, shaped by a height profile (rounded
// cumulus), a coverage threshold, and wind drift, eroded by higher-frequency detail.
float CloudDensity(float3 p, float coverage, float density, float time, float2 wind,
                   float base, float top)
{
    float h = (p.y - base) / max(top - base, 1.0f); // 0..1 within the slab
    if (h < 0.0f || h > 1.0f) return 0.0f;
    // Height profile: rises fast off the base, tapers to the top (rounded tops).
    float heightGrad = saturate(h * 4.0f) * saturate((1.0f - h) * 2.0f);
    float2 drift = wind * time * 120.0f; // world-metre drift (windSpeed is tiny UV/sec)
    float3 wp = float3(p.x + drift.x, p.y, p.z + drift.y);
    float shape = FBM3(wp * 0.0016f); // low-freq base structure (~600 m cells)
    float cov = saturate(coverage);
    float d = saturate(shape - (1.0f - cov) * 0.9f) * heightGrad;
    if (d > 0.0f)
    {
        float detail = FBM3(wp * 0.011f); // erode edges into wisps (~90 m cells)
        d = saturate(d - (1.0f - detail) * 0.28f);
    }
    return d * density * 1.6f;
}

// Marches the cloud slab along the view ray from the camera, accumulating in-scattered
// sun + ambient with Beer-Lambert transmittance and a short sun light-march self-shadow.
// Returns the in-scattered radiance; `transmittance` is how much sky shows through.
float3 VolumetricClouds(float3 ro, float3 rd, float3 sunDir, float coverage, float density,
                        float time, float2 wind, float quality, out float transmittance)
{
    transmittance = 1.0f;
    const float base = 900.0f, top = 2200.0f;
    if (coverage <= 0.001f || rd.y < 0.03f) return 0.0f; // clear, or looking down/horizon
    float t0 = (base - ro.y) / rd.y;
    float t1 = (top - ro.y) / rd.y;
    if (t1 <= 0.0f) return 0.0f;
    t0 = max(t0, 0.0f);
    float marchLen = min(t1 - t0, 6000.0f); // near-horizon rays would be enormous
    int steps = (int)clamp(lerp(28.0f, 96.0f, saturate(quality)), 16.0f, 128.0f);
    float dt = marchLen / (float)steps;

    float elev = sunDir.y;
    float day = saturate(elev * 4.0f + 0.1f);
    float3 sunCol = lerp(float3(0.45f, 0.50f, 0.62f), float3(1.0f, 0.95f, 0.85f), day) *
                    (2.6f * day + 0.15f);
    // Golden-hour warmth: near the horizon the light through the clouds turns orange
    // (mirrors the 2D layer's warmTint), so dawn/dusk clouds glow instead of reading grey.
    float warmth = saturate(1.0f - abs(elev) * 6.0f);
    sunCol = lerp(sunCol, float3(1.0f, 0.50f, 0.28f) * (2.6f * day + 0.15f), warmth * 0.75f);
    float3 ambCol = lerp(float3(0.10f, 0.13f, 0.20f), float3(0.55f, 0.62f, 0.75f), day);
    float mu = dot(rd, sunDir);
    float phase = HGphase(mu, 0.5f) * 0.7f + HGphase(mu, -0.15f) * 0.3f; // dual-lobe

    float3 scatter = 0.0f;
    [loop] for (int i = 0; i < steps; ++i)
    {
        if (transmittance < 0.02f) break;
        float3 p = ro + rd * (t0 + ((float)i + 0.5f) * dt);
        float d = CloudDensity(p, coverage, density, time, wind, base, top);
        if (d > 0.001f)
        {
            float lt = 0.0f; // optical depth toward the sun (self-shadow)
            [unroll] for (int j = 1; j <= 4; ++j)
                lt += CloudDensity(p + sunDir * ((float)j * 90.0f), coverage, density, time,
                                   wind, base, top) * 90.0f;
            float3 lightE = sunCol * exp(-lt) * phase + ambCol;
            float dtrans = exp(-d * dt * 1.2f);
            scatter += transmittance * (1.0f - dtrans) * lightE;
            transmittance *= dtrans;
        }
    }
    // Feather the low-elevation cutoff so clouds ramp in over a few degrees rather than
    // snapping on at the horizon (matching the smooth fade the 2D layer had).
    float horizon = saturate((rd.y - 0.03f) * 8.0f);
    scatter *= horizon;
    transmittance = lerp(1.0f, transmittance, horizon);
    return scatter;
}

float3 SkyRadiance(float3 dir, float3 sunDir, float coverage, float density,
                   float overcast, float time)
{
    const float rPlanet = 6371e3f, rAtmos = 6471e3f;
    const float3 kRlh = float3(5.8e-6f, 13.5e-6f, 33.1e-6f);
    const float kMie = 21e-6f, shRlh = 8000.0f, shMie = 1200.0f, gMie = 0.758f;
    const float3 r0 = float3(0.0f, rPlanet + 1000.0f, 0.0f);
    const float iSun = 22.0f;

    // Daytime scattering. Below the horizon, clamp the ray up + dim as ground.
    float3 vr = dir.y < 0.0f ? normalize(float3(dir.x, 0.03f, dir.z)) : dir;
    float3 day = Atmosphere(vr, r0, sunDir, iSun, rPlanet, rAtmos, kRlh, kMie, shRlh, shMie, gMie);
    if (dir.y < 0.0f) day *= float3(0.22f, 0.20f, 0.18f);

    // Crisp sun disc; hidden behind clouds and as it drops below the horizon.
    float cosSun = dot(dir, sunDir);
    float disc = smoothstep(0.99975f, 0.99995f, cosSun);
    day += disc * (iSun * 6.0f) * float3(1.0f, 0.95f, 0.85f) * saturate(sunDir.y * 8.0f + 0.2f);

    // Night fades in below the horizon (smooth twilight).
    float dayFactor = smoothstep(-0.10f, 0.06f, sunDir.y);
    float3 night = NightSky(dir, sunDir, time);
    float3 sky = lerp(night, day, dayFactor);

    // Overcast: blend the whole sky toward a flat grey ceiling (dimmer at night).
    if (overcast > 0.001f)
    {
        float3 grey = float3(0.5f, 0.52f, 0.55f) * (0.12f + 0.9f * saturate(sunDir.y * 3.0f + 0.1f));
        sky = lerp(sky, grey, saturate(overcast));
        coverage = max(coverage, overcast); // overcast implies cloud cover
    }

    // Clouds: a raymarched VOLUMETRIC slab (gWeather3.z on) with real depth + sun
    // self-shadow, or the cheap 2D sky-plane layer. Both composite over the sky.
    if (gWeather3.z > 0.5f)
    {
        float tr;
        float3 cloudScatter = VolumetricClouds(gCameraPosWS, dir, sunDir, coverage, density,
                                               time, gWeather1.xy, gWeather3.w, tr);
        sky = sky * tr + cloudScatter;
    }
    else
    {
        float cloudA;
        float3 cloud = Clouds(dir, sunDir, coverage, density, time, cloudA);
        sky = lerp(sky, cloud, cloudA);
    }
    return sky;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float4 pNear = mul(gInvViewProj, float4(input.ndc, 0.0f, 1.0f));
    float4 pFar  = mul(gInvViewProj, float4(input.ndc, 1.0f, 1.0f));
    float3 dir = normalize(pFar.xyz / pFar.w - pNear.xyz / pNear.w);

    float3 sunDir = normalize(gLightDirWS); // already points TO the sun
    float3 sky = SkyRadiance(dir, sunDir, gWeather.x, gWeather.y, gWeather.z, gWeather.w);

    if (gOutputLinear != 0)
        return float4(sky, 0.0f); // HDR pipeline: alpha = painterly mask, 0 = paint the sky (it's background)

    float3 color = sky * gExposure;
    color = TonemapACES(color);
    color = LinearToSRGB(color);
    return float4(color, 1.0f);
}
