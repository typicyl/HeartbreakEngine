// RHI/GL/GLDevice.h - OpenGL 4.6 RHI backend factory.
#pragma once

#include "RHI/RHI.h"

#include <memory>

namespace hbe::rhi {

// Creates the OpenGL backend (GL 4.6 core). Returns nullptr if the context fails.
std::unique_ptr<IRenderDevice> CreateOpenGLDevice(const RenderDeviceDesc& desc);

} // namespace hbe::rhi
