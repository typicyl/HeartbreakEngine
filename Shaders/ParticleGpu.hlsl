// Shaders/ParticleGpu.hlsl - GPU VERTEX EXPANSION for particle billboards.
//
// This is the GPU twin of particle::BuildVertices (Source/Scene/ParticleSystem.cpp).
// Particle.hlsl's VS is a pure pass-through: the CPU billboards every particle into
// six 40-byte world-space vertices (240 B/particle, 1.2 M verts at the 200k stress
// point) and the VS only multiplies by gViewProj. THAT expansion - not the
// simulation, not overdraw - is what the profile says the frame is spending its CPU
// on. Here the CPU uploads a 64-byte record per particle and NOTHING else, and this
// VS builds the quad from SV_VertexID.
//
// LAYOUT. One buffer, one 64-byte stride, laid out per batch as
//     [emitter record: 2 elements = 128 B][particle 0][particle 1]...
// and the batch is selected by pointing the buffer's BASE at its emitter record
// (D3D12: root-SRV GPU virtual address; Vulkan: dynamic storage-buffer offset).
// Never a firstInstance: D3D12's SV_InstanceID excludes StartInstanceLocation while
// Vulkan's gl_InstanceIndex includes firstInstance, so instancing here would make
// the two backends silently disagree. The draw is DrawInstanced(6*N,1,0,0) /
// vkCmdDraw(cmd, 6*N, 1, 0, 0) with firstInstance = 0 on both.
//
// PARITY. Every arm of BuildVertices is reproduced: the size/colour ramps, the
// fade-in/out envelope, the sub-UV frame, and all THREE render modes including
// Horizontal's world-XZ quads and Stretched's velocity streaks (with its
// per-particle vlen fallback AND its per-emitter hasVel fallback). The one
// deliberate divergence is documented at the sub-UV frame below.
#include "Common.hlsli"
#include "VfxCommon.hlsli"

// The general VS-visible structured buffer seam (RHI::SetVertexShaderBuffer /
// D3D12 root param 6 / Vulkan set 2, binding 0). A ByteAddressBuffer, not a
// StructuredBuffer<T>, because this ONE binding carries both record layouts.
[[vk::binding(0, 2)]] ByteAddressBuffer gVfxRecords : register(t2, space1);

struct VSOutput
{
    float4 positionCS : SV_Position;
    float2 uv         : TEXCOORD0;
    float4 color      : COLOR0;
    nointerpolation uint texIndex : TEXCOORD1;
};

// Corner table for the two triangles the CPU emits, in its exact order:
//   p0=TL(-r,+u)  p1=TR(+r,+u)  p2=BR(+r,-u)  p3=BL(-r,-u)
//   tri 0 = p0,p1,p2   tri 1 = p0,p2,p3
// x = 0 -> -r / u0, 1 -> +r / u1.  y = 0 -> +u / v0, 1 -> -u / v1.
//
// Held as two BIT MASKS, not a `static const uint2[6]`, and that is deliberate: an
// array indexed by a non-literal is one-sided between the backends. DXC scalarises
// it to select/phi for DXIL, but for SPIR-V it emits an OpVariable in Function
// storage plus an OpStore of the whole table and a dynamically-indexed OpAccessChain
// - i.e. a scratch store and a scratch load in EVERY vertex invocation, 480k of them
// per frame at the 80k-particle point, on Vulkan only. Bit i of each mask is corner
// i's component, so this is two shifts and no addressable memory on both.
//   x per corner = 0,1,1,0,1,0 -> 0b010110 = 0x16
//   y per corner = 0,0,1,0,1,1 -> 0b110100 = 0x34
#define kCornerMaskX 0x16u
#define kCornerMaskY 0x34u

