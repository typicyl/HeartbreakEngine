// RHI/RHIFactory.cpp
#include "RHI/RHIFactory.h"
#include "Core/Log.h"

#if HBE_ENABLE_D3D12
#  include "RHI/D3D12/D3D12Device.h"
#endif
#if HBE_ENABLE_VULKAN
#  include "RHI/Vulkan/VulkanDevice.h"
#endif
#if HBE_ENABLE_OPENGL
#  include "RHI/GL/GLDevice.h"
#endif

namespace hbe::rhi {

const char* ToString(GraphicsAPI api) {
    switch (api) {
        case GraphicsAPI::D3D12:  return "Direct3D 12";
        case GraphicsAPI::Vulkan: return "Vulkan";
        case GraphicsAPI::OpenGL: return "OpenGL";
    }
    return "Unknown";
}

namespace {
ShaderProvider g_shaderProvider;
}

void SetShaderProvider(ShaderProvider provider) {
    g_shaderProvider = std::move(provider);
}

bool LoadShaderBytecode(const std::string& leaf, std::vector<u8>& out) {
    return g_shaderProvider && g_shaderProvider(leaf, out);
}

bool IsBackendCompiled(GraphicsAPI api) {
    switch (api) {
        case GraphicsAPI::D3D12:  return HBE_ENABLE_D3D12 != 0;
        case GraphicsAPI::Vulkan: return HBE_ENABLE_VULKAN != 0;
        case GraphicsAPI::OpenGL: return HBE_ENABLE_OPENGL != 0;
    }
    return false;
}

std::unique_ptr<IRenderDevice> CreateRenderDevice(const RenderDeviceDesc& desc) {
    if (!IsBackendCompiled(desc.api)) {
        HBE_ERROR("Backend '{}' is not compiled into this build.", ToString(desc.api));
        return nullptr;
    }

    switch (desc.api) {
#if HBE_ENABLE_D3D12
        case GraphicsAPI::D3D12:  return CreateD3D12Device(desc);
#endif
#if HBE_ENABLE_VULKAN
        case GraphicsAPI::Vulkan: return CreateVulkanDevice(desc);
#endif
#if HBE_ENABLE_OPENGL
        case GraphicsAPI::OpenGL: return CreateOpenGLDevice(desc);
#endif
        default: break;
    }

    HBE_ERROR("No factory available for backend '{}'.", ToString(desc.api));
    return nullptr;
}

} // namespace hbe::rhi
