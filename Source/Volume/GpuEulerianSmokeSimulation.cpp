// Source/Volume/GpuEulerianSmokeSimulation.cpp - see the header.
#include "Volume/GpuEulerianSmokeSimulation.h"

#include "Core/Log.h"
#include "Renderer/Renderer.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace hbe::volume {
namespace {

// Constant blocks - every member is a 16-byte row (ivec4/vec4) so the C++ and HLSL cbuffer layouts
// are identical without HLSL packing surprises. All well under kMaxComputeConstantBytes (256).
struct InitCB   { glm::ivec4 dim; glm::vec4 amb; };
struct EmitCB   { glm::ivec4 dim; glm::vec4 worldMin_dt, voxel_amb, center_cone, half_soft, rot, vel_dens, rates; };
struct AdvCB    { glm::ivec4 dim; glm::vec4 invVoxel_dt; };
struct BuoyCB   { glm::ivec4 dim; glm::vec4 up_beta, alpha_amb_dt; };
struct VortCB   { glm::ivec4 dim; glm::vec4 invVoxel; };
struct ConfCB   { glm::ivec4 dim; glm::vec4 invVoxel, eps_h_dt; };
struct DivCB    { glm::ivec4 dim; glm::vec4 invVoxel; };
struct ClearCB  { glm::ivec4 dim; };
struct JacobiCB { glm::ivec4 dim; glm::vec4 coeff; };
struct GradCB   { glm::ivec4 dim; glm::vec4 invVoxel; };
struct AdvScalCB{ glm::ivec4 dim; glm::vec4 invVoxel_dt, fades; };

} // namespace

GpuEulerianSmokeSimulation::GpuEulerianSmokeSimulation(const VolumeSimConfig& config,
                                                       Renderer& renderer)
    : config_(config), renderer_(&renderer) {
    bounds_ = config_.bounds;
    dim_ = glm::max(bounds_.dim, glm::ivec3(1));
    bounds_.dim = dim_;
    voxelSize_ = glm::max(bounds_.voxelSize(), glm::vec3(1e-5f));
    invVoxel_ = 1.0f / voxelSize_;
    h_ = (voxelSize_.x + voxelSize_.y + voxelSize_.z) / 3.0f;
    count_ = static_cast<u32>(bounds_.voxelCount());

    fields_.Declare("density", FieldType::Scalar, 0.0f, true);
    fields_.Declare("temperature", FieldType::Scalar, config_.ambientTemperature, true);
    fields_.Declare("velocity", FieldType::Vector3, 0.0f, false);
    fields_.SetExposedByName(config_.bakeFields);

    valid_ = CreateResources();
    if (valid_) Reset();
}

GpuEulerianSmokeSimulation::~GpuEulerianSmokeSimulation() {
    if (!renderer_) return;
    for (rhi::GpuBufferHandle* h :
         {&velA_, &velB_, &sclA_, &sclB_, &curl_, &pressA_, &pressB_, &div_})
        if (h->IsValid()) {
            renderer_->DestroyGpuBuffer(*h);
            *h = {};
        }
    // Pipelines are intentionally NOT freed (no DestroyComputePipeline in the RHI) - see the header.
}

