// RHI/Vulkan/VulkanDevice.h - factory entry point for the Vulkan backend.
//
// The concrete device type is defined privately in VulkanDevice.cpp so this
// header (included by the cross-backend factory) stays free of <vulkan.h>.
#pragma once

#include "RHI/RHI.h"

#include <memory>

namespace hbe::rhi {

std::unique_ptr<IRenderDevice> CreateVulkanDevice(const RenderDeviceDesc& desc);

} // namespace hbe::rhi
