#pragma once

#include "dxvk_include.h"

namespace dxvk {

  class DxvkDevice;

  /**
   * \\brief Parameters for creating or importing a timeline semaphore.
   */
  struct DxvkFenceCreateInfo {
    uint64_t initialValue = 0;
#ifdef _WIN32
    VkExternalSemaphoreHandleTypeFlagBits sharedType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
    HANDLE sharedHandle = INVALID_HANDLE_VALUE;
#endif
  };

  /**
   * \\brief Vulkan timeline semaphore used for external GPU synchronization.
   */
  class DxvkFence : public RcObject {
  public:
    DxvkFence(DxvkDevice* device, const DxvkFenceCreateInfo& info);
    ~DxvkFence();

    uint64_t getValue() const;
    VkResult signal(uint64_t value);

  private:
    DxvkDevice* m_device;
    Rc<vk::DeviceFn> m_vkd;
    VkSemaphore m_semaphore = VK_NULL_HANDLE;
  };

}