bool GpuEulerianSmokeSimulation::CreateResources() {
    if (!renderer_ || !renderer_->SupportsGpuCompute() || count_ == 0) return false;
    // The GPU solver models a CLOSED BOX only - it has no per-cell solid mask, so interior solid /
    // Sink obstacles are NOT simulated. Refuse those configs (Valid() -> false) so a selection layer
    // falls back to the CPU solver rather than silently producing wrong physics.
    if (!config_.obstacles.empty()) return false;

    auto makeBuf = [&](rhi::GpuBufferHandle& hnd, u32 stride, const char* name) {
        if (hnd.IsValid()) return;
        rhi::GpuBufferDesc d{};
        d.elementCount = count_;
        d.elementStride = stride;
        d.usage = rhi::GpuBufferUsage::ShaderRead | rhi::GpuBufferUsage::ShaderWrite;
        d.debugName = name;
        hnd = renderer_->CreateGpuBuffer(d);
    };
    const u32 v4 = static_cast<u32>(sizeof(glm::vec4));
    const u32 f1 = static_cast<u32>(sizeof(f32));
    makeBuf(velA_, v4, "SmokeVelA");
    makeBuf(velB_, v4, "SmokeVelB");
    makeBuf(sclA_, v4, "SmokeSclA");
    makeBuf(sclB_, v4, "SmokeSclB");
    makeBuf(curl_, v4, "SmokeCurl");
    makeBuf(pressA_, f1, "SmokePressA");
    makeBuf(pressB_, f1, "SmokePressB");
    makeBuf(div_, f1, "SmokeDiv");

    auto makePipe = [&](rhi::ComputePipelineHandle& hnd, const char* shader, u32 cbBytes, u32 uav,
                        u32 srv) {
        if (hnd.IsValid()) return;
        rhi::ComputePipelineDesc d{};
        d.shaderName = shader;
        d.constantBytes = cbBytes;
        d.uavCount = uav;
        d.srvCount = srv;
        d.debugName = shader;
        hnd = renderer_->CreateComputePipeline(d);
    };
    makePipe(pInit_, "SmokeInit", sizeof(InitCB), 2, 0);
    makePipe(pEmit_, "SmokeEmit", sizeof(EmitCB), 2, 0);
    makePipe(pAdvectVel_, "SmokeAdvectVel", sizeof(AdvCB), 1, 1);
    makePipe(pBuoy_, "SmokeBuoyancy", sizeof(BuoyCB), 1, 1);
    makePipe(pVort_, "SmokeVorticity", sizeof(VortCB), 1, 1);
    makePipe(pConfine_, "SmokeConfine", sizeof(ConfCB), 1, 1);
    makePipe(pDiv_, "SmokeDivergence", sizeof(DivCB), 1, 1);
    makePipe(pClear_, "SmokePressureClear", sizeof(ClearCB), 2, 0);
    makePipe(pJacobi_, "SmokeJacobi", sizeof(JacobiCB), 1, 2);
    makePipe(pGradSub_, "SmokeGradSub", sizeof(GradCB), 1, 1);
    makePipe(pAdvectScl_, "SmokeAdvectScalars", sizeof(AdvScalCB), 1, 2);

    return velA_.IsValid() && velB_.IsValid() && sclA_.IsValid() && sclB_.IsValid() &&
           curl_.IsValid() && pressA_.IsValid() && pressB_.IsValid() && div_.IsValid() &&
           pInit_.IsValid() && pEmit_.IsValid() && pAdvectVel_.IsValid() && pBuoy_.IsValid() &&
           pVort_.IsValid() && pConfine_.IsValid() && pDiv_.IsValid() && pClear_.IsValid() &&
           pJacobi_.IsValid() && pGradSub_.IsValid() && pAdvectScl_.IsValid();
}

void GpuEulerianSmokeSimulation::Reset() {
    time_ = 0.0f;
    needsInit_ = true; // the SmokeInit dispatch re-zeroes state at the start of the next Step
    curVel_ = velA_;
    otherVel_ = velB_;
    curScl_ = sclA_;
    otherScl_ = sclB_;
}

