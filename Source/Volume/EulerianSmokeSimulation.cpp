// Source/Volume/EulerianSmokeSimulation.cpp - see the header.
#include "Volume/EulerianSmokeSimulation.h"

#include "Core/JobSystem.h"
#include "Volume/VolumeRasterize.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace hbe::volume {
namespace {

constexpr u32 kJobGroup = 2048; // voxels per job (fiber ParallelFor grain)

f32 Clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

EulerianSmokeSimulation::EulerianSmokeSimulation(const VolumeSimConfig& config) : config_(config) {
    bounds_ = config_.bounds;
    dim_ = glm::max(bounds_.dim, glm::ivec3(1));
    bounds_.dim = dim_;
    voxelSize_ = bounds_.voxelSize();
    voxelSize_ = glm::max(voxelSize_, glm::vec3(1e-5f));
    invVoxel_ = 1.0f / voxelSize_;
    h_ = (voxelSize_.x + voxelSize_.y + voxelSize_.z) / 3.0f;
    count_ = bounds_.voxelCount();

    // Declare the field inventory. density/temperature default-exposed; velocity is internal but can
    // be baked on request (config.bakeFields). AvailableFields()/exposure derive from this.
    fields_.Declare("density", FieldType::Scalar, 0.0f, true);
    fields_.Declare("temperature", FieldType::Scalar, config_.ambientTemperature, true);
    fields_.Declare("velocity", FieldType::Vector3, 0.0f, false);
    fields_.SetExposedByName(config_.bakeFields);

    Reset();
}

void EulerianSmokeSimulation::Reset() {
    time_ = 0.0f;
    vel_.assign(count_, glm::vec3(0.0f));
    velTmp_.assign(count_, glm::vec3(0.0f));
    curl_.assign(count_, glm::vec3(0.0f));
    density_.assign(count_, 0.0f);
    densityTmp_.assign(count_, 0.0f);
    temperature_.assign(count_, config_.ambientTemperature);
    tempTmp_.assign(count_, config_.ambientTemperature);
    pressure_.assign(count_, 0.0f);
    pressureTmp_.assign(count_, 0.0f);
    div_.assign(count_, 0.0f);
    curlMag_.assign(count_, 0.0f);

    // Voxelize static obstacles once (moving obstacles are Phase 4). Sink obstacles are not solid
    // for the flow; they only remove density (handled in AdvectScalars), so only Solid ones fill it.
    solid_.assign(count_, 0.0f);
    for (const VolumeObstacle& ob : config_.obstacles) {
        if (ob.kind != VolumeObstacle::Kind::Solid) continue;
        RasterizeShape(bounds_, ob.Resolve(0.0f), solid_, /*additive=*/false);
    }
}

// --------------------------------------------------------------------------------------------------
// Sampling
// --------------------------------------------------------------------------------------------------
f32 EulerianSmokeSimulation::SampleScalar(const std::vector<f32>& f, glm::vec3 g) const {
    const f32 gx = Clampf(g.x, 0.0f, static_cast<f32>(dim_.x - 1));
    const f32 gy = Clampf(g.y, 0.0f, static_cast<f32>(dim_.y - 1));
    const f32 gz = Clampf(g.z, 0.0f, static_cast<f32>(dim_.z - 1));
    const int x0 = static_cast<int>(std::floor(gx)), x1 = std::min(x0 + 1, dim_.x - 1);
    const int y0 = static_cast<int>(std::floor(gy)), y1 = std::min(y0 + 1, dim_.y - 1);
    const int z0 = static_cast<int>(std::floor(gz)), z1 = std::min(z0 + 1, dim_.z - 1);
    const f32 fx = gx - x0, fy = gy - y0, fz = gz - z0;
    const f32 c000 = f[Idx(x0, y0, z0)], c100 = f[Idx(x1, y0, z0)];
    const f32 c010 = f[Idx(x0, y1, z0)], c110 = f[Idx(x1, y1, z0)];
    const f32 c001 = f[Idx(x0, y0, z1)], c101 = f[Idx(x1, y0, z1)];
    const f32 c011 = f[Idx(x0, y1, z1)], c111 = f[Idx(x1, y1, z1)];
    const f32 x00 = c000 + (c100 - c000) * fx, x10 = c010 + (c110 - c010) * fx;
    const f32 x01 = c001 + (c101 - c001) * fx, x11 = c011 + (c111 - c011) * fx;
    const f32 y0v = x00 + (x10 - x00) * fy, y1v = x01 + (x11 - x01) * fy;
    return y0v + (y1v - y0v) * fz;
}

glm::vec3 EulerianSmokeSimulation::SampleVel(const std::vector<glm::vec3>& f, glm::vec3 g) const {
    const f32 gx = Clampf(g.x, 0.0f, static_cast<f32>(dim_.x - 1));
    const f32 gy = Clampf(g.y, 0.0f, static_cast<f32>(dim_.y - 1));
    const f32 gz = Clampf(g.z, 0.0f, static_cast<f32>(dim_.z - 1));
    const int x0 = static_cast<int>(std::floor(gx)), x1 = std::min(x0 + 1, dim_.x - 1);
    const int y0 = static_cast<int>(std::floor(gy)), y1 = std::min(y0 + 1, dim_.y - 1);
    const int z0 = static_cast<int>(std::floor(gz)), z1 = std::min(z0 + 1, dim_.z - 1);
    const f32 fx = gx - x0, fy = gy - y0, fz = gz - z0;
    const glm::vec3 c000 = f[Idx(x0, y0, z0)], c100 = f[Idx(x1, y0, z0)];
    const glm::vec3 c010 = f[Idx(x0, y1, z0)], c110 = f[Idx(x1, y1, z0)];
    const glm::vec3 c001 = f[Idx(x0, y0, z1)], c101 = f[Idx(x1, y0, z1)];
    const glm::vec3 c011 = f[Idx(x0, y1, z1)], c111 = f[Idx(x1, y1, z1)];
    const glm::vec3 x00 = c000 + (c100 - c000) * fx, x10 = c010 + (c110 - c010) * fx;
    const glm::vec3 x01 = c001 + (c101 - c001) * fx, x11 = c011 + (c111 - c011) * fx;
    const glm::vec3 y0v = x00 + (x10 - x00) * fy, y1v = x01 + (x11 - x01) * fy;
    return y0v + (y1v - y0v) * fz;
}

// --------------------------------------------------------------------------------------------------
// Passes
// --------------------------------------------------------------------------------------------------
void EulerianSmokeSimulation::EmitSources(f32 dt) {
    for (const VolumeEmitter& em : config_.emitters) {
        const VolumeEmitter::Resolved r = em.Resolve(time_);
        if (!r.active || r.strength <= 0.0f) continue;
        jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                int x, y, z; Decode(i, x, y, z);
                if (solid_[i] > 0.5f) continue;
                const glm::vec3 wp = bounds_.voxelCenter(x, y, z);
                const f32 cov = ShapeCoverage(r.shape, wp);
                if (cov <= 0.0f) continue;
                density_[i] += r.densityRate * cov * dt;
                const f32 k = Clampf(r.temperatureRate * cov * dt, 0.0f, 1.0f);
                temperature_[i] += (r.temperatureTarget - temperature_[i]) * k;
                vel_[i] += r.velocity * (cov * dt);
            }
        });
    }
}

