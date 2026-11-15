#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include <openvr/openvr.hpp>
#include "dxvk_extension_provider.h"

namespace dxvk {

  struct OpenVrProjectionBounds {
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
  };

  // One compositor-synchronized HMD frame. Eye transforms are eye-to-head;
  // invert them when composing a world-to-eye view matrix.
  struct OpenVrFrameState {
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> renderPoses = {};
    std::array<vr::HmdMatrix34_t, 2> eyeToHead = {};
    std::array<OpenVrProjectionBounds, 2> projection = {};
    uint64_t frameIndex = 0;
    bool hmdPoseValid = false;
  };

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

    // Acquires poses and both eyes' optical parameters as one synchronized
    // frame snapshot. The caller must hold DxvkDevice's submission queue gate
    // around this call because WaitGetPoses may access the Vulkan queue.
    bool acquireFrame(OpenVrFrameState& frame);

    // Lower-level accessors retained for diagnostics and staged integration.
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
    uint64_t m_frameIndex = 0;
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