VSOutput VSMain(uint vid : SV_VertexID)
{
    const uint pid = vid / 6u;
    const uint corner = vid - pid * 6u;

    // Base element 0 is this batch's emitter record; particles follow it.
    const GpuEmitter em = VfxLoadEmitter(gVfxRecords, 0u);
    const GpuParticle p = VfxLoadParticle(gVfxRecords, kVfxEmitterBytes + pid * kVfxParticleBytes);

    const bool simColor = (em.flags & kVfxEmitterSimColor) != 0u;
    const bool simSize  = (em.flags & kVfxEmitterSimSize) != 0u;
    const bool hasRot   = (em.flags & kVfxEmitterHasRot) != 0u;
    const bool hasVel   = (em.flags & kVfxEmitterHasVel) != 0u;

    const float t = saturate(p.age / max(p.lifetime, 1e-4f));

    // Size: the simulated attribute when the stack produces one, else the ramp.
    const float size = simSize ? p.sizeX : lerp(em.sizeFade.x, em.sizeFade.y, t);

    // Colour: likewise. The envelope is applied ONLY on the ramp path - when the
    // stack simulates colour, ColorOverLife has already baked the envelope in, and
    // applying it twice would square the alpha (same rule as the CPU path).
    float4 col;
    if (simColor)
    {
        col = VfxUnpackColor(p);
    }
    else
    {
        col = lerp(em.startColor, em.endColor, t);
        float env = 1.0f;
        const float fadeIn = em.sizeFade.z, fadeOut = em.sizeFade.w;
        if (fadeIn > 0.0f && t < fadeIn) env *= t / fadeIn;
        if (fadeOut > 0.0f && t > 1.0f - fadeOut) env *= (1.0f - t) / fadeOut;
        col.a *= saturate(env);
    }

    // Sub-UV (sprite-sheet) cell -> local UV rect.
    float2 uvMin = float2(0.0f, 0.0f), uvMax = float2(1.0f, 1.0f);
    const uint cols = max(1u, em.subUV & 0xFFFFu);
    const uint rows = max(1u, em.subUV >> 16);
    if (cols > 1u || rows > 1u)
    {
        const uint cells = cols * rows;
        uint frame;
        if (em.subUVFps > 0.0f)
        {
            // DELIBERATE DIVERGENCE, and the only one. The CPU computes
            // (u32)(age * subUVFps) % cells, which is undefined once the product
            // leaves the u32 range (a long-lived particle on a fast sheet). Float
            // -> uint is equally undefined in HLSL, so clamp instead of inheriting
            // the hazard: past ~4.29e9 frames the animation simply pins.
            frame = ((uint)clamp(p.age * em.subUVFps, 0.0f, 4.29e9f)) % cells;
        }
        else
        {
            frame = min((uint)(t * (float)cells), cells - 1u);
        }
        const uint fx = frame % cols, fy = frame / cols;
        const float du = 1.0f / (float)cols, dv = 1.0f / (float)rows;
        uvMin = float2((float)fx * du, (float)fy * dv);
        uvMax = uvMin + float2(du, dv);
    }

    // Quad axes per render mode. The branch is uniform per EMITTER (one batch = one
    // emitter = one draw), so there is no cross-lane divergence here at all; only
    // Stretched's vlen test below is genuinely per-particle.
    const float rot = hasRot ? p.rotation : 0.0f;
    const float cr = cos(rot), sr = sin(rot);
    const float ext = size * 0.5f;
    float3 axisR, axisU;
    if (em.renderMode == kVfxRenderStretched && hasVel)
    {
        const float2 vs = float2(dot(p.velocity, em.camRight), dot(p.velocity, em.camUp));
        const float vlen = length(vs);
        if (vlen > 1e-4f)
        {
            const float2 vn = vs / vlen;
            const float3 lng = em.camRight * vn.x + em.camUp * vn.y;   // along velocity
            const float3 prp = em.camRight * (-vn.y) + em.camUp * vn.x; // perpendicular
            axisR = prp * ext;
            axisU = lng * (ext * max(1.0f, em.stretch));
        }
        else
        {
            // Degenerate screen-space velocity -> plain billboard (CPU does the same).
            axisR = (em.camRight * cr + em.camUp * sr) * ext;
            axisU = (em.camUp * cr - em.camRight * sr) * ext;
        }
    }
    else if (em.renderMode == kVfxRenderHorizontal)
    {
        // Lies flat in world XZ, spun about Y. No camera term at all.
        axisR = float3(cr, 0.0f, sr) * ext;
        axisU = float3(-sr, 0.0f, cr) * ext;
    }
    else
    {
        // Billboard: camera-facing, spun around the view axis. Also the fallback
        // when the emitter asked for Stretched but the velocity stream was
        // eliminated - exactly the `em.render == Stretched && hasVel` guard the CPU
        // path uses.
        axisR = (em.camRight * cr + em.camUp * sr) * ext;
        axisU = (em.camUp * cr - em.camRight * sr) * ext;
    }

    const uint2 c = uint2((kCornerMaskX >> corner) & 1u, (kCornerMaskY >> corner) & 1u);
    const float3 wp = p.position + (c.x != 0u ? axisR : -axisR) + (c.y != 0u ? -axisU : axisU);

    VSOutput o;
    o.positionCS = mul(gViewProj, float4(wp, 1.0f));
    o.uv = float2(c.x != 0u ? uvMax.x : uvMin.x, c.y != 0u ? uvMax.y : uvMin.y);
    o.color = col;
    o.texIndex = em.texIndex;
    return o;
}

// Identical to Particle.hlsl's PS, on purpose: the two paths must look the same,
// including the procedural soft dot's known quirk that under sub-UV it reads the
// atlas sub-rect UV rather than a quad-local one. Fixing that here and not there
// would make the opt-in flag change the look, which is the one thing it must not do.
float4 PSMain(VSOutput input) : SV_Target
{
    float4 c = input.color;
    if (input.texIndex != 0u)
    {
        c *= gTextures[NonUniformResourceIndex(input.texIndex)].Sample(gBindlessSampler, input.uv);
    }
    else
    {
        float2 d = input.uv * 2.0f - 1.0f;
        float r = saturate(1.0f - dot(d, d));
        c.a *= r * r;
    }
    return c;
}
