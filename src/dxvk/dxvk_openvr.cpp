#include "dxvk_instance.h"
#include "dxvk_openvr.h"
#include "rtx_render/rtx_vr.h"

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#include <openvr/openvr.hpp>

#include <cstring>

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
          || buffer[actualSize - 1] != '\\0'
          || std::memchr(buffer.data(), '\\0', actualSize - 1) != nullptr) {
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

      std::vector<char> buffer(bufferSize, '\\0');
      const uint32_t actualSize = query(buffer.data(), bufferSize);
      if (!actualSize || actualSize > bufferSize
          || buffer[actualSize - 1] != '\\0'
          || std::memchr(buffer.data(), '\\0', actualSize - 1) != nullptr) {
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
    
    if (adapterId < m_devExtensions.size())
      return m_devExtensions[adapterId];
    
    return DxvkNameSet();
  }


  void VrInstance::initInstanceExtensions() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_no_vr || m_initializedDevExt)
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
    
    m_insExtensions = this->queryInstanceExtensions();
    m_initializedInsExt = true;
  }


  void VrInstance::initDeviceExtensions(const DxvkInstance* instance) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_no_vr || (!m_vr_key && !m_compositor) || m_initializedDevExt)
      return;
    
    for (uint32_t i = 0; instance->enumAdapters(i) != nullptr; i++) {
      m_devExtensions.push_back(this->queryDeviceExtensions(
        instance->enumAdapters(i)));
    }

    m_initializedDevExt = true;
    this->shutdown();
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
    if (type != REG_DWORD)
    {
        Logger::err(str::format("OpenVR: unexpected value type ", type));
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

  DxvkNameSet VrInstance::queryInstanceExtensions() const {
    std::string extensionList;

    if (m_vr_key) {
      if (!this->waitVrKeyReady()
          || !readRegistryString(m_vr_key, "openvr_vulkan_instance_extensions", extensionList))
        return DxvkNameSet();
    } else {
      if (!m_compositor || !queryOpenVrExtensionString(
          [this](char* buffer, uint32_t size) {
            return m_compositor->GetVulkanInstanceExtensionsRequired(buffer, size);
          }, extensionList))
        return DxvkNameSet();
    }

    return parseExtensionList(extensionList);
  }
  
  
  DxvkNameSet VrInstance::queryDeviceExtensions(Rc<DxvkAdapter> adapter) const {
    std::string extensionList;

    if (m_vr_key) {
      char name[32];
      const auto& properties = adapter->deviceProperties();
      sprintf(name, "PCIID:%04x:%04x",
        static_cast<uint16_t>(properties.vendorID),
        static_cast<uint16_t>(properties.deviceID));

      if (!this->waitVrKeyReady() || !readRegistryString(m_vr_key, name, extensionList))
        return DxvkNameSet();
    } else {
      if (!m_compositor || !queryOpenVrExtensionString(
          [this, &adapter](char* buffer, uint32_t size) {
            return m_compositor->GetVulkanDeviceExtensionsRequired(adapter->handle(), buffer, size);
          }, extensionList))
        return DxvkNameSet();
    }

    return parseExtensionList(extensionList);
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
    g_vrFunctions.initInternal        = reinterpret_cast<VR_InitInternalProc>       (this->getSym("VR_InitInternal"));
    g_vrFunctions.shutdownInternal    = reinterpret_cast<VR_ShutdownInternalProc>   (this->getSym("VR_ShutdownInternal"));
    g_vrFunctions.getGenericInterface = reinterpret_cast<VR_GetGenericInterfaceProc>(this->getSym("VR_GetGenericInterface"));
    
    if (!g_vrFunctions.getGenericInterface) {
      Logger::warn("OpenVR: VR_GetGenericInterface not found");
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
        return nullptr;
      }

      // If the app has not initialized OpenVR yet, we need
      // to do it now in order to grab a compositor instance
      g_vrFunctions.initInternal(&error, vr::VRApplication_Background);
      m_initializedOpenVr = error == vr::VRInitError_None;

      if (error != vr::VRInitError_None) {
        Logger::warn("OpenVR: Failed to initialize OpenVR");
        return nullptr;
      }

      compositor = reinterpret_cast<vr::IVRCompositor*>(
        g_vrFunctions.getGenericInterface(vr::IVRCompositor_Version, &error));
      
      if (error != vr::VRInitError_None || !compositor) {
        Logger::warn("OpenVR: Failed to query compositor interface");
        this->shutdown();
        return nullptr;
      }
    }
    
    Logger::info("OpenVR: Compositor interface found");
    return compositor;
  }


  bool VrInstance::initializeSceneSession(const void* owner) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (!owner) {
      Logger::warn("OpenVR: refusing scene session claim without an owner");
      return false;
    }

    if (m_no_vr || !RtxVr::enable())
      return false;

    if (m_sceneSessionActive && m_compositor && m_system) {
      if (m_sceneSessionOwner == owner)
        return true;
      Logger::warn("OpenVR: scene session is already owned by another DXVK device");
      return false;
    }

    if (!m_ovrApi)
      m_ovrApi = this->loadLibrary();

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
    auto* compositor = reinterpret_cast<vr::IVRCompositor*>(
      g_vrFunctions.getGenericInterface(vr::IVRCompositor_Version, &error));

    bool initializedHere = false;
    if (error != vr::VRInitError_None || !compositor) {
      if (!g_vrFunctions.initInternal || !g_vrFunctions.shutdownInternal) {
        Logger::warn("OpenVR: initialization entry points are unavailable");
        releaseFailedInitialization(false);
        return false;
      }

      g_vrFunctions.initInternal(&error, vr::VRApplication_Scene);
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

    auto* system = reinterpret_cast<vr::IVRSystem*>(
      g_vrFunctions.getGenericInterface(vr::IVRSystem_Version, &error));
    if (error != vr::VRInitError_None || !system) {
      Logger::warn("OpenVR: failed to acquire IVRSystem");
      releaseFailedInitialization(initializedHere);
      return false;
    }

    m_compositor = compositor;
    m_system = system;
    m_initializedOpenVr = initializedHere;
    m_sceneSessionOwner = owner;
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

      if (next.hmdPoseValid) {
        const auto headFromEye = OpenVrMatrix4::fromHmdMatrix34(next.eyeToHead[i]);
        next.eyeFromTracking[i] = headFromEye.inverseRigid().multipliedBy(
          next.trackingFromHead.inverseRigid());
      }
    }

    // Publish only a complete snapshot. A caller never observes a mixture of
    // poses from one frame and eye parameters from another.
    frame = next;
    return next.hmdPoseValid;
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

    return success;
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

    if (!owner || owner != m_sceneSessionOwner) {
      Logger::warn("OpenVR: ignoring scene session release from a non-owner");
      return;
    }

    if (m_initializedOpenVr && g_vrFunctions.shutdownInternal)
      g_vrFunctions.shutdownInternal();

    m_initializedOpenVr = false;
    m_sceneSessionActive = false;
    m_sceneSessionOwner = nullptr;
    m_compositor = nullptr;
    m_system = nullptr;

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
    m_sceneSessionOwner = nullptr;
    m_initializedOpenVr = false;

    if (m_loadedOvrApi)
      this->freeLibrary();
    
    m_initializedOpenVr = false;
    m_loadedOvrApi      = false;
    m_ovrApi            = nullptr;
  }


  HMODULE VrInstance::loadLibrary() {
    HMODULE handle = nullptr;
    if (!(handle = ::GetModuleHandle("openvr_api.dll"))) {
      handle = ::LoadLibrary("openvr_api_dxvk.dll");
      m_loadedOvrApi = handle != nullptr;
    }
    return handle;
  }


  void VrInstance::freeLibrary() {
    ::FreeLibrary(m_ovrApi);
  }

  
  void* VrInstance::getSym(const char* sym) {
    return reinterpret_cast<void*>(
      ::GetProcAddress(m_ovrApi, sym));
  }
  
}