void EulerianSmokeSimulation::AdvectVelocity(f32 dt) {
    jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            int x, y, z; Decode(i, x, y, z);
            const glm::vec3 g(x, y, z);
            const glm::vec3 v = vel_[i];
            const glm::vec3 gdep = g - (v * dt) * invVoxel_; // backtrace in grid space
            velTmp_[i] = SampleVel(vel_, gdep);
        }
    });
    vel_.swap(velTmp_);
}

void EulerianSmokeSimulation::ApplyBuoyancy(f32 dt) {
    const glm::vec3 up = config_.up();
    const f32 beta = config_.buoyancyBeta, alpha = config_.buoyancyAlpha;
    const f32 tAmb = config_.ambientTemperature;
    jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            const f32 force = beta * (temperature_[i] - tAmb) - alpha * density_[i];
            vel_[i] += up * (force * dt);
        }
    });
}

void EulerianSmokeSimulation::ComputeVorticity() {
    jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            int x, y, z; Decode(i, x, y, z);
            auto V = [&](int xx, int yy, int zz) -> glm::vec3 {
                return vel_[Idx(Clampi(xx, 0, dim_.x - 1), Clampi(yy, 0, dim_.y - 1),
                                Clampi(zz, 0, dim_.z - 1))];
            };
            glm::vec3 c;
            c.x = (V(x, y + 1, z).z - V(x, y - 1, z).z) * 0.5f * invVoxel_.y -
                  (V(x, y, z + 1).y - V(x, y, z - 1).y) * 0.5f * invVoxel_.z;
            c.y = (V(x, y, z + 1).x - V(x, y, z - 1).x) * 0.5f * invVoxel_.z -
                  (V(x + 1, y, z).z - V(x - 1, y, z).z) * 0.5f * invVoxel_.x;
            c.z = (V(x + 1, y, z).y - V(x - 1, y, z).y) * 0.5f * invVoxel_.x -
                  (V(x, y + 1, z).x - V(x, y - 1, z).x) * 0.5f * invVoxel_.y;
            curl_[i] = c;
            curlMag_[i] = glm::length(c);
        }
    });
}