void GpuEulerianSmokeSimulation::Step(f32 dt) {
    if (!valid_) return;
    if (count_ == 0 || dt <= 0.0f) { time_ += std::max(dt, 0.0f); return; }

    const glm::ivec4 dim4(dim_, 0);
    const u32 gx = (dim_.x + 7u) / 8u, gy = (dim_.y + 7u) / 8u, gz = (dim_.z + 7u) / 8u;

    auto queue = [&](rhi::ComputePipelineHandle pipe, const void* cb, u32 cbBytes,
                     std::initializer_list<rhi::GpuBufferHandle> uavs,
                     std::initializer_list<rhi::GpuBufferHandle> srvs) {
        rhi::ComputeDispatch d{};
        d.pipeline = pipe;
        d.constants = cb;
        d.constantBytes = cbBytes;
        u32 u = 0;
        for (rhi::GpuBufferHandle h : uavs) d.uavs[u++] = h;
        d.uavCount = u;
        u32 s = 0;
        for (rhi::GpuBufferHandle h : srvs) d.srvs[s++] = h;
        d.srvCount = s;
        d.groupsX = gx;
        d.groupsY = gy;
        d.groupsZ = gz;
        renderer_->QueueCompute(d);
    };

    // Resolve active emitters up front so the dispatch budget is known before queuing.
    std::vector<VolumeEmitter::Resolved> active;
    active.reserve(config_.emitters.size());
    for (const VolumeEmitter& em : config_.emitters) {
        const VolumeEmitter::Resolved r = em.Resolve(time_);
        if (r.active && r.strength > 0.0f) active.push_back(r);
    }
    const bool confineOn = config_.vorticityStrength > 0.0f;

    // The WHOLE substep must fit in ONE drain: QueueCompute silently DROPS dispatches past the cap,
    // and the tail (GradSub + AdvectScalars) is exactly what would be lost -> the pressure projection
    // would vanish and the field would blow up with only a log line. Fixed passes = advectVel,
    // buoyancy, vorticity, divergence, pressureClear, gradSub, advectScalars (7) + optional confine +
    // optional init + one per emitter. Clamp the Jacobi count to the remaining budget (a quality
    // reduction, never silent corruption). This assumes exactly ONE Step() per BeginFrame drain (the
    // frame-driven usage) - do NOT call Step() several times before a frame flushes.
    const int fixed =
        (needsInit_ ? 1 : 0) + static_cast<int>(active.size()) + (confineOn ? 1 : 0) + 7;
    int iters = std::max(config_.pressureIterations, 1);
    const int maxIters = static_cast<int>(rhi::kMaxQueuedComputeDispatches) - fixed;
    if (iters > maxIters) {
        HBE_WARN("GPU smoke: {} pressure iterations + {} other dispatches exceed the {}-dispatch "
                 "queue; clamping Jacobi to {}. Lower pressureIterations or emitter count.",
                 iters, fixed, static_cast<int>(rhi::kMaxQueuedComputeDispatches),
                 std::max(1, maxIters));
        iters = std::max(1, maxIters);
    }

    // 0. One-time state init (Reset): zero curVel_/curScl_ (== CPU Reset). Queued in this same drain
    //    so it strictly precedes this substep's passes (inter-dispatch barriers serialize it).
    if (needsInit_) {
        InitCB cb{dim4, glm::vec4(config_.ambientTemperature, 0, 0, 0)};
        queue(pInit_, &cb, sizeof(cb), {curVel_, curScl_}, {});
        needsInit_ = false;
    }

    // 1. Emit sources - one dispatch per active emitter (emitter data via the constant block).
    for (const VolumeEmitter::Resolved& r : active) {
        EmitCB cb;
        cb.dim = dim4;
        cb.worldMin_dt = glm::vec4(bounds_.worldMin, dt);
        cb.voxel_amb = glm::vec4(voxelSize_, config_.ambientTemperature);
        cb.center_cone = glm::vec4(r.shape.center, r.shape.coneHeight);
        cb.half_soft = glm::vec4(r.shape.halfExtents, r.shape.edgeSoftness);
        cb.rot = glm::vec4(r.shape.rotation.x, r.shape.rotation.y, r.shape.rotation.z,
                           r.shape.rotation.w);
        cb.vel_dens = glm::vec4(r.velocity, r.densityRate);
        cb.rates = glm::vec4(r.temperatureRate, r.temperatureTarget, r.fuelRate,
                             static_cast<f32>(static_cast<int>(r.shape.kind)));
        queue(pEmit_, &cb, sizeof(cb), {curScl_, curVel_}, {});
    }

    // 2. Advect velocity: srv curVel_ -> uav otherVel_; swap so curVel_ is the advected field.
    {
        AdvCB cb{dim4, glm::vec4(invVoxel_, dt)};
        queue(pAdvectVel_, &cb, sizeof(cb), {otherVel_}, {curVel_});
        std::swap(curVel_, otherVel_);
    }
    // 3. Buoyancy (in-place on curVel_).
    {
        BuoyCB cb{dim4, glm::vec4(config_.up(), config_.buoyancyBeta),
                  glm::vec4(config_.buoyancyAlpha, config_.ambientTemperature, dt, 0.0f)};
        queue(pBuoy_, &cb, sizeof(cb), {curVel_}, {curScl_});
    }
    // 4. Vorticity (curl + |curl|).
    {
        VortCB cb{dim4, glm::vec4(invVoxel_, 0.0f)};
        queue(pVort_, &cb, sizeof(cb), {curl_}, {curVel_});
    }
    // 5. Vorticity confinement (in-place on curVel_); skipped when eps<=0 to match CPU.
    if (confineOn) {
        ConfCB cb{dim4, glm::vec4(invVoxel_, 0.0f),
                  glm::vec4(config_.vorticityStrength, h_, dt, 0.0f)};
        queue(pConfine_, &cb, sizeof(cb), {curVel_}, {curl_});
    }
    // 6. Divergence.
    {
        DivCB cb{dim4, glm::vec4(invVoxel_, 0.0f)};
        queue(pDiv_, &cb, sizeof(cb), {div_}, {curVel_});
    }
    // 7. Clear both pressure buffers.
    {
        ClearCB cb{dim4};
        queue(pClear_, &cb, sizeof(cb), {pressA_, pressB_}, {});
    }
    // 8. Jacobi pressure solve (ping-pong). After the loop, pIn holds the final iterate.
    {
        const glm::vec3 c(invVoxel_.x * invVoxel_.x, invVoxel_.y * invVoxel_.y,
                          invVoxel_.z * invVoxel_.z);
        JacobiCB cb{dim4, glm::vec4(c, 0.0f)};
        rhi::GpuBufferHandle pIn = pressA_, pOut = pressB_;
        for (int it = 0; it < iters; ++it) { // iters is the budget-clamped count computed above
            queue(pJacobi_, &cb, sizeof(cb), {pOut}, {pIn, div_});
            std::swap(pIn, pOut);
        }
        // 9. Gradient subtract (in-place on curVel_), reading the final pressure (pIn).
        GradCB gcb{dim4, glm::vec4(invVoxel_, 0.0f)};
        queue(pGradSub_, &gcb, sizeof(gcb), {curVel_}, {pIn});
    }
    // 10. Advect scalars by the projected velocity (+dissipation/cooling): srv curScl_,curVel_ ->
    //     uav otherScl_; swap.
    {
        const f32 densFade = std::exp(-config_.densityDissipation * dt);
        const f32 tempFade = std::exp(-config_.temperatureCooling * dt);
        AdvScalCB cb{dim4, glm::vec4(invVoxel_, dt),
                     glm::vec4(densFade, tempFade, config_.ambientTemperature, 0.0f)};
        queue(pAdvectScl_, &cb, sizeof(cb), {otherScl_}, {curScl_, curVel_});
        std::swap(curScl_, otherScl_);
    }

    time_ += dt;
}

