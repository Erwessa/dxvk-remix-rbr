#include "dxvk_device.h"
#include "dxvk_fence.h"

namespace dxvk {

  DxvkFence::DxvkFence(DxvkDevice* device, const DxvkFenceCreateInfo& info)
  : m_device(device), m_vkd(device->vkd()) {
    VkSemaphoreTypeCreateInfo typeInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = info.initialValue;

    VkSemaphoreCreateInfo createInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &typeInfo };
#ifdef _WIN32
    VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    if (info.sharedType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_FLAG_BITS_MAX_ENUM) {
      if (!device->extensions().khrExternalSemaphore || !device->extensions().khrExternalSemaphoreWin32)
        throw DxvkError("External Win32 semaphore extensions are not enabled");

      VkPhysicalDeviceExternalSemaphoreInfo externalInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &typeInfo };
      externalInfo.handleType = info.sharedType;
      VkExternalSemaphoreProperties properties = { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
      device->adapter()->vki()->vkGetPhysicalDeviceExternalSemaphoreProperties(
        device->adapter()->handle(), &externalInfo, &properties);

      if (!(properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
        throw DxvkError("External Win32 semaphore import is not supported");

      if (properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) {
        exportInfo.handleTypes = info.sharedType;
        typeInfo.pNext = &exportInfo;
      }
    }
#endif

    VkResult vr = m_vkd->vkCreateSemaphore(m_vkd->device(), &createInfo, nullptr, &m_semaphore);
    if (vr != VK_SUCCESS)
      throw DxvkError(str::format("Failed to create timeline semaphore: ", vr));

#ifdef _WIN32
    if (info.sharedHandle != INVALID_HANDLE_VALUE && info.sharedHandle != nullptr) {
      VkImportSemaphoreWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
      importInfo.semaphore = m_semaphore;
      importInfo.handleType = info.sharedType;
      importInfo.handle = info.sharedHandle;
      vr = m_vkd->vkImportSemaphoreWin32HandleKHR(m_vkd->device(), &importInfo);
      if (vr != VK_SUCCESS) {
        m_vkd->vkDestroySemaphore(m_vkd->device(), m_semaphore, nullptr);
        m_semaphore = VK_NULL_HANDLE;
        throw DxvkError(str::format("Failed to import Win32 timeline semaphore: ", vr));
      }
    }
#endif
  }


  DxvkFence::~DxvkFence() {
    if (m_semaphore != VK_NULL_HANDLE)
      m_vkd->vkDestroySemaphore(m_vkd->device(), m_semaphore, nullptr);
  }


  uint64_t DxvkFence::getValue() const {
    uint64_t value = 0;
    VkResult vr = m_vkd->vkGetSemaphoreCounterValue(m_vkd->device(), m_semaphore, &value);
    if (vr != VK_SUCCESS)
      Logger::err(str::format("Failed to query timeline semaphore: ", vr));
    return value;
  }


  VkResult DxvkFence::signal(uint64_t value) {
    VkTimelineSemaphoreSubmitInfo timelineInfo = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &value;

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timelineInfo };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &m_semaphore;

    // The caller holds DxvkDevice::lockSubmission(), so this submission is
    // serialized with DXVK and ordered after previously queued GPU work.
    return m_vkd->vkQueueSubmit(m_device->queues().graphics.queueHandle, 1, &submitInfo, VK_NULL_HANDLE);
  }

}