void EulerianSmokeSimulation::ApplyConfinement(f32 dt) {
    const f32 eps = config_.vorticityStrength;
    if (eps <= 0.0f) return;
    jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            int x, y, z; Decode(i, x, y, z);
            auto M = [&](int xx, int yy, int zz) -> f32 {
                return curlMag_[Idx(Clampi(xx, 0, dim_.x - 1), Clampi(yy, 0, dim_.y - 1),
                                    Clampi(zz, 0, dim_.z - 1))];
            };
            glm::vec3 grad((M(x + 1, y, z) - M(x - 1, y, z)) * 0.5f * invVoxel_.x,
                           (M(x, y + 1, z) - M(x, y - 1, z)) * 0.5f * invVoxel_.y,
                           (M(x, y, z + 1) - M(x, y, z - 1)) * 0.5f * invVoxel_.z);
            const f32 len = glm::length(grad);
            if (len < 1e-5f) continue;
            const glm::vec3 N = grad / len;
            const glm::vec3 force = eps * h_ * glm::cross(N, curl_[i]);
            vel_[i] += force * dt;
        }
    });
}

void EulerianSmokeSimulation::Project() {
    // 1) Divergence of the (post-force) velocity. Solid/out-of-domain neighbour velocity = 0.
    jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            int x, y, z; Decode(i, x, y, z);
            if (solid_[i] > 0.5f) { div_[i] = 0.0f; continue; }
            const f32 vxp = Solid(x + 1, y, z) ? 0.0f : vel_[Idx(x + 1, y, z)].x;
            const f32 vxm = Solid(x - 1, y, z) ? 0.0f : vel_[Idx(x - 1, y, z)].x;
            const f32 vyp = Solid(x, y + 1, z) ? 0.0f : vel_[Idx(x, y + 1, z)].y;
            const f32 vym = Solid(x, y - 1, z) ? 0.0f : vel_[Idx(x, y - 1, z)].y;
            const f32 vzp = Solid(x, y, z + 1) ? 0.0f : vel_[Idx(x, y, z + 1)].z;
            const f32 vzm = Solid(x, y, z - 1) ? 0.0f : vel_[Idx(x, y, z - 1)].z;
            div_[i] = 0.5f * ((vxp - vxm) * invVoxel_.x + (vyp - vym) * invVoxel_.y +
                              (vzp - vzm) * invVoxel_.z);
        }
    });

    // 2) Solve the Poisson equation (Laplacian p = div) with Jacobi iterations, Neumann at solids.
    //    Anisotropic per-axis coefficients c_a = 1/h_a^2. Ping-pong pressure_ <-> pressureTmp_.
    const f32 cx = invVoxel_.x * invVoxel_.x;
    const f32 cy = invVoxel_.y * invVoxel_.y;
    const f32 cz = invVoxel_.z * invVoxel_.z;
    std::fill(pressure_.begin(), pressure_.end(), 0.0f); // fresh solve each step (simple + stable)
    const int iters = std::max(config_.pressureIterations, 1);
    for (int it = 0; it < iters; ++it) {
        const std::vector<f32>& pOld = pressure_;
        std::vector<f32>&       pNew = pressureTmp_;
        jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                int x, y, z; Decode(i, x, y, z);
                if (solid_[i] > 0.5f) { pNew[i] = 0.0f; continue; }
                f32 num = -div_[i], den = 0.0f;
                if (!Solid(x + 1, y, z)) { num += cx * pOld[Idx(x + 1, y, z)]; den += cx; }
                if (!Solid(x - 1, y, z)) { num += cx * pOld[Idx(x - 1, y, z)]; den += cx; }
                if (!Solid(x, y + 1, z)) { num += cy * pOld[Idx(x, y + 1, z)]; den += cy; }
                if (!Solid(x, y - 1, z)) { num += cy * pOld[Idx(x, y - 1, z)]; den += cy; }
                if (!Solid(x, y, z + 1)) { num += cz * pOld[Idx(x, y, z + 1)]; den += cz; }
                if (!Solid(x, y, z - 1)) { num += cz * pOld[Idx(x, y, z - 1)]; den += cz; }
                pNew[i] = den > 1e-8f ? num / den : 0.0f;
            }
        });
        pressure_.swap(pressureTmp_);
    }

    // 3) Subtract the pressure gradient to make velocity divergence-free. Neumann (mirror) at solids.
    jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            int x, y, z; Decode(i, x, y, z);
            if (solid_[i] > 0.5f) { vel_[i] = glm::vec3(0.0f); continue; }
            const f32 pc = pressure_[i];
            const f32 pxp = Solid(x + 1, y, z) ? pc : pressure_[Idx(x + 1, y, z)];
            const f32 pxm = Solid(x - 1, y, z) ? pc : pressure_[Idx(x - 1, y, z)];
            const f32 pyp = Solid(x, y + 1, z) ? pc : pressure_[Idx(x, y + 1, z)];
            const f32 pym = Solid(x, y - 1, z) ? pc : pressure_[Idx(x, y - 1, z)];
            const f32 pzp = Solid(x, y, z + 1) ? pc : pressure_[Idx(x, y, z + 1)];
            const f32 pzm = Solid(x, y, z - 1) ? pc : pressure_[Idx(x, y, z - 1)];
            vel_[i].x -= 0.5f * (pxp - pxm) * invVoxel_.x;
            vel_[i].y -= 0.5f * (pyp - pym) * invVoxel_.y;
            vel_[i].z -= 0.5f * (pzp - pzm) * invVoxel_.z;
        }
    });
}

