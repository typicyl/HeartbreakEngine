// Shaders/GrassGen.cs.hlsl - GPU generation of grass blade records.
//
// One thread per blade. Blades tile a grid CENTERED on the camera (snapped to a cell), so
// the field follows the player and only near blades exist. Each blade samples the uploaded
// terrain heightfield for its ground Y, and is deterministic from its cell hash (a moving
// camera re-derives the same blades for the same cells - no popping within a cell). A
// culled blade (beyond range / off-terrain) is written with height 0, which the draw VS
// collapses to a degenerate point - the CPU-known-count + degenerate-cull pattern the
// engine uses instead of indirect draw. Binding convention: b0 = CB, u0 = records,
// t0 = heights (RHI.h compute layout).
[[vk::binding(0, 0)]] cbuffer GrassGenCB : register(b0)
{
    float3 gCamPos;      float gTime;
    float  gCellSize;    uint  gGridDim;    uint  gBladesPerCell; float gMaxDist;
    float  gTerrainExtent; uint gTerrainGridN; float gTerrainStep; float gBladeHeight;
    float  gBladeWidth;  uint  gTotalBlades; float2 _pad;
};
[[vk::binding(1, 0)]] RWByteAddressBuffer gBlades  : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float> gHeights : register(t0);

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float Rand(uint s) { return float(Hash(s) & 0x00FFFFFFu) / 16777216.0f; }

// Bilinear terrain height at world XZ (terrain assumed centered at the origin). Returns a
// large negative when the point is off the terrain footprint (caller culls the blade).
float SampleHeight(float2 wxz)
{
    const int gn = int(gTerrainGridN);
    const float fx = (wxz.x + gTerrainExtent * 0.5f) / gTerrainStep;
    const float fz = (wxz.y + gTerrainExtent * 0.5f) / gTerrainStep;
    if (fx < 0.0f || fz < 0.0f || fx > float(gn - 1) || fz > float(gn - 1)) return -1e9f;
    const int x0 = clamp(int(floor(fx)), 0, gn - 2);
    const int z0 = clamp(int(floor(fz)), 0, gn - 2);
    const float tx = saturate(fx - float(x0));
    const float tz = saturate(fz - float(z0));
    const float h00 = gHeights[z0 * gn + x0];
    const float h10 = gHeights[z0 * gn + x0 + 1];
    const float h01 = gHeights[(z0 + 1) * gn + x0];
    const float h11 = gHeights[(z0 + 1) * gn + x0 + 1];
    return lerp(lerp(h00, h10, tx), lerp(h01, h11, tx), tz);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint bi = id.x;
    if (bi >= gTotalBlades) return;

    const uint cell = bi / gBladesPerCell;
    const uint inCell = bi - cell * gBladesPerCell;
    const int cx = int(cell % gGridDim);
    const int cz = int(cell / gGridDim);

    // The grid FOLLOWS the camera, but each blade is anchored to its ABSOLUTE WORLD cell -
    // so its seed and position depend only on the world cell, never on the grid's current
    // offset. Without this the whole field regenerates (crawls / pops) every time the
    // camera crosses a cell boundary; with it, blades stay put and only the visible window
    // slides.
    const int baseX = int(floor(gCamPos.x / gCellSize)) - int(gGridDim) / 2;
    const int baseZ = int(floor(gCamPos.z / gCellSize)) - int(gGridDim) / 2;
    const int wcx = baseX + cx;
    const int wcz = baseZ + cz;

    const uint seed = (uint(wcx) * 73856093u) ^ (uint(wcz) * 19349663u) ^ (inCell * 83492791u);
    const float jx = Rand(seed);
    const float jz = Rand(seed ^ 0x9e3779b9u);
    const float2 wxz = (float2(float(wcx), float(wcz)) + float2(jx, jz)) * gCellSize;

    const float dist = distance(wxz, gCamPos.xz);
    const float ground = SampleHeight(wxz);
    float height = gBladeHeight * (0.7f + 0.6f * Rand(seed ^ 0x1234u));
    // Smooth fade toward the range edge (no hard pop) + hard cull off the terrain footprint.
    height *= smoothstep(gMaxDist, gMaxDist * 0.8f, dist);
    if (ground < -1e8f) height = 0.0f;

    const float yaw = Rand(seed ^ 0x0000ABCDu) * 6.2831853f;
    const float2 dir = float2(cos(yaw), sin(yaw));
    const float phase = Rand(seed ^ 0x000055AAu) * 6.2831853f;

    const uint o = bi * 32u;
    gBlades.Store3(o, asuint(float3(wxz.x, ground, wxz.y)));
    gBlades.Store(o + 12u, asuint(height));
    gBlades.Store2(o + 16u, asuint(dir));
    gBlades.Store(o + 24u, asuint(gBladeWidth));
    gBlades.Store(o + 28u, asuint(phase));
}
