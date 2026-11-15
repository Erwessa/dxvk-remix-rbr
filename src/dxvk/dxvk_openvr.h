#pragma once

#include <array>
#include <mutex>
#include <vector>

#include <openvr/openvr.hpp>
#include "dxvk_extension_provider.h"

namespace dxvk {

  class DxvkInstance;

  /**
   * \brief OpenVR instance
   * 
   * Loads Initializes OpenVR to provide
   * access to Vulkan extension queries.
   */
  class VrInstance : public DxvkExtensionProvider {
    
  public:
    
    VrInstance();
    ~VrInstance();

    std::string_view getName();

    DxvkNameSet getInstanceExtensions();

    DxvkNameSet getDeviceExtensions(
            uint32_t      adapterId);
    
    void initInstanceExtensions();

    void initDeviceExtensions(
      const DxvkInstance* instance);

    // Starts a long-lived OpenVR scene session after Vulkan device creation.
    // If the host already owns a scene session, it is reused and never shut down here.
    bool initializeSceneSession();
    void shutdownSceneSession();

    vr::IVRSystem* getSystem() const { return m_system; }
    vr::IVRCompositor* getActiveCompositor() const { return m_sceneSessionActive ? m_compositor : nullptr; }
    bool isSceneSessionActive() const { return m_sceneSessionActive; }

    // OpenVR frame primitives. Call waitGetPoses once per rendered HMD frame on
    // the render thread; eye transforms and raw projection bounds are queried
    // from the same active scene session.
    bool waitGetPoses(std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount>& renderPoses);
    bool getRecommendedRenderTargetSize(uint32_t& width, uint32_t& height);
    bool getEyeToHeadTransform(vr::EVREye eye, vr::HmdMatrix34_t& transform);
    bool getProjectionRaw(vr::EVREye eye, float& left, float& right, float& top, float& bottom);

    static VrInstance s_instance;

  private:

    dxvk::mutex           m_mutex;
    HKEY                  m_vr_key     = nullptr;
    vr::IVRCompositor*    m_compositor = nullptr;
    vr::IVRSystem*        m_system = nullptr;
    HMODULE               m_ovrApi     = nullptr;

    bool m_no_vr;
    bool m_loadedOvrApi      = false;
    bool m_initializedOpenVr = false;
    bool m_sceneSessionActive = false;
    bool m_initializedInsExt = false;
    bool m_initializedDevExt = false;

    DxvkNameSet              m_insExtensions;
    std::vector<DxvkNameSet> m_devExtensions;
    
    DxvkNameSet queryInstanceExtensions() const;

    DxvkNameSet queryDeviceExtensions(
            Rc<DxvkAdapter>           adapter) const;

    DxvkNameSet parseExtensionList(
      const std::string&              str) const;
    
    vr::IVRCompositor* getCompositor();

    void shutdown();

    HMODULE loadLibrary();

    void freeLibrary();

    void* getSym(const char* sym);

    bool waitVrKeyReady() const;
  };

  extern VrInstance g_vrInstance;
  
}
