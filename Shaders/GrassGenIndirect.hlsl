// Shaders/GrassGenIndirect.cs.hlsl - GPU generation + COMPACTION of grass blades.
//
// The compaction twin of GrassGen.cs. Same per-blade math (camera-centered grid, world-cell
// anchored seed, terrain height sample, distance fade), but instead of writing one record per
// grid slot and letting the draw VS collapse culled blades to a point, this kernel appends
// ONLY the blades that survive the cull into a DENSE buffer via an atomic counter, and writes
// an indirect draw-arg buffer {vtxPerInstance=6, instanceCount, 0, 0}. GrassIndirect.hlsl then
// draws 6 verts x instanceCount instances (one blade per instance) via ExecuteIndirect /
// vkCmdDrawIndirect - so the vertex shader only runs for visible blades.
//
// Two dispatches drive this, distinguished by gResetOnly (both use THIS pipeline):
//   pass 0 (gResetOnly=1, 1 group): thread 0 writes args {6,0,0,0} - resets the counter.
//   pass 1 (gResetOnly=0, full):    each thread tests a grid slot and appends if visible.
// The engine's inter-dispatch UAV barrier serializes pass 0 before pass 1 (the fluid-solver
// read-after-write precedent). Binding: b0 = CB, u0 = compacted records, u1 = draw args,
// t0 = heights (RHI compute layout: uav i -> binding 1+i, srv i -> binding 1+uavCount+i).
[[vk::binding(0, 0)]] cbuffer GrassGenCB : register(b0)
{
    float3 gCamPos;      float gTime;
    float  gCellSize;    uint  gGridDim;    uint  gBladesPerCell; float gMaxDist;
    float  gTerrainExtent; uint gTerrainGridN; float gTerrainStep; float gBladeHeight;
    float  gBladeWidth;  uint  gTotalBlades; uint gResetOnly; float _pad;
};
[[vk::binding(1, 0)]] RWByteAddressBuffer gBlades : register(u0);
[[vk::binding(2, 0)]] RWByteAddressBuffer gArgs   : register(u1);
[[vk::binding(3, 0)]] StructuredBuffer<float> gHeights : register(t0);

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float Rand(uint s) { return float(Hash(s) & 0x00FFFFFFu) / 16777216.0f; }

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
    // Pass 0: reset the indirect draw args (6 verts/instance, 0 instances so far).
    if (gResetOnly != 0u)
    {
        if (id.x == 0u) gArgs.Store4(0u, uint4(6u, 0u, 0u, 0u));
        return;
    }

    const uint bi = id.x;
    if (bi >= gTotalBlades) return;

    const uint cell = bi / gBladesPerCell;
    const uint inCell = bi - cell * gBladesPerCell;
    const int cx = int(cell % gGridDim);
    const int cz = int(cell / gGridDim);

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
    height *= smoothstep(gMaxDist, gMaxDist * 0.8f, dist);

    // COMPACT: skip blades the non-indirect path would have drawn as a degenerate point
    // (off-terrain, or faded to ~nothing at the range edge). Only survivors get an instance.
    if (ground < -1e8f || height < 0.005f) return;

    const float yaw = Rand(seed ^ 0x0000ABCDu) * 6.2831853f;
    const float2 dir = float2(cos(yaw), sin(yaw));
    const float phase = Rand(seed ^ 0x000055AAu) * 6.2831853f;

    // Append: atomically claim the next dense slot and bump the instance count in one step.
    uint slot;
    gArgs.InterlockedAdd(4u, 1u, slot); // offset 4 = InstanceCount word of {vtx,inst,0,0}
    if (slot >= gTotalBlades) return;   // never happens (survivors <= grid slots), but safe

    const uint o = slot * 32u;
    gBlades.Store3(o, asuint(float3(wxz.x, ground, wxz.y)));
    gBlades.Store(o + 12u, asuint(height));
    gBlades.Store2(o + 16u, asuint(dir));
    gBlades.Store(o + 24u, asuint(gBladeWidth));
    gBlades.Store(o + 28u, asuint(phase));
}