void EulerianSmokeSimulation::AdvectScalars(f32 dt) {
    const f32 densFade = std::exp(-config_.densityDissipation * dt);
    const f32 tempFade = std::exp(-config_.temperatureCooling * dt);
    const f32 tAmb = config_.ambientTemperature;
    jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            int x, y, z; Decode(i, x, y, z);
            const glm::vec3 g(x, y, z);
            const glm::vec3 gdep = g - (vel_[i] * dt) * invVoxel_;
            const f32 d = SampleScalar(density_, gdep) * densFade;
            const f32 t = tAmb + (SampleScalar(temperature_, gdep) - tAmb) * tempFade;
            densityTmp_[i] = d;
            tempTmp_[i] = t;
        }
    });
    density_.swap(densityTmp_);
    temperature_.swap(tempTmp_);
}

void EulerianSmokeSimulation::EnforceSolids() {
    // Sink obstacles remove density; solid cells hold nothing. (Static solids already zero velocity
    // in Project's gradient-subtract; this cleans density/temperature.)
    for (const VolumeObstacle& ob : config_.obstacles) {
        if (ob.kind != VolumeObstacle::Kind::Sink) continue;
        const VolumeShape shp = ob.Resolve(time_);
        jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                int x, y, z; Decode(i, x, y, z);
                const f32 cov = ShapeCoverage(shp, bounds_.voxelCenter(x, y, z));
                if (cov > 0.0f) density_[i] *= (1.0f - cov);
            }
        });
    }
    if (!config_.obstacles.empty()) {
        jobs::ParallelFor(static_cast<u32>(count_), kJobGroup, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                if (solid_[i] > 0.5f) { density_[i] = 0.0f; temperature_[i] = config_.ambientTemperature; }
            }
        });
    }
}

