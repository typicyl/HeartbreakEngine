// Shaders/StrokeSurface.hlsl - TRUE 3D painterly: brush strokes as real geometry
// on the mesh surfaces, lit by the scene's PBR lighting.
//
// NOT a screen-space post effect. Each instance is a brush-stroke seed scattered
// on a mesh surface (object space, from StrokeGen). The VS transforms it to world
// space by the object's model matrix and expands it into a small CAMERA-FACING
// card sized in WORLD UNITS (so far strokes shrink + merge = automatic LOD),
// oriented along the surface tangent so strokes follow the form. The PS shades the
// card with the SURFACE normal under the real directional light + shadows +
// ambient/IBL, so each stroke physically picks up the light's colour. Drawn over
// the lit mesh (the "rough colour" underpainting), depth-tested against the scene
// (nearer geometry occludes), alpha-blended.
//
// Reuses ObjectConstants (b1: gModel / albedo / paint canvas) per draw and the
// global stroke params in FrameConstants (gStroke0/1). Emits linear radiance.
#include "Common.hlsli"

struct VSInput
{
    // Per-instance (matches rhi::StrokeInstance).
    float3 posOS     : POSITION;
    float3 normalOS  : NORMAL;
    float3 tangentOS : TANGENT;
    float2 uv        : TEXCOORD0;
    float  seed      : TEXCOORD1;
    uint   vid       : SV_VertexID;
};

struct VSOutput
{
    float4 posCS    : SV_Position;
    float2 luv      : TEXCOORD0; // local quad uv (x along stroke, y across)
    float2 suv      : TEXCOORD1; // surface uv (albedo / paint lookup, sampled in PS)
    float3 normalWS : TEXCOORD2; // SURFACE normal (for lighting)
    float3 worldPos : TEXCOORD3; // surface point (for shadows)
    float  seed     : TEXCOORD4;
};

float Hash11(float n) { return frac(sin(n) * 43758.5453f); }

VSOutput VSMain(VSInput i)
{
    VSOutput o;

    const float3 surfPos = mul(gModel, float4(i.posOS, 1.0f)).xyz;
    const float3 normalWS = normalize(mul((float3x3)gNormalMatrix, i.normalOS));

    const float3 toCam = normalize(gCameraPosWS - surfPos);
    const float dist = length(gCameraPosWS - surfPos);

    const float sizeW = gStroke0.x;
    const float widthFrac = gStroke0.y;
    const float flow = saturate(gStroke0.w);
    const float sizeJit = gStroke1.y;
    const float angleJit = gStroke1.z;

    // --- surface frame: strokes live ON the surface (object tangent -> world,
    // orthonormalized to the normal). The stroke direction is a true 3D vector in
    // the surface plane, so it follows the form and NEVER collapses to screen-
    // horizontal the way a camera-space angle did on side walls.
    const float3 N = normalWS;
    float3 T = mul((float3x3)gModel, i.tangentOS);
    T = T - N * dot(N, T);
    if (dot(T, T) < 1e-6f) T = (abs(N.y) < 0.99f) ? cross(N, float3(0, 1, 0)) : float3(1, 0, 0);
    T = normalize(T);
    const float3 Bn = cross(N, T); // surface bitangent

    // Per-stroke direction within the surface plane: a base bias along the tangent
    // plus per-stroke spread (more scatter at low flow), so strokes follow the
    // form with natural variation.
    const float a = (Hash11(i.seed * 17.3f) - 0.5f) * angleJit * 3.14159265f +
                    (1.0f - flow) * (i.seed - 0.5f) * 2.4f;
    const float3 dirSurf = normalize(cos(a) * T + sin(a) * Bn);

    // --- cylindrical billboard AROUND the stroke direction: long axis stays the
    // surface direction; the width axis turns to face the camera so the mark is
    // always visible (no edge-on disappearing at grazing angles).
    float3 widthAxis = cross(dirSurf, toCam);
    if (dot(widthAxis, widthAxis) < 1e-5f) widthAxis = Bn;
    widthAxis = normalize(widthAxis);

    // --- size in WORLD units, grown with distance + grazing angle so strokes
    // automatically MERGE into broader marks far away / at glancing angles.
    const float graze = 1.0f - saturate(dot(N, toCam));
    const float mergeScale = (1.0f + dist * 0.02f) * (1.0f + graze * 0.35f);
    const float len = sizeW * mergeScale *
                      (1.0f + (Hash11(i.seed * 13.1f) - 0.5f) * 2.0f * sizeJit);
    const float wid = len * widthFrac;

    const float2 corners[6] = {float2(-0.5f, -0.5f), float2(0.5f, -0.5f), float2(0.5f, 0.5f),
                               float2(-0.5f, -0.5f), float2(0.5f, 0.5f),  float2(-0.5f, 0.5f)};
    const float2 cr = corners[i.vid];
    const float3 offset = cr.x * len * dirSurf + cr.y * wid * widthAxis;
    // Lift off the surface (along its normal) + a touch toward the camera so the
    // card isn't z-killed by the very surface it sits on.
    const float3 cardPos = surfPos + N * (wid * 0.3f) + toCam * (len * 0.08f) + offset;

    o.posCS = mul(gViewProj, float4(cardPos, 1.0f));
    o.luv = cr + 0.5f;
    o.suv = i.uv; // sampled in the PS (the static samplers are pixel-stage only)
    o.normalWS = normalWS;
    o.worldPos = surfPos;
    o.seed = i.seed;
    return o;
}

