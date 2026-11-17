#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>
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

  // Row-major affine transform using OpenVR's column-vector composition convention.
  // This deliberately does not apply RBR's world origin, scale, or renderer-specific handedness.
  struct OpenVrMatrix4 {
    std::array<float, 16> elements = {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
    };

    float& at(std::size_t row, std::size_t column) { return elements[row * 4 + column]; }
    float at(std::size_t row, std::size_t column) const { return elements[row * 4 + column]; }

    static OpenVrMatrix4 fromHmdMatrix34(const vr::HmdMatrix34_t& source) {
      OpenVrMatrix4 result;
      for (std::size_t row = 0; row < 3; row++)
        for (std::size_t column = 0; column < 4; column++)
          result.at(row, column) = source.m[row][column];
      return result;
    }

    // Inverse of a rigid transform [R|t]: [R^T|-R^T t].
    OpenVrMatrix4 inverseRigid() const {
      OpenVrMatrix4 result;
      for (std::size_t row = 0; row < 3; row++)
        for (std::size_t column = 0; column < 3; column++)
          result.at(row, column) = at(column, row);
      for (std::size_t row = 0; row < 3; row++) {
        result.at(row, 3) = 0.0f;
        for (std::size_t column = 0; column < 3; column++)
          result.at(row, 3) -= result.at(row, column) * at(column, 3);
      }
      return result;
    }

    OpenVrMatrix4 multipliedBy(const OpenVrMatrix4& rhs) const {
      OpenVrMatrix4 result;
      result.elements.fill(0.0f);
      for (std::size_t row = 0; row < 4; row++)
        for (std::size_t column = 0; column < 4; column++)
          for (std::size_t k = 0; k < 4; k++)
            result.at(row, column) += at(row, k) * rhs.at(k, column);
      return result;
    }
  };

  // Explicit calibration from RBR game-world coordinates into OpenVR tracking
  // coordinates. The affine matrix must include the chosen tracking origin,
  // axis/handedness mapping, and game-units-to-metres scale. It is never inferred.
  struct OpenVrWorldCalibration {
    OpenVrMatrix4 trackingFromGameWorld = {};
    bool valid = false;
  };

  // One compositor-synchronized HMD frame. eyeToHead maps eye space to head
  // space; eyeFromTracking maps OpenVR tracking space to each eye's view space.
  struct OpenVrFrameState {
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> renderPoses = {};
    std::array<vr::HmdMatrix34_t, 2> eyeToHead = {};
    std::array<OpenVrProjectionBounds, 2> projection = {};
    std::array<OpenVrMatrix4, 2> eyeFromTracking = {};
    OpenVrMatrix4 trackingFromHead = {};
    uint32_t recommendedWidth = 0;
    uint32_t recommendedHeight = 0;
    uint64_t frameIndex = 0;
    bool hmdPoseValid = false;

    // Compose a calibrated game-world transform with this frame's eye pose.
    // Result maps game-world positions directly into the requested eye's view
    // space. OpenVrMatrix4 uses column vectors: eyeFromTracking * trackingFromGameWorld.
    // The caller converts the result with matrixFromRowMajor() before passing it
    // to RtCamera.
    bool getEyeFromGameWorld(
      std::size_t eye,
      const OpenVrWorldCalibration& calibration,
      OpenVrMatrix4& eyeFromGameWorld) const {
      if (!hmdPoseValid || !calibration.valid || eye >= eyeFromTracking.size())
        return false;

      eyeFromGameWorld = eyeFromTracking[eye].multipliedBy(calibration.trackingFromGameWorld);
      return true;
    }
  };

  class DxvkInstance;
  class DxvkAdapter;

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

    // Disable optional OpenVR integration if the runtime requests instance extensions
    // that the active Vulkan loader does not expose.
    void validateInstanceExtensions(const DxvkNameSet& available);

    void initDeviceExtensions(
      const DxvkInstance* instance);

    // Filter per-adapter OpenVR extensions against that physical device.
    void validateDeviceExtensions(uint32_t adapterId, const DxvkAdapter& adapter);

    // Starts a long-lived OpenVR scene session after Vulkan device creation.
    // If the host already owns a scene session, it is reused and never shut down here.
    bool initializeSceneSession(const void* owner, uint32_t adapterId, bool force = false);
    void shutdownSceneSession(const void* owner);

    vr::IVRSystem* getSystem() const { return m_system; }
    vr::IVRCompositor* getActiveCompositor() const { return m_sceneSessionActive ? m_compositor : nullptr; }
    bool isSceneSessionActive() const { return m_sceneSessionActive; }

    // Acquires poses and both eyes' optical parameters as one synchronized
    // frame snapshot. The caller must hold DxvkDevice's submission queue gate
    // around this call because WaitGetPoses may access the Vulkan queue.
    bool acquireFrame(OpenVrFrameState& frame);

    // Submits a completed left/right Vulkan image pair. The caller must hold
    // DxvkDevice's submission queue gate for the duration of this call.
    bool submitFrame(
      const std::array<vr::VRVulkanTextureData_t, 2>& textures,
      vr::EColorSpace colorSpace);

    bool getFrameTiming(vr::Compositor_FrameTiming& timing);
    bool resetView();

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
    vr::IVRChaperone*     m_chaperone = nullptr;
    HMODULE               m_ovrApi     = nullptr;

    bool m_no_vr;
    bool m_loadedOvrApi      = false;
    bool m_initializedOpenVr = false;
    bool m_sceneSessionActive = false;
    uint32_t m_sceneSessionAdapterId = UINT32_MAX;
    std::unordered_set<const void*> m_sceneSessionOwners;
    uint64_t m_frameIndex = 0;
    uint64_t m_waitGetPosesFailures = 0;
    bool m_initializedInsExt = false;
    bool m_initializedDevExt = false;
    bool m_extensionQueryFailed = false;

    DxvkNameSet              m_insExtensions;
    std::vector<DxvkNameSet> m_devExtensions;
    std::vector<bool> m_devExtensionsValid;
    
    bool queryInstanceExtensions(DxvkNameSet& extensions) const;

    bool queryDeviceExtensions(
            Rc<DxvkAdapter>           adapter,
            DxvkNameSet&              extensions) const;

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
