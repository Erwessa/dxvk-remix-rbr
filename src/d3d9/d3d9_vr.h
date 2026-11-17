#pragma once

#include <d3d9.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#undef VK_USE_PLATFORM_WIN32_KHR

#include <stdint.h>

struct D3D9_TEXTURE_VR_DESC {
  uint64_t Image;
  VkDevice Device;
  VkPhysicalDevice PhysicalDevice;
  VkInstance Instance;
  VkQueue Queue;
  uint32_t QueueFamilyIndex;
  uint32_t Width;
  uint32_t Height;
  VkFormat Format;
  uint32_t SampleCount;
};

struct OXR_VK_DEVICE_DESC {
  VkDevice Device;
  VkPhysicalDevice PhysicalDevice;
  VkInstance Instance;
  uint32_t QueueIndex;
  uint32_t QueueFamilyIndex;
};

#include <d3d9vr.h>

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9* pDevice, IDirect3DVR9** pInterface);
