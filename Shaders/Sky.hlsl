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
    // Deep navy night: a real night sky isn't black - it keeps a blue glow,
    // brighter (slightly teal) at the horizon, dark navy overhead.
    float3 night = lerp(float3(0.028f, 0.050f, 0.090f), float3(0.010f, 0.020f, 0.050f), up);
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

    // Clouds composited over the sky (hide the stars/sun where opaque).
    float cloudA;
    float3 cloud = Clouds(dir, sunDir, coverage, density, time, cloudA);
    sky = lerp(sky, cloud, cloudA);
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
