#include "dxvk_instance.h"
#include "dxvk_openvr.h"
#include "rtx_render/rtx_vr.h"

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#include <openvr/openvr.hpp>

#include <cstring>
#include <fstream>
#include <windows.h>

using VR_InitInternalProc        = uint32_t (VR_CALLTYPE *)(vr::EVRInitError*, vr::EVRApplicationType);
using VR_ShutdownInternalProc    = void  (VR_CALLTYPE *)();
using VR_GetGenericInterfaceProc = void* (VR_CALLTYPE *)(const char*, vr::EVRInitError*);

namespace dxvk {
  
  struct VrFunctions {
    VR_InitInternalProc        initInternal        = nullptr;
    VR_ShutdownInternalProc    shutdownInternal    = nullptr;
    VR_GetGenericInterfaceProc getGenericInterface = nullptr;
  };
  
  VrFunctions g_vrFunctions;

  namespace {

    bool readRegistryString(HKEY key, const char* name, std::string& value) {
      DWORD type = 0;
      DWORD size = 0;
      LSTATUS status = RegQueryValueExA(key, name, nullptr, &type, nullptr, &size);

      if (status != ERROR_SUCCESS || type != REG_SZ || size == 0) {
        Logger::err(str::format("OpenVR: invalid registry string ", name,
          " (status ", status, ", type ", type, ", size ", size, ")"));
        return false;
      }

      std::vector<char> buffer(size);
      DWORD actualType = 0;
      DWORD actualSize = size;
      status = RegQueryValueExA(key, name, nullptr, &actualType,
        reinterpret_cast<BYTE*>(buffer.data()), &actualSize);

      if (status != ERROR_SUCCESS || actualType != REG_SZ
          || actualSize == 0 || actualSize > buffer.size()
          || buffer[actualSize - 1] != '\0'
          || std::memchr(buffer.data(), '\0', actualSize - 1) != nullptr) {
        Logger::err(str::format("OpenVR: malformed registry string ", name,
          " (status ", status, ", type ", actualType, ", size ", actualSize, ")"));
        return false;
      }

      value.assign(buffer.data(), actualSize - 1);
      return true;
    }

    template<typename Query>
    bool queryOpenVrExtensionString(Query query, std::string& value) {
      const uint32_t bufferSize = query(nullptr, 0);
      if (!bufferSize) {
        Logger::err("OpenVR: extension query returned zero buffer size");
        return false;
      }

      std::vector<char> buffer(bufferSize, '\0');
      const uint32_t actualSize = query(buffer.data(), bufferSize);
      if (!actualSize || actualSize > bufferSize
          || buffer[actualSize - 1] != '\0'
          || std::memchr(buffer.data(), '\0', actualSize - 1) != nullptr) {
        Logger::err(str::format("OpenVR: invalid extension query result (allocated ",
          bufferSize, ", returned ", actualSize, ")"));
        return false;
      }

      value.assign(buffer.data(), actualSize - 1);
      return true;
    }

  }

  VrInstance VrInstance::s_instance;

  VrInstance:: VrInstance() {
    m_no_vr = env::getEnvVar("DXVK_NO_VR") == "1";
  }
  VrInstance::~VrInstance() { }


