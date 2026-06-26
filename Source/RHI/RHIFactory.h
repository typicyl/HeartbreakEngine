// RHI/RHIFactory.h - constructs a concrete RHI device for the requested backend.
#pragma once

#include "RHI/RHI.h"

#include <memory>

namespace hbe::rhi {

// Returns true if the engine was compiled with support for the given backend.
bool IsBackendCompiled(GraphicsAPI api);

// Creates and initializes a render device for `desc.api`. Returns nullptr on
// failure (unsupported backend, device creation error). Errors are logged.
std::unique_ptr<IRenderDevice> CreateRenderDevice(const RenderDeviceDesc& desc);

} // namespace hbe::rhi
