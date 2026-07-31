#ifndef HBE_VFX_COMMON_HLSLI
#define HBE_VFX_COMMON_HLSLI

// VfxCommon.hlsli - GPU mirror of Source/Vfx/VfxTypes.h.
//
// Source/Vfx/VfxTypes.h carries three static_asserts that name THIS FILE as the
// byte-for-byte mirror of `struct GpuParticle`. Until now the file did not exist,
// so those asserts pinned a contract with nothing on the other side; this closes
// that. RHI rule 2 applies: change GpuParticle in VfxTypes.h, this struct, and the
// upload path together or not at all.
//
// The record is EXACTLY 64 bytes = one cache line. HLSL packs StructuredBuffer
// elements at 4-byte granularity with no implicit padding between scalars, and
// glm::vec3 is 12 bytes with 4-byte alignment under this project's GLM config
// (GLM_FORCE_DEFAULT_ALIGNED_GENTYPES is NOT defined) - that is what makes these
// offsets agree. Field order below is load-bearing; do not reorder to "tidy" it.
//
//   offset  field
//        0  position  (float3)
//       12  age
//       16  velocity  (float3)
//       28  lifetime
//       32  colorRG   (uint, half2)
//       36  colorBA   (uint, half2)
//       40  sizeX
//       44  sizeY
//       48  rotation
//       52  rotationRate
//       56  subImage
//       60  seed      (flags in the HIGH 8 BITS - see below)
struct GpuParticle {
    float3 position;
    float  age;
    float3 velocity;
    float  lifetime;
    uint   colorRG;      // half4 linear HDR, packed as two uints
    uint   colorBA;
    float  sizeX;
    float  sizeY;
    float  rotation;
    float  rotationRate;
    float  subImage;
    uint   seed;         // low 24 bits = RNG state, high 8 bits = flags
};

// ---------------------------------------------------------------------------
// GpuEmitter - the 128-byte per-emitter render record (2 GpuParticle elements)
// ---------------------------------------------------------------------------
//
// Mirror of `struct GpuEmitter` in Source/Vfx/VfxTypes.h, whose static_asserts pin
// every offset below. It carries the values BuildVertices reads as loop invariants
// on the CPU - the colour/size ramps, the fade envelope, the sub-UV grid, the
// render mode, the sprite index, the stream-presence flags - plus the CAMERA BASIS,
// which lives here rather than in FrameConstants because the CPU basis is roll-free
// and world-up derived and reconstructing it from the view matrix would silently
// change how a rolled camera renders particles.
//
//   offset  field
//        0  startColor (float4)
//       16  endColor   (float4)
//       32  sizeFade   (float4: startSize, endSize, fadeIn, fadeOut)
//       48  texIndex   (uint)
//       52  subUV      (uint: cols | rows<<16)
//       56  subUVFps   (float)
//       60  flags      (uint, see kVfxEmitter*)
//       64  camRight   (float3)
//       76  stretch    (float)
//       80  camUp      (float3)
//       92  renderMode (uint)
//       96  padding to 128
struct GpuEmitter {
    float4 startColor;
    float4 endColor;
    float4 sizeFade;
    uint   texIndex;
    uint   subUV;
    float  subUVFps;
    uint   flags;
    float3 camRight;
    float  stretch;
    float3 camUp;
    uint   renderMode;
};

// Stream-presence flags (GpuEmitterFlag in VfxTypes.h). They say whether the
// per-particle STREAM EXISTS, not which value to prefer: dead-stream elimination
// means an unused stream has no memory behind it, so the CPU gather substitutes a
// default and these bits tell the VS which case it is reading.
static const uint kVfxEmitterSimColor = 1u << 0;
static const uint kVfxEmitterSimSize  = 1u << 1;
static const uint kVfxEmitterHasRot   = 1u << 2;
static const uint kVfxEmitterHasVel   = 1u << 3;

// ParticleEmitter::Render (Scene/Components.h). Order is authored-asset state.
static const uint kVfxRenderBillboard  = 0u;
static const uint kVfxRenderStretched  = 1u;
static const uint kVfxRenderHorizontal = 2u;

// Byte sizes of the two records. A batch is one emitter record followed by its
// particles, so particle i sits at kVfxEmitterBytes + i * kVfxParticleBytes.
static const uint kVfxParticleBytes = 64u;
static const uint kVfxEmitterBytes  = 128u;