  std::string_view VrInstance::getName() {
    return "OpenVR";
  }
  
  
  DxvkNameSet VrInstance::getInstanceExtensions() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    return m_insExtensions;
  }


  DxvkNameSet VrInstance::getDeviceExtensions(uint32_t adapterId) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    
    if (adapterId < m_devExtensions.size()
        && adapterId < m_devExtensionsValid.size()
        && m_devExtensionsValid[adapterId])
      return m_devExtensions[adapterId];
    
    return DxvkNameSet();
  }


  void VrInstance::initInstanceExtensions() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_no_vr || m_extensionQueryFailed || m_initializedDevExt)
        return;

    if (!m_vr_key)
    {
        LSTATUS status;

        if ((status = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0, KEY_READ, &m_vr_key)))
            Logger::info(str::format("OpenVR: could not open registry key, status ", status));
    }

    if (!m_vr_key && !m_compositor)
      m_compositor = this->getCompositor();

    if (!m_vr_key && !m_compositor)
      return;
    
    if (!this->queryInstanceExtensions(m_insExtensions)) {
      m_extensionQueryFailed = true;
      this->shutdown();
      return;
    }

    m_initializedInsExt = true;
  }


  void VrInstance::validateInstanceExtensions(const DxvkNameSet& available) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_no_vr || m_extensionQueryFailed || !m_initializedInsExt)
      return;

    const auto requested = m_insExtensions.toNameList();
    for (uint32_t i = 0; i < requested.count(); i++) {
      if (!available.supports(requested.name(i))) {
        Logger::warn(str::format(
          "OpenVR: requested Vulkan instance extension is unavailable: ",
          requested.name(i), "; disabling OpenVR integration"));

        m_extensionQueryFailed = true;
        m_insExtensions = DxvkNameSet();
        this->shutdown();
        return;
      }
    }
  }


  void VrInstance::initDeviceExtensions(const DxvkInstance* instance) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_no_vr || m_extensionQueryFailed || !m_initializedInsExt
        || (!m_vr_key && !m_compositor) || m_initializedDevExt)
      return;

    for (uint32_t i = 0; instance->enumAdapters(i) != nullptr; i++) {
      DxvkNameSet extensions;
      const bool valid = this->queryDeviceExtensions(instance->enumAdapters(i), extensions);

      // Device extension requirements are adapter-specific. A missing or
      // malformed entry for one GPU must not disable OpenVR on other GPUs.
      if (!valid) {
        Logger::warn(str::format(
          "OpenVR: failed to query Vulkan device extensions for adapter ", i,
          "; OpenVR will be unavailable on this adapter"));
      }

      m_devExtensions.push_back(std::move(extensions));
      m_devExtensionsValid.push_back(valid);
    }

    // Discovery is complete even if an individual adapter could not provide
    // its requirements. initializeSceneSession checks the selected adapter.
    m_initializedDevExt = true;
    this->shutdown();
  }

  void VrInstance::validateDeviceExtensions(uint32_t adapterId, const DxvkAdapter& adapter) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_no_vr || m_extensionQueryFailed || !m_initializedDevExt
        || adapterId >= m_devExtensions.size()
        || adapterId >= m_devExtensionsValid.size()
        || !m_devExtensionsValid[adapterId])
      return;

    const auto requested = m_devExtensions[adapterId].toNameList();
    for (uint32_t i = 0; i < requested.count(); i++) {
      if (!adapter.supportsExtension(requested.name(i))) {
        Logger::warn(str::format(
          "OpenVR: requested Vulkan device extension is unavailable for adapter ",
          adapterId, ": ", requested.name(i), "; disabling OpenVR on this adapter"));

        m_devExtensions[adapterId] = DxvkNameSet();
        m_devExtensionsValid[adapterId] = false;
        return;
      }
    }
  }


  bool VrInstance::waitVrKeyReady() const {
    DWORD type = 0;
    DWORD value = 0;
    DWORD wait_status = WAIT_FAILED;
    DWORD size = sizeof(value);
    LSTATUS status;
    HANDLE event;
    if ((status = RegQueryValueExA(m_vr_key, "state", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size)))
    {
        Logger::err(str::format("OpenVR: could not query value, status ", status));
        return false;
    }
    if (type != REG_DWORD || size != sizeof(value))
    {
        Logger::err(str::format("OpenVR: unexpected state value type or size (type ", type, ", size ", size, ")"));
        return false;
    }

    if (value)
        return value == 1;

    event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
      Logger::err(str::format("OpenVR: could not create registry notification event, error ", GetLastError()));
      return false;
    }

    while (1)
    {
        if (RegNotifyChangeKeyValue(m_vr_key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, event, TRUE))
        {
            Logger::err("Error registering registry change notification");
            goto done;
        }
        size = sizeof(value);
        if ((status = RegQueryValueExA(m_vr_key, "state", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size)))
        {
            Logger::err(str::format("OpenVR: could not query value, status ", status));
            goto done;
        }
        if (type != REG_DWORD || size != sizeof(value)) {
            Logger::err(str::format("OpenVR: invalid state value type or size (type ", type, ", size ", size, ")"));
            goto done;
        }
        if (value)
            break;
        while ((wait_status = WaitForSingleObject(event, 1000)) == WAIT_TIMEOUT)
            Logger::warn("VR state wait timeout (retrying)");

        if (wait_status != WAIT_OBJECT_0)
        {
            Logger::err(str::format("Got unexpected wait status ", wait_status));
            break;
        }
    }

  done:
    CloseHandle(event);
    return value == 1;
  }

  bool VrInstance::queryInstanceExtensions(DxvkNameSet& extensions) const {
    std::string extensionList;

    if (m_vr_key) {
      if (!this->waitVrKeyReady()
          || !readRegistryString(m_vr_key, "openvr_vulkan_instance_extensions", extensionList))
        return false;
    } else {
      if (!m_compositor || !queryOpenVrExtensionString(
          [this](char* buffer, uint32_t size) {
            return m_compositor->GetVulkanInstanceExtensionsRequired(buffer, size);
          }, extensionList))
        return false;
    }

    extensions = parseExtensionList(extensionList);
    return true;
  }
  
  
  bool VrInstance::queryDeviceExtensions(
      Rc<DxvkAdapter> adapter, DxvkNameSet& extensions) const {
    std::string extensionList;

    if (m_vr_key) {
      char name[32];
      const auto& properties = adapter->deviceProperties();
      sprintf(name, "PCIID:%04x:%04x",
        static_cast<uint16_t>(properties.vendorID),
        static_cast<uint16_t>(properties.deviceID));

      if (!this->waitVrKeyReady() || !readRegistryString(m_vr_key, name, extensionList))
        return false;
    } else {
      if (!m_compositor || !queryOpenVrExtensionString(
          [this, &adapter](char* buffer, uint32_t size) {
            return m_compositor->GetVulkanDeviceExtensionsRequired(adapter->handle(), buffer, size);
          }, extensionList))
        return false;
    }

    extensions = parseExtensionList(extensionList);
    return true;
  }
  
  
  DxvkNameSet VrInstance::parseExtensionList(const std::string& str) const {
    DxvkNameSet result;
    
    std::stringstream strstream(str);
    std::string       section;
    
    while (std::getline(strstream, section, ' '))
      result.add(section.c_str());
    
    return result;
  }
  
  
  vr::IVRCompositor* VrInstance::getCompositor() {
    // Skip OpenVR initialization if requested
    
    // Locate the OpenVR DLL if loaded by the process. Some
    // applications may not have OpenVR loaded at the time
    // they create the DXGI instance, so we try our own DLL.
    m_ovrApi = this->loadLibrary();
    
    if (!m_ovrApi) {
      Logger::info("OpenVR: Failed to locate module");
      return nullptr;
    }
    
    // Load method used to retrieve the IVRCompositor interface
    OutputDebugStringA("[DXVK x64] scene init: resolving OpenVR entry points\\n");
    g_vrFunctions.initInternal        = reinterpret_cast<VR_InitInternalProc>       (this->getSym("VR_InitInternal"));
    g_vrFunctions.shutdownInternal    = reinterpret_cast<VR_ShutdownInternalProc>   (this->getSym("VR_ShutdownInternal"));
    g_vrFunctions.getGenericInterface = reinterpret_cast<VR_GetGenericInterfaceProc>(this->getSym("VR_GetGenericInterface"));
    
    OutputDebugStringA(g_vrFunctions.getGenericInterface ? "[DXVK x64] scene init: GetGenericInterface resolved\\n" : "[DXVK x64] scene init: GetGenericInterface missing\\n");
    if (!g_vrFunctions.getGenericInterface) {
      Logger::warn("OpenVR: VR_GetGenericInterface not found");
      this->shutdown();
      return nullptr;
    }
    
    // Retrieve the compositor interface
    vr::EVRInitError error = vr::VRInitError_None;
    
    vr::IVRCompositor* compositor = reinterpret_cast<vr::IVRCompositor*>(
      g_vrFunctions.getGenericInterface(vr::IVRCompositor_Version, &error));
    
    if (error != vr::VRInitError_None || !compositor) {
      if (!g_vrFunctions.initInternal
       || !g_vrFunctions.shutdownInternal) {
        Logger::warn("OpenVR: VR_InitInternal or VR_ShutdownInternal not found");
        this->shutdown();
        return nullptr;
      }

      // If the app has not initialized OpenVR yet, we need
      // to do it now in order to grab a compositor instance
      g_vrFunctions.initInternal(&error, vr::VRApplication_Background);
      m_initializedOpenVr = error == vr::VRInitError_None;

      if (error != vr::VRInitError_None) {
        Logger::warn("OpenVR: Failed to initialize OpenVR");
        this->shutdown();
        return nullptr;
      }

      OutputDebugStringA("[DXVK x64] scene init: querying compositor after Scene init\\n");
      compositor = reinterpret_cast<vr::IVRCompositor*>(
        g_vrFunctions.getGenericInterface(vr::IVRCompositor_Version, &error));
      OutputDebugStringA(compositor ? "[DXVK x64] scene init: compositor acquired\\n" : "[DXVK x64] scene init: compositor query failed\\n");
      
      if (error != vr::VRInitError_None || !compositor) {
        Logger::warn("OpenVR: Failed to query compositor interface");
        this->shutdown();
        return nullptr;
      }
    }
    
    Logger::info("OpenVR: Compositor interface found");
    return compositor;
  }


  bool VrInstance::initializeSceneSession(const void* owner, uint32_t adapterId, bool force) {
    OutputDebugStringA("[DXVK x64] initializeSceneSession: entered\\n");
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    OutputDebugStringA("[DXVK x64] initializeSceneSession: mutex acquired\\n");

    if (!owner) {
      Logger::warn("OpenVR: refusing scene session claim without an owner");
      return false;
    }

    if (m_no_vr || (!force && !RtxVr::enable()))
      return false;

    // A scene session is only safe after both Vulkan extension queries
    // completed successfully before instance/device creation.
    if (!m_initializedInsExt || !m_initializedDevExt || m_extensionQueryFailed
        || adapterId >= m_devExtensionsValid.size()
        || !m_devExtensionsValid[adapterId]) {
      Logger::warn("OpenVR: Vulkan extension discovery did not complete successfully");
      return false;
    }

    if (m_sceneSessionActive && m_compositor && m_system) {
      if (m_sceneSessionAdapterId != adapterId) {
        Logger::warn("OpenVR: refusing to share a scene session across different Vulkan adapters");
        return false;
      }
      m_sceneSessionOwners.insert(owner);
      return true;
    }

    OutputDebugStringA("[DXVK x64] scene init: before loadLibrary\\n");
    if (!m_ovrApi)
      m_ovrApi = this->loadLibrary();
    OutputDebugStringA(m_ovrApi ? "[DXVK x64] scene init: runtime library loaded\\n" : "[DXVK x64] scene init: runtime library unavailable\\n");

    if (!m_ovrApi) {
      Logger::warn("OpenVR: failed to load runtime for scene session");
      return false;
    }

    auto releaseFailedInitialization = [this](bool shutdownRuntime) {
      if (shutdownRuntime && g_vrFunctions.shutdownInternal)
        g_vrFunctions.shutdownInternal();
      if (m_loadedOvrApi)
        this->freeLibrary();
      m_loadedOvrApi = false;
      m_ovrApi = nullptr;
    };

    g_vrFunctions.initInternal        = reinterpret_cast<VR_InitInternalProc>       (this->getSym("VR_InitInternal"));
    g_vrFunctions.shutdownInternal    = reinterpret_cast<VR_ShutdownInternalProc>   (this->getSym("VR_ShutdownInternal"));
    g_vrFunctions.getGenericInterface = reinterpret_cast<VR_GetGenericInterfaceProc>(this->getSym("VR_GetGenericInterface"));

    if (!g_vrFunctions.getGenericInterface) {
      Logger::warn("OpenVR: VR_GetGenericInterface not found");
      releaseFailedInitialization(false);
      return false;
    }

    // Prefer an existing host-owned OpenVR scene session. Do not initialize or
    // shut down a second runtime instance when the game already owns one.
    vr::EVRInitError error = vr::VRInitError_None;
    OutputDebugStringA("[DXVK x64] scene init: querying host compositor interface\\n");
    auto* compositor = reinterpret_cast<vr::IVRCompositor*>(
      g_vrFunctions.getGenericInterface(vr::IVRCompositor_Version, &error));
    OutputDebugStringA(compositor ? "[DXVK x64] scene init: host compositor found\\n" : "[DXVK x64] scene init: host compositor unavailable\\n");

    bool initializedHere = false;
    if (error != vr::VRInitError_None || !compositor) {
      if (!g_vrFunctions.initInternal || !g_vrFunctions.shutdownInternal) {
        Logger::warn("OpenVR: initialization entry points are unavailable");
        releaseFailedInitialization(false);
        return false;
      }

      OutputDebugStringA("[DXVK x64] scene init: before VR_InitInternal(Scene)\\n");
      g_vrFunctions.initInternal(&error, vr::VRApplication_Scene);
      OutputDebugStringA("[DXVK x64] scene init: VR_InitInternal returned\\n");
      if (error != vr::VRInitError_None) {
        Logger::warn(str::format("OpenVR: scene initialization failed: ", static_cast<int>(error)));
        releaseFailedInitialization(false);
        return false;
      }

      initializedHere = true;
      compositor = reinterpret_cast<vr::IVRCompositor*>(
        g_vrFunctions.getGenericInterface(vr::IVRCompositor_Version, &error));
    }

    if (error != vr::VRInitError_None || !compositor) {
      Logger::warn("OpenVR: failed to acquire compositor for scene session");
      releaseFailedInitialization(initializedHere);
      return false;
    }

    OutputDebugStringA("[DXVK x64] scene init: querying IVRSystem\\n");
    auto* system = reinterpret_cast<vr::IVRSystem*>(
      g_vrFunctions.getGenericInterface(vr::IVRSystem_Version, &error));
    OutputDebugStringA(system ? "[DXVK x64] scene init: IVRSystem acquired\\n" : "[DXVK x64] scene init: IVRSystem query failed\\n");
    if (error != vr::VRInitError_None || !system) {
      Logger::warn("OpenVR: failed to acquire IVRSystem");
      releaseFailedInitialization(initializedHere);
      return false;
    }

    OutputDebugStringA("[DXVK x64] scene init: publishing scene session state\\n");
    m_compositor = compositor;
    m_compositor->SetTrackingSpace(vr::TrackingUniverseStanding);
    m_system = system;
    m_initializedOpenVr = initializedHere;
    m_sceneSessionAdapterId = adapterId;
    m_sceneSessionOwners.insert(owner);
    m_sceneSessionActive = true;
    Logger::info("OpenVR: scene session is active");
    return true;
  }


  bool VrInstance::acquireFrame(OpenVrFrameState& frame) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (!m_sceneSessionActive || !m_compositor || !m_system)
      return false;

    OpenVrFrameState next = {};
    const auto error = m_compositor->WaitGetPoses(
      next.renderPoses.data(), static_cast<uint32_t>(next.renderPoses.size()), nullptr, 0);
    if (error != vr::VRCompositorError_None) {
      // The call repeats every frame, so only every 600th failure is logged.
      if (m_waitGetPosesFailures++ % 600 == 0) {
        Logger::warn(str::format("OpenVR: WaitGetPoses failed: ", static_cast<int>(error)));
      }
      return false;
    }

    next.frameIndex = ++m_frameIndex;
    next.hmdPoseValid = next.renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
    m_system->GetRecommendedRenderTargetSize(&next.recommendedWidth, &next.recommendedHeight);

    if (next.hmdPoseValid) {
      next.trackingFromHead = OpenVrMatrix4::fromHmdMatrix34(
        next.renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);
      m_lastValidTrackingFromHead = next.trackingFromHead;
      m_hasLastValidTrackingFromHead = true;
    } else if (m_hasLastValidTrackingFromHead) {
      // Match OpenRBRVR's retained-pose fallback when tracking is temporarily invalid.
      next.trackingFromHead = m_lastValidTrackingFromHead;
    }

    constexpr std::array<vr::EVREye, 2> eyes = {
      vr::Eye_Left,
      vr::Eye_Right,
    };

    for (size_t i = 0; i < eyes.size(); i++) {
      next.eyeToHead[i] = m_system->GetEyeToHeadTransform(eyes[i]);
      m_system->GetProjectionRaw(
        eyes[i],
        &next.projection[i].left,
        &next.projection[i].right,
        &next.projection[i].top,
        &next.projection[i].bottom);
      next.projectionMatrix[i] = m_system->GetProjectionMatrix(eyes[i], 0.01f, 10000.0f);

      if (next.hmdPoseValid) {
        const auto headFromEye = OpenVrMatrix4::fromHmdMatrix34(next.eyeToHead[i]);
        next.eyeFromTracking[i] = headFromEye.inverseRigid().multipliedBy(
          next.trackingFromHead.inverseRigid());
      }
    }

    // Publish only a complete snapshot. A caller never observes a mixture of
    // poses from one frame and eye parameters from another.
    frame = next;
    // A successful WaitGetPoses call is a usable snapshot even while the HMD
    // pose is temporarily invalid. Consumers can retain the last valid pose.
    return true;
  }


  void VrInstance::registerExternalFrameProvider(const void* owner) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (!owner || !m_sceneSessionActive || m_sceneSessionOwners.find(owner) == m_sceneSessionOwners.end()) {
      Logger::warn("OpenVR: refusing external frame provider registration from a non-owner");
      return;
    }
    if (m_externalFrameProvider && m_externalFrameProvider != owner) {
      Logger::warn("OpenVR: refusing a second external frame provider");
      return;
    }
    m_externalFrameProvider = owner;
    m_externalFramePending = false;
    m_latestExternalFrameIndex = 0;
    m_rtxSubmittedFrameIndex = 0;
  }


  void VrInstance::publishExternalFrame(const void* owner, const OpenVrFrameState& frame) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (owner != m_externalFrameProvider || frame.frameIndex == 0)
      return;
    m_externalFrame = frame;
    m_externalFramePending = true;
    m_latestExternalFrameIndex = frame.frameIndex;
    m_rtxSubmittedFrameIndex = 0;
  }


  void VrInstance::invalidateExternalFrame(const void* owner) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (owner == m_externalFrameProvider) {
      m_externalFramePending = false;
      m_externalFrame = {};
      m_latestExternalFrameIndex = 0;
      m_rtxSubmittedFrameIndex = 0;
    }
  }


  bool VrInstance::consumeExternalFrame(OpenVrFrameState& frame, bool& providerActive) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    providerActive = m_externalFrameProvider != nullptr;
    if (!providerActive || !m_externalFramePending)
      return false;
    frame = m_externalFrame;
    m_externalFramePending = false;
    return true;
  }


  void VrInstance::markRtxFrameSubmitted(uint64_t frameIndex) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (frameIndex != 0 && frameIndex == m_latestExternalFrameIndex)
      m_rtxSubmittedFrameIndex = frameIndex;
  }


  bool VrInstance::wasRtxFrameSubmittedForLatestExternalFrame() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    return m_latestExternalFrameIndex != 0
        && m_rtxSubmittedFrameIndex == m_latestExternalFrameIndex;
  }


  bool VrInstance::submitFrame(
      const std::array<vr::VRVulkanTextureData_t, 2>& textures,
      vr::EColorSpace colorSpace) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (!m_sceneSessionActive || !m_compositor)
      return false;

    // Keep the Vulkan descriptors alive for both synchronous Submit calls.
    auto vulkanTextures = textures;
    constexpr std::array<vr::EVREye, 2> eyes = {
      vr::Eye_Left,
      vr::Eye_Right,
    };

    bool success = true;
    for (size_t i = 0; i < eyes.size(); i++) {
      vr::Texture_t texture = {
        &vulkanTextures[i],
        vr::TextureType_Vulkan,
        colorSpace,
      };

      const auto error = m_compositor->Submit(eyes[i], &texture);
      if (error != vr::VRCompositorError_None) {
        Logger::warn(str::format(
          "OpenVR: Submit failed for ", i == 0 ? "left" : "right",
          " eye: ", static_cast<int>(error)));
        success = false;
      }
    }

    m_compositor->PostPresentHandoff();
    return success;
  }


  bool VrInstance::getFrameTiming(vr::Compositor_FrameTiming& timing) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    timing = {};
    timing.m_nSize = sizeof(timing);
    return m_sceneSessionActive && m_compositor
        && m_compositor->GetFrameTiming(&timing);
  }


  bool VrInstance::resetView() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (!m_sceneSessionActive || !m_system)
      return false;

    // The current OpenVR IVRSystem API exposes seated-origin recentering.
    // It does not provide the former standing chaperone zero-pose reset.
    m_system->ResetSeatedZeroPose();
    return true;
  }


  bool VrInstance::waitGetPoses(
      std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount>& renderPoses) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (!m_sceneSessionActive || !m_compositor)
      return false;

    // WaitGetPoses is the frame synchronization point for a scene application.
    // Keep this call on the render thread and call it once per HMD frame.
    const auto error = m_compositor->WaitGetPoses(
      renderPoses.data(), static_cast<uint32_t>(renderPoses.size()), nullptr, 0);
    if (error != vr::VRCompositorError_None) {
      Logger::warn(str::format("OpenVR: WaitGetPoses failed: ", static_cast<int>(error)));
      return false;
    }
    return true;
  }


  bool VrInstance::getRecommendedRenderTargetSize(uint32_t& width, uint32_t& height) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (!m_sceneSessionActive || !m_system)
      return false;

    m_system->GetRecommendedRenderTargetSize(&width, &height);
    return width != 0 && height != 0;
  }


  bool VrInstance::getEyeToHeadTransform(vr::EVREye eye, vr::HmdMatrix34_t& transform) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (!m_sceneSessionActive || !m_system)
      return false;

    transform = m_system->GetEyeToHeadTransform(eye);
    return true;
  }


  bool VrInstance::getProjectionRaw(vr::EVREye eye, float& left, float& right, float& top, float& bottom) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    if (!m_sceneSessionActive || !m_system)
      return false;

    m_system->GetProjectionRaw(eye, &left, &right, &top, &bottom);
    return true;
  }


  void VrInstance::shutdownSceneSession(const void* owner) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (!owner || m_sceneSessionOwners.erase(owner) == 0) {
      Logger::warn("OpenVR: ignoring scene session release from a non-owner");
      return;
    }

    if (m_externalFrameProvider == owner) {
      m_externalFrameProvider = nullptr;
      m_externalFramePending = false;
      m_externalFrame = {};
    }

    // RTX and D3D9VR may share one DxvkDevice and one OpenVR Scene session.
    // Keep the session alive until its final registered owner releases it.
    if (!m_sceneSessionOwners.empty())
      return;

    if (m_initializedOpenVr && g_vrFunctions.shutdownInternal)
      g_vrFunctions.shutdownInternal();

    m_initializedOpenVr = false;
    m_sceneSessionActive = false;
    m_sceneSessionAdapterId = UINT32_MAX;
    m_compositor = nullptr;
    m_system = nullptr;
    m_externalFrameProvider = nullptr;
    m_externalFramePending = false;
    m_externalFrame = {};
    m_latestExternalFrameIndex = 0;
    m_rtxSubmittedFrameIndex = 0;
    m_hasLastValidTrackingFromHead = false;
    m_lastValidTrackingFromHead = {};

    if (m_loadedOvrApi)
      this->freeLibrary();

    m_loadedOvrApi = false;
    m_ovrApi = nullptr;
  }


  void VrInstance::shutdown() {
    if (m_vr_key)
    {
        RegCloseKey(m_vr_key);
        m_vr_key = nullptr;
    }

    if (m_initializedOpenVr && g_vrFunctions.shutdownInternal)
      g_vrFunctions.shutdownInternal();

    m_compositor = nullptr;
    m_system = nullptr;
    m_sceneSessionActive = false;
    m_sceneSessionAdapterId = UINT32_MAX;
    m_sceneSessionOwners.clear();
    m_externalFrameProvider = nullptr;
    m_externalFramePending = false;
    m_externalFrame = {};
    m_hasLastValidTrackingFromHead = false;
    m_lastValidTrackingFromHead = {};
    m_initializedOpenVr = false;

    if (m_loadedOvrApi)
      this->freeLibrary();
    
    m_initializedOpenVr = false;
    m_loadedOvrApi      = false;
    m_ovrApi            = nullptr;
  }


  HMODULE VrInstance::loadLibrary() {
    m_loadedOvrApi = false;

    if (HMODULE handle = ::GetModuleHandleA("openvr_api.dll"))
      return handle;

    if (HMODULE handle = ::LoadLibraryA("openvr_api_dxvk.dll")) {
      m_loadedOvrApi = true;
      return handle;
    }

    // The x86 game process cannot share its loaded openvr_api.dll with the
    // x64 Bridge Server. Resolve SteamVR's native x64 loader directly from
    // Steam's registered installation path instead of relying on PATH or the
    // server's current working directory.
    struct RegistryLocation {
      HKEY root;
      const char* subkey;
    };
    const RegistryLocation locations[] = {
      { HKEY_CURRENT_USER, "Software\\Valve\\Steam" },
      { HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam" },
      { HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam" },
    };
    constexpr const char* valueNames[] = { "SteamPath", "InstallPath" };

    for (const auto& location : locations) {
      HKEY key = nullptr;
      if (RegOpenKeyExA(location.root, location.subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        continue;

      for (const char* valueName : valueNames) {
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExA(key, valueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS
            || type != REG_SZ || size < 2 || size > 32768)
          continue;

        std::vector<char> value(size + 1, '\0');
        DWORD actualSize = size;
        DWORD actualType = 0;
        if (RegQueryValueExA(key, valueName, nullptr, &actualType,
            reinterpret_cast<BYTE*>(value.data()), &actualSize) != ERROR_SUCCESS
            || actualType != REG_SZ || actualSize < 2 || actualSize > size)
          continue;

        std::string steamPath(value.data());
        while (!steamPath.empty() && (steamPath.back() == '\\' || steamPath.back() == '/'))
          steamPath.pop_back();
        for (char& ch : steamPath) {
          if (ch == '/')
            ch = '\\';
        }

        const auto tryLoadFromLibrary = [&](const std::string& libraryPath) -> HMODULE {
          const std::string dllPath = libraryPath
            + "\\steamapps\\common\\SteamVR\\bin\\win64\\openvr_api.dll";
          HMODULE loaded = ::LoadLibraryExA(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
          if (loaded) {
            m_loadedOvrApi = true;
            Logger::info(str::format("OpenVR: loaded x64 API from ", dllPath));
          }
          return loaded;
        };

        if (HMODULE handle = tryLoadFromLibrary(steamPath)) {
          RegCloseKey(key);
          return handle;
        }

        // SteamVR may live in a secondary Steam library. Read the registered
        // library roots instead of assuming the default steamapps directory.
        std::ifstream libraryFolders(steamPath + "\\steamapps\\libraryfolders.vdf");
        std::string line;
        while (std::getline(libraryFolders, line)) {
          const size_t pathKey = line.find("\"path\"");
          if (pathKey == std::string::npos)
            continue;
          const size_t valueStart = line.find('"', pathKey + 6);
          const size_t valueEnd = valueStart == std::string::npos
            ? std::string::npos : line.find('"', valueStart + 1);
          if (valueStart == std::string::npos || valueEnd == std::string::npos)
            continue;

          std::string libraryPath = line.substr(valueStart + 1, valueEnd - valueStart - 1);
          for (size_t i = 0; i + 1 < libraryPath.size();) {
            if (libraryPath[i] == '\\' && libraryPath[i + 1] == '\\')
              libraryPath.erase(i, 1);
            else
              i++;
          }
          for (char& ch : libraryPath) {
            if (ch == '/')
              ch = '\\';
          }

          if (HMODULE handle = tryLoadFromLibrary(libraryPath)) {
            RegCloseKey(key);
            return handle;
          }
        }
      }

      RegCloseKey(key);
    }

    Logger::warn(str::format("OpenVR: could not locate x64 openvr_api.dll (last error ",
      ::GetLastError(), ")"));
    return nullptr;
  }


  void VrInstance::freeLibrary() {
    ::FreeLibrary(m_ovrApi);
  }

  
  void* VrInstance::getSym(const char* sym) {
    return reinterpret_cast<void*>(
      ::GetProcAddress(m_ovrApi, sym));
  }
  
}