void EulerianSmokeSimulation::Step(f32 dt) {
    if (count_ == 0 || dt <= 0.0f) { time_ += std::max(dt, 0.0f); return; }
    EmitSources(dt);       // grid source terms (density/temperature/inflow)
    AdvectVelocity(dt);    // u <- advect(u, u)
    ApplyBuoyancy(dt);     // hot rises, smoke weighs down
    ComputeVorticity();    // curl + |curl|
    ApplyConfinement(dt);  // re-inject the swirl semi-Lagrangian smeared away
    Project();             // make u divergence-free (the "it's a fluid" step)
    AdvectScalars(dt);     // move density/temperature by the projected u (+ dissipate/cool)
    EnforceSolids();       // sinks + solid-cell cleanup
    time_ += dt;
}

// --------------------------------------------------------------------------------------------------
// Readback + snapshot
// --------------------------------------------------------------------------------------------------
void EulerianSmokeSimulation::ReadbackFrame(VolumeFrame& out) {
    out.time = time_;
    out.bounds = bounds_;
    // Ensure ALL exposed fields first, then fill via pointers (ensureField may reallocate out.fields).
    for (const VolumeFieldDecl& d : fields_.Decls())
        if (d.exposed) out.ensureField(d.name, d.type, d.background);

    for (const VolumeFieldDecl& d : fields_.Decls()) {
        if (!d.exposed) continue;
        VolumeField* f = out.field(d.name);
        if (!f) continue;
        if (d.name == "density") {
            f->data = density_;
        } else if (d.name == "temperature") {
            f->data = temperature_;
        } else if (d.name == "velocity") {
            f->data.resize(count_ * 3);
            for (usize i = 0; i < count_; ++i) {
                f->data[i * 3 + 0] = vel_[i].x;
                f->data[i * 3 + 1] = vel_[i].y;
                f->data[i * 3 + 2] = vel_[i].z;
            }
        }
    }
}

bool EulerianSmokeSimulation::SaveState(std::vector<u8>& out) const {
    // [time_][count_][vel][density][temperature]  (pressure/curl/div are transient, not needed).
    const usize velBytes = count_ * sizeof(glm::vec3);
    const usize sclBytes = count_ * sizeof(f32);
    out.resize(sizeof(f32) + sizeof(u64) + velBytes + 2 * sclBytes);
    usize o = 0;
    std::memcpy(out.data() + o, &time_, sizeof(f32)); o += sizeof(f32);
    const u64 c = count_;
    std::memcpy(out.data() + o, &c, sizeof(u64)); o += sizeof(u64);
    std::memcpy(out.data() + o, vel_.data(), velBytes); o += velBytes;
    std::memcpy(out.data() + o, density_.data(), sclBytes); o += sclBytes;
    std::memcpy(out.data() + o, temperature_.data(), sclBytes); o += sclBytes;
    return true;
}

bool EulerianSmokeSimulation::LoadState(const std::vector<u8>& in) {
    const usize velBytes = count_ * sizeof(glm::vec3);
    const usize sclBytes = count_ * sizeof(f32);
    const usize need = sizeof(f32) + sizeof(u64) + velBytes + 2 * sclBytes;
    if (in.size() != need) return false;
    usize o = 0;
    std::memcpy(&time_, in.data() + o, sizeof(f32)); o += sizeof(f32);
    u64 c = 0;
    std::memcpy(&c, in.data() + o, sizeof(u64)); o += sizeof(u64);
    if (c != count_) return false;
    std::memcpy(vel_.data(), in.data() + o, velBytes); o += velBytes;
    std::memcpy(density_.data(), in.data() + o, sclBytes); o += sclBytes;
    std::memcpy(temperature_.data(), in.data() + o, sclBytes); o += sclBytes;
    return true;
}

