// RHI/Vulkan/VulkanSurface.h - OS-abstracted Vulkan window-surface creation.
//
// The VkSurfaceKHR is the ONE Vulkan object whose creation is platform-specific: Windows uses
// vkCreateWin32SurfaceKHR, X11 vkCreateXlibSurfaceKHR, Wayland vkCreateWaylandSurfaceKHR,
// macOS/MoltenVK vkCreateMetalSurfaceEXT. Everything else in the Vulkan backend is already
// portable. Keeping that one seam behind this interface is what lets VulkanDevice.cpp - and the
// entire shipped RUNTIME Vulkan device - compile with NO <windows.h> and NO VK_USE_PLATFORM_*
// macro. Only the backend TU (VulkanSurface_Win32.cpp) knows which KHR extension and which
// vkCreate*SurfaceKHR the host OS uses; a port adds VulkanSurface_Linux.cpp beside it and
// changes nothing here. Same discipline the RHI already keeps for D3D12/Vulkan and Core/Platform
// keeps for the OS.
//
// DISCIPLINE: this header includes ONLY core <vulkan/vulkan.h> (which defines VkInstance /
// VkSurfaceKHR / VkResult without any platform macro). It never includes a platform Vulkan
// header (vulkan_win32.h) and never <windows.h> - a surface interface that mentions HWND is a
// Win32 header with extra steps, not an abstraction.
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace hbe::rhi::vk_surface {

// The instance extensions a window surface needs on THIS platform, in enable order: always
// VK_KHR_surface, plus the single OS-specific surface extension (VK_KHR_win32_surface on
// Windows). Feed straight into VkInstanceCreateInfo::ppEnabledExtensionNames - the instance
// MUST enable these before any surface can be created.
std::vector<const char*> RequiredInstanceExtensions();

// Create a VkSurfaceKHR for an OS window. `nativeWindowHandle` is the platform window handle
// (an HWND on Windows) as a void*; `nativeWindowInstance` is the module/connection handle
// (an HINSTANCE on Windows), or nullptr to let the backend resolve it from the running module.
// Returns the VkResult of the underlying vkCreate*SurfaceKHR so the caller can VK_CHECK it.
//
// `allocator` is forwarded to the underlying vkCreate*SurfaceKHR. The main swapchain passes
// nullptr (Vulkan's default host allocator - all this engine uses today); the ImGui
// multi-viewport path forwards the callbacks ImGui hands it, so a surface's create and destroy
// stay symmetric if a custom allocator is ever adopted. Defaults to nullptr.
VkResult CreateWindowSurface(VkInstance instance, void* nativeWindowHandle,
                             void* nativeWindowInstance, VkSurfaceKHR* outSurface,
                             const VkAllocationCallbacks* allocator = nullptr);

} // namespace hbe::rhi::vk_surface