// Decoders. Both records are read out of a ByteAddressBuffer rather than a
// StructuredBuffer<T> because ONE buffer carries BOTH layouts (that is what lets a
// batch be selected by a single buffer offset), and a structured buffer can only
// have one element type. Every offset below is pinned by a static_assert in
// VfxTypes.h.
GpuParticle VfxLoadParticle(ByteAddressBuffer b, uint off) {
    const uint4 w0 = b.Load4(off + 0u);
    const uint4 w1 = b.Load4(off + 16u);
    const uint4 w2 = b.Load4(off + 32u);
    const uint4 w3 = b.Load4(off + 48u);
    GpuParticle p;
    p.position = asfloat(w0.xyz);
    p.age = asfloat(w0.w);
    p.velocity = asfloat(w1.xyz);
    p.lifetime = asfloat(w1.w);
    p.colorRG = w2.x;
    p.colorBA = w2.y;
    p.sizeX = asfloat(w2.z);
    p.sizeY = asfloat(w2.w);
    p.rotation = asfloat(w3.x);
    p.rotationRate = asfloat(w3.y);
    p.subImage = asfloat(w3.z);
    p.seed = w3.w;
    return p;
}

GpuEmitter VfxLoadEmitter(ByteAddressBuffer b, uint off) {
    const uint4 w0 = b.Load4(off + 0u);
    const uint4 w1 = b.Load4(off + 16u);
    const uint4 w2 = b.Load4(off + 32u);
    const uint4 w3 = b.Load4(off + 48u);
    const uint4 w4 = b.Load4(off + 64u);
    const uint4 w5 = b.Load4(off + 80u);
    GpuEmitter e;
    e.startColor = asfloat(w0);
    e.endColor = asfloat(w1);
    e.sizeFade = asfloat(w2);
    e.texIndex = w3.x;
    e.subUV = w3.y;
    e.subUVFps = asfloat(w3.z);
    e.flags = w3.w;
    e.camRight = asfloat(w4.xyz);
    e.stretch = asfloat(w4.w);
    e.camUp = asfloat(w5.xyz);
    e.renderMode = w5.w;
    return e;
}

// The half4 linear-HDR colour packed into colorRG/colorBA. C++ twin:
// glm::packHalf2x16 at the gather site.
float4 VfxUnpackColor(GpuParticle p) {
    return float4(f16tof32(p.colorRG & 0xFFFFu), f16tof32(p.colorRG >> 16),
                  f16tof32(p.colorBA & 0xFFFFu), f16tof32(p.colorBA >> 16));
}

// Flags <-> seed packing. On the GPU AttrBit(Seed) and AttrBit(Flags) ALIAS: the
// xorshift stream only needs 24 bits of state to stay well distributed, and
// re-hashing on read restores the avalanche. That is what keeps the record at 64
// bytes instead of 80. C++ twins: SeedOf / FlagsOf / PackSeedFlags in VfxTypes.h.
uint VfxSeedOf(uint packed) { return packed & 0x00FFFFFFu; }
uint VfxFlagsOf(uint packed) { return packed >> 24; }
uint VfxPackSeedFlags(uint seed, uint flags) {
    return (seed & 0x00FFFFFFu) | ((flags & 0xFFu) << 24);
}

// PCG-style spawn hash. Every particle's stream is derived PURELY from
// (emitter seed, spawn index) - never a global counter, never a wall clock - which
// is what makes a movie capture reproducible. Bit-exact port of HashSeed in
// VfxTypes.h; a CPU/GPU sim of the same emitter agreeing depends on it.
uint VfxHashSeed(uint emitterSeed, uint spawnIndex) {
    uint s = emitterSeed ^ (spawnIndex * 0x9E3779B9u);
    s ^= s >> 16;
    s *= 0x7FEB352Du;
    s ^= s >> 15;
    s *= 0x846CA68Bu;
    s ^= s >> 16;
    return s | 1u; // never 0: xorshift's fixed point
}

// xorshift32, 13/17/5 - the exact shift triple hbe::vfx::Rng uses.
uint VfxRngNext(inout uint state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// [0,1). 24 bits of mantissa - exactly what an f32 represents losslessly.
float VfxRngNext01(inout uint state) {
    return float(VfxRngNext(state) & 0x00FFFFFFu) / 16777216.0;
}

// [-1,1].
float VfxRngNextSigned(inout uint state) { return VfxRngNext01(state) * 2.0 - 1.0; }

// Uniform direction on the unit sphere (inverse-CDF on z, so no rejection loop -
// a rejection loop would make cost data-dependent and wreck lane coherence).
float3 VfxRngNextUnit(inout uint state) {
    const float z = VfxRngNextSigned(state);
    const float a = VfxRngNext01(state) * 6.28318530718;
    const float r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(a), r * sin(a), z);
}

#endif // HBE_VFX_COMMON_HLSLI