// --------------------------------------------------------------------------------------------------
// --test-eulersim self-test
// --------------------------------------------------------------------------------------------------
bool SelfTestEulerianSmoke(std::string& report) {
    VolumeSimConfig cfg;
    cfg.model = "eulerian-smoke";
    cfg.bounds.worldMin = glm::vec3(-1.0f, 0.0f, -1.0f);
    cfg.bounds.worldMax = glm::vec3(1.0f, 4.0f, 1.0f);
    cfg.bounds.dim = glm::ivec3(24, 48, 24);
    cfg.frameRate = 30.0f;
    cfg.substeps = 2;
    cfg.pressureIterations = 20;
    {
        VolumeEmitter em;
        em.name = "base";
        em.shape.kind = VolumeShapeKind::Sphere;
        em.shape.center = glm::vec3(0.0f, 0.35f, 0.0f);
        em.shape.halfExtents = glm::vec3(0.4f);
        em.densityRate = 4.0f;
        em.temperatureRate = 6.0f;
        em.temperatureTarget = 1.0f;
        cfg.emitters.push_back(em);
    }

    auto centreOfMassY = [](const std::vector<f32>& d, const VolumeBounds& b) -> f64 {
        f64 wsum = 0.0, ysum = 0.0;
        for (int z = 0; z < b.dim.z; ++z)
            for (int y = 0; y < b.dim.y; ++y)
                for (int x = 0; x < b.dim.x; ++x) {
                    const f64 w = d[VoxelIndex(b, x, y, z)];
                    if (w <= 0.0) continue;
                    wsum += w;
                    ysum += w * static_cast<f64>(b.voxelCenter(x, y, z).y);
                }
        return wsum > 0.0 ? ysum / wsum : 0.0;
    };

    const u32 kFrames = 24;
    EulerianSmokeSimulation sim(cfg);
    sim.Reset();
    const f32 dt = 1.0f / (cfg.frameRate * cfg.substeps);
    const int substeps = cfg.substeps;

    VolumeFrame fr;
    f64 comEarly = 0.0, comLate = 0.0, finalTotal = 0.0;
    for (u32 f = 0; f < kFrames; ++f) {
        for (int s = 0; s < substeps; ++s) sim.Step(dt);
        sim.ReadbackFrame(fr);
        const VolumeField* d = fr.field("density");
        if (!d) { report = "no density field produced"; return false; }
        for (f32 v : d->data) {
            if (!std::isfinite(v)) { report = "non-finite density at frame " + std::to_string(f); return false; }
        }
        if (f == 6)  comEarly = centreOfMassY(d->data, fr.bounds);
        if (f == kFrames - 1) {
            comLate = centreOfMassY(d->data, fr.bounds);
            for (f32 v : d->data) finalTotal += v;
        }
    }

    if (finalTotal <= 0.0) { report = "solver produced no density"; return false; }
    if (!(comLate > comEarly + 1e-3)) {
        report = "plume did not rise (comEarly=" + std::to_string(comEarly) +
                 " comLate=" + std::to_string(comLate) + ")";
        return false;
    }

    // Determinism: an identical run must reproduce the final density total bit-for-bit.
    EulerianSmokeSimulation sim2(cfg);
    sim2.Reset();
    VolumeFrame fr2;
    f64 finalTotal2 = 0.0;
    for (u32 f = 0; f < kFrames; ++f) {
        for (int s = 0; s < substeps; ++s) sim2.Step(dt);
    }
    sim2.ReadbackFrame(fr2);
    for (f32 v : fr2.field("density")->data) finalTotal2 += v;
    if (finalTotal != finalTotal2) {
        report = "non-deterministic (total " + std::to_string(finalTotal) + " vs " +
                 std::to_string(finalTotal2) + ")";
        return false;
    }

    report = "rises " + std::to_string(comEarly) + "->" + std::to_string(comLate) +
             ", density " + std::to_string(finalTotal) + ", deterministic";
    return true;
}

} // namespace hbe::volume
