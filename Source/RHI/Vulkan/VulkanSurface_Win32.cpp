// RHI/Vulkan/VulkanSurface_Win32.cpp - the Windows backend of VulkanSurface.h.
//
// This is the ONLY Vulkan translation unit that defines VK_USE_PLATFORM_WIN32_KHR and includes
// <windows.h>. A second platform adds VulkanSurface_Linux.cpp (VK_USE_PLATFORM_XLIB_KHR or
// _WAYLAND_KHR) beside it, and the CMake selection picks one - exactly as the RHI already picks
// D3D12/Vulkan/OpenGL. Because the platform macro is defined HERE and nowhere else, the rest of
// the engine's Vulkan code sees only core vulkan.h.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#  define VK_USE_PLATFORM_WIN32_KHR // pulls vulkan_win32.h (VkWin32SurfaceCreateInfoKHR, etc.)
#endif
#include <vulkan/vulkan.h>

#include "RHI/Vulkan/VulkanSurface.h"

namespace hbe::rhi::vk_surface {

std::vector<const char*> RequiredInstanceExtensions() {
    // The exact pair the instance-extension list used to hardcode inline in VulkanDevice.cpp.
    return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
}

VkResult CreateWindowSurface(VkInstance instance, void* nativeWindowHandle,
                             void* nativeWindowInstance, VkSurfaceKHR* outSurface,
                             const VkAllocationCallbacks* allocator) {
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    // An HINSTANCE was not always supplied (the ImGui multi-viewport path has only the HWND);
    // fall back to the running module, exactly as the two inline call sites did before.
    sci.hinstance = nativeWindowInstance ? static_cast<HINSTANCE>(nativeWindowInstance)
                                         : ::GetModuleHandleW(nullptr);
    sci.hwnd = static_cast<HWND>(nativeWindowHandle);
    return vkCreateWin32SurfaceKHR(instance, &sci, allocator, outSurface);
}

} // namespace hbe::rhi::vk_surface
