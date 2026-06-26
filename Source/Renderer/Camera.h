// Renderer/Camera.h - a simple perspective fly-camera (header-only, GLM-based).
//
// Produces the view/projection matrices the PBR pass consumes. Projection uses
// a [0,1] depth range (GLM_FORCE_DEPTH_ZERO_TO_ONE) to match D3D12/Vulkan.
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace hbe {

class Camera {
public:
    void SetPerspective(float fovYDegrees, float aspect, float zNear, float zFar) {
        fovY_   = glm::radians(fovYDegrees);
        aspect_ = aspect;
        zNear_  = zNear;
        zFar_   = zFar;
    }

    void SetAspect(float aspect) { aspect_ = aspect; }
    void SetClipPlanes(float zNear, float zFar) { zNear_ = zNear; zFar_ = zFar; }
    void SetFovY(float fovYDegrees) { fovY_ = glm::radians(fovYDegrees); }
    float NearPlane() const { return zNear_; }
    float FarPlane() const { return zFar_; }
    float Aspect() const { return aspect_; }
    float FovY() const { return fovY_; } // radians

    void LookAt(glm::vec3 eye, glm::vec3 target, glm::vec3 up = {0, 1, 0}) {
        position_ = eye;
        target_   = target;
        up_       = up;
    }

    glm::mat4 View() const { return glm::lookAtRH(position_, target_, up_); }

    glm::mat4 Projection() const {
        // RH, zero-to-one depth (configured globally via GLM_FORCE_DEPTH_ZERO_TO_ONE).
        return glm::perspectiveRH_ZO(fovY_, aspect_, zNear_, zFar_);
    }

    glm::mat4 ViewProjection() const { return Projection() * View(); }

    glm::vec3 Position() const { return position_; }
    glm::vec3 Forward() const { return glm::normalize(target_ - position_); }

private:
    glm::vec3 position_{0, 1, 3};
    glm::vec3 target_{0, 0, 0};
    glm::vec3 up_{0, 1, 0};
    float fovY_   = glm::radians(60.0f);
    float aspect_ = 16.0f / 9.0f;
    float zNear_  = 0.1f;
    float zFar_   = 1000.0f;
};

} // namespace hbe