void GpuEulerianSmokeSimulation::ReadbackFrame(VolumeFrame& out) {
    out.time = time_;
    out.bounds = bounds_;
    for (const VolumeFieldDecl& d : fields_.Decls())
        if (d.exposed) out.ensureField(d.name, d.type, d.background);

    // No Step() since Reset() -> the device buffers hold UNINITIALIZED memory (SmokeInit runs inside
    // the first Step). Return the deterministic CLEARED state (== CPU Reset), never a GPU read of
    // garbage. Fill explicitly - a reused VolumeFrame may carry stale data from a prior readback.
    if (needsInit_ || !valid_ || count_ == 0) {
        if (VolumeField* d = out.field("density")) d->data.assign(count_, 0.0f);
        if (VolumeField* t = out.field("temperature"))
            t->data.assign(count_, config_.ambientTemperature);
        if (VolumeField* v = out.field("velocity"))
            v->data.assign(static_cast<usize>(count_) * 3, 0.0f);
        return;
    }

    std::vector<glm::vec4> scl(count_);
    const bool okScl =
        renderer_->ReadGpuBuffer(curScl_, scl.data(), count_ * static_cast<u32>(sizeof(glm::vec4)));
    if (okScl) {
        if (VolumeField* dens = out.field("density")) {
            dens->data.resize(count_);
            for (u32 i = 0; i < count_; ++i) dens->data[i] = scl[i].x;
        }
        if (VolumeField* temp = out.field("temperature")) {
            temp->data.resize(count_);
            for (u32 i = 0; i < count_; ++i) temp->data[i] = scl[i].y;
        }
    }
    if (VolumeField* vel = out.field("velocity")) {
        std::vector<glm::vec4> v(count_);
        if (renderer_->ReadGpuBuffer(curVel_, v.data(),
                                     count_ * static_cast<u32>(sizeof(glm::vec4)))) {
            vel->data.resize(count_ * 3);
            for (u32 i = 0; i < count_; ++i) {
                vel->data[i * 3 + 0] = v[i].x;
                vel->data[i * 3 + 1] = v[i].y;
                vel->data[i * 3 + 2] = v[i].z;
            }
        }
    }
}

} // namespace hbe::volume