float4 PSMain(VSOutput i) : SV_Target
{
    const float sharp = saturate(gStroke0.z);
    const float bristle = saturate(gStroke1.x);

    const float u = i.luv.x, v = i.luv.y;
    // Flat-brush footprint; sharpness pushes the falloff to the edge (crisp marks).
    const float wEdge = lerp(0.20f, 0.46f, sharp);
    const float lEdge = lerp(0.30f, 0.47f, sharp);
    float a = smoothstep(0.5f, wEdge, abs(v - 0.5f)) * smoothstep(0.5f, lEdge, abs(u - 0.5f));
    // Bristle streaks running along the stroke (smooth value noise across width).
    const float vb = v * 14.0f + i.seed * 23.0f;
    const float br = lerp(Hash11(floor(vb)), Hash11(floor(vb) + 1.0f), frac(vb));
    a *= lerp(1.0f, 0.35f + br * 0.9f, bristle);
    if (a <= 0.01f) discard;

    // The colour the stroke lays down = the AVERAGE colour of the material it sits
    // on (broad painted masses, not per-texel detail). Sample a coarse mip; the
    // mip rises with distance so far strokes average even more. Art Editor paint
    // canvas wins, else the base-colour map, else the material factor.
    const float dist = length(gCameraPosWS - i.worldPos);
    const float lod = 2.5f + log2(1.0f + dist) * 0.5f;
    float3 albedo = gBaseColorFactor.rgb;
    if (gPaintColorIndex != 0u)
    {
        float4 pc = SampleBindlessLod(gPaintColorIndex, i.suv, lod);
        albedo = lerp(albedo, pc.rgb, saturate(pc.a * gPaintOpacity));
    }
    else if (gAlbedoIndex != 0u)
    {
        albedo = SampleBindlessLod(gAlbedoIndex, i.suv, lod).rgb;
    }

    // Light the stroke by the SURFACE normal under the real scene lighting, so a
    // coloured light bleeds its hue into the paint and shadows read correctly.
    const float3 N = normalize(i.normalWS);
    const float3 L = normalize(gLightDirWS);
    const float NdotL = saturate(dot(N, L));
    const float shadow = ShadowFactor(i.worldPos, NdotL);
    const float3 direct = gLightColor * gLightIntensity * NdotL * shadow;
    float3 ambient = gAmbientIntensity.xxx;
    if (gIrradianceIndex != 0u)
        ambient = SampleBindlessLod(gIrradianceIndex, EquirectUV(N), 0.0f).rgb * gAmbientIntensity;

    float3 lit = albedo * (direct + ambient);
    lit *= 0.9f + 0.2f * Hash11(i.seed * 7.3f); // per-stroke value jitter (hand-mixed paint)
    lit *= 0.85f + 0.15f * br;                    // ridge shading in the bristle gaps

    return float4(max(lit, 0.0f), saturate(a));
}
