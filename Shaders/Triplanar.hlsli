// Triplanar.hlsli - 3-axis abs(normal) triplanar helpers for the unified material-layer system.
//
// The engine's ONLY prior world-space projection is the terrain-splat single-plane world-XZ block
// (MeshPBR.hlsl). This generalises it to a 3-axis triplanar blend so a material tiles without UV
// stretch on ANY face orientation (walls AND floors AND overhangs). Pure math (no bindless / no
// cbuffer dependency) so it compiles standalone (MaterialLayeredTest.hlsl) on both DXIL and SPIR-V.
#ifndef HBE_TRIPLANAR_HLSLI
#define HBE_TRIPLANAR_HLSLI

// Per-axis blend weights from the surface normal, sharpened + normalised (sum == 1).
float3 TriplanarWeights(float3 n)
{
    float3 w = abs(n);
    w = max(w - 0.2f, 0.0f);          // sharpen the transition so faces don't smear
    float s = w.x + w.y + w.z + 1e-5f;
    return w / s;
}

// World-aligned tangent frame matching the world-XZ tiling (u = world +X), identical to the
// terrain-splat frame in MeshPBR.hlsl so blended normal maps apply consistently on meshes with
// no per-vertex tangents.
void WorldTangentFrame(float3 n, out float3 t, out float3 b)
{
    float3 nn = normalize(n);
    t = normalize(float3(1.0f, 0.0f, 0.0f) - nn * nn.x);
    b = cross(t, nn); // +Z-ish (matches the green-up normal-map convention)
}

// Sample a bindless texture triplanar-style is done in the caller (it owns gTextures[]); this
// helper only supplies the weights + the per-axis UVs so the sampling site stays where the
// bindless array is visible.
//   axisUV.xy = ZY plane (X-facing), axisUV.zw would need a second call; callers use the three
//   planes: (worldPos.zy), (worldPos.xz), (worldPos.xy), each divided by the matching tile.
void TriplanarUVs(float3 worldPos, float3 tileMeters,
                  out float2 uvX, out float2 uvY, out float2 uvZ)
{
    uvX = worldPos.zy / max(tileMeters.zy, 1e-4f); // X-facing plane
    uvY = worldPos.xz / max(tileMeters.xz, 1e-4f); // Y-facing plane (floors)
    uvZ = worldPos.xy / max(tileMeters.xy, 1e-4f); // Z-facing plane (walls)
}

#endif // HBE_TRIPLANAR_HLSLI
