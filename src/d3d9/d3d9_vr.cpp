#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"
#include "d3d9_include.h"
#include "d3d9_surface.h"
#include "d3d9_device.h"
#include "d3d9_shader.h"
#include "../dxvk/dxvk_openvr.h"

#include <atomic>
#include <vector>

namespace dxvk {

class D3D9VR final : public ComObjectClamp<IDirect3DVR9> {
public:
  explicit D3D9VR(IDirect3DDevice9* device)
  : m_device(static_cast<D3D9DeviceEx*>(device)) { }

  ~D3D9VR() {
    if (m_openVrSceneSession)
      VrInstance::s_instance.shutdownSceneSession(this);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVR9)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetVRDesc(IDirect3DSurface9* surface, D3D9_TEXTURE_VR_DESC* desc) {
    if (surface == nullptr || desc == nullptr)
      return D3DERR_INVALIDCALL;

    D3D9DeviceLock lock = m_device->LockDevice();

    auto* d3dSurface = static_cast<D3D9Surface*>(surface);
    auto* texture = d3dSurface->GetCommonTexture();
    const auto& image = texture->GetImage();
    if (image == nullptr)
      return D3DERR_INVALIDCALL;

    const auto& imageInfo = image->info();
    constexpr VkImageUsageFlags requiredUsage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // OpenVR's Vulkan texture descriptor has no array-layer selector.
    // Only expose a single-layer 2D image with the usage required by SteamVR.
    if (imageInfo.type != VK_IMAGE_TYPE_2D
     || imageInfo.extent.depth != 1
     || imageInfo.numLayers != 1
     || (imageInfo.usage & requiredUsage) != requiredUsage)
      return D3DERR_NOTAVAILABLE;

    // Keep this list in sync with Valve's documented Vulkan compositor formats.
    switch (imageInfo.format) {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_SRGB:
      case VK_FORMAT_R32G32B32A32_SFLOAT:
      case VK_FORMAT_R32G32B32_SFLOAT:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_A2R10G10B10_UINT_PACK32:
        break;
      default:
        return D3DERR_NOTAVAILABLE;
    }

    auto device = texture->Device()->GetDXVKDevice();

    desc->Image = uint64_t(image->handle());
    desc->Device = device->handle();
    desc->PhysicalDevice = device->adapter()->handle();
    desc->Instance = device->instance()->handle();
    desc->Queue = device->queues().graphics.queueHandle;
    desc->QueueFamilyIndex = device->queues().graphics.queueFamily;
    desc->Width = imageInfo.extent.width;
    desc->Height = imageInfo.extent.height;
    desc->Format = imageInfo.format;
    desc->SampleCount = uint32_t(imageInfo.sampleCount);
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE TransferSurfaceForVR(IDirect3DSurface9* surface) {
    if (surface == nullptr)
      return D3DERR_INVALIDCALL;

    D3D9DeviceLock lock = m_device->LockDevice();

    auto* texture = static_cast<D3D9Surface*>(surface)->GetCommonTexture();
    auto image = texture->GetImage();
    if (image == nullptr)
      return D3DERR_INVALIDCALL;

    for (const auto& entry : m_vrImages) {
      if (entry.image->handle() == image->handle())
        return D3D_OK;
    }

    // Wait for the previous frame's restore transition before capturing
    // the layout to which this image must return after compositor submission.
    if (m_vrImages.empty())
      m_device->SynchronizeCsThread();

    const VkImageLayout originalLayout = image->info().layout;
    const VkPipelineStageFlags originalStages = image->info().stages;
    const VkAccessFlags originalAccess = image->info().access;
    m_vrImages.push_back({ image, originalLayout, originalStages, originalAccess });
    m_device->TransitionImageForVR(
      image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      originalStages, originalAccess,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE BeginVRSubmit() {
    const uint32_t threadId = GetCurrentThreadId();
    if (m_submissionQueueThreadId.load(std::memory_order_acquire) == threadId)
      return D3DERR_INVALIDCALL;

    // Acquire into a local first: checking m_vrSubmitActive before taking
    // the device lock lets concurrent callers both pass the check.
    D3D9DeviceLock submitLock = m_device->LockDevice();
    if (m_vrSubmitActive.load(std::memory_order_acquire))
      return D3DERR_INVALIDCALL;

    // Keep D3D9 commands out of the interval in which the external
    // compositor owns the Vulkan queue and the VR images.
    m_device->SynchronizeCsThread();
    m_device->GetDXVKDevice()->lockSubmission();
    m_vrSubmitDeviceLock = std::move(submitLock);
    m_submissionQueueThreadId.store(threadId, std::memory_order_release);
    m_vrSubmitActive.store(true, std::memory_order_release);
    AddRefPrivate();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE EndVRSubmit() {
    if (!m_vrSubmitActive.load(std::memory_order_acquire)
     || m_submissionQueueThreadId.load(std::memory_order_acquire) != GetCurrentThreadId())
      return D3DERR_INVALIDCALL;

    m_device->GetDXVKDevice()->unlockSubmission();

    // Queue their restoration before allowing another D3D9 thread
    // to record commands that may access these images.
    for (const auto& entry : m_vrImages)
      m_device->TransitionImageForVR(
        entry.image, entry.originalLayout,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        entry.originalStages, entry.originalAccess);

    m_vrImages.clear();
    m_vrSubmitActive.store(false, std::memory_order_release);
    m_submissionQueueThreadId.store(0, std::memory_order_release);
    m_vrSubmitDeviceLock = D3D9DeviceLock();
    ReleasePrivate();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE LockDevice() {
    const uint32_t threadId = GetCurrentThreadId();
    if (m_deviceLockHeld.exchange(true, std::memory_order_acq_rel))
      return D3DERR_INVALIDCALL;

    D3D9DeviceLock deviceLock = m_device->LockDevice();
    m_deviceLock = std::move(deviceLock);
    m_deviceLockThreadId.store(threadId, std::memory_order_release);
    AddRefPrivate();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE UnlockDevice() {
    if (!m_deviceLockHeld.load(std::memory_order_acquire)
     || m_deviceLockThreadId.load(std::memory_order_acquire) != GetCurrentThreadId())
      return D3DERR_INVALIDCALL;

    m_deviceLockHeld.store(false, std::memory_order_release);
    m_deviceLockThreadId.store(0, std::memory_order_release);
    m_deviceLock = D3D9DeviceLock();
    ReleasePrivate();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE WaitDeviceIdle(BOOL flush) {
    if (m_submissionQueueThreadId.load(std::memory_order_acquire) == GetCurrentThreadId())
      return D3DERR_INVALIDCALL;
    D3D9DeviceLock lock = m_device->LockDevice();
    if (flush) {
      m_device->Flush();
      m_device->SynchronizeCsThread();
    }
    m_device->GetDXVKDevice()->waitForIdle();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE WaitGraphicsQueueIdle(BOOL flush) {
    if (m_submissionQueueThreadId.load(std::memory_order_acquire) == GetCurrentThreadId())
      return D3DERR_INVALIDCALL;
    D3D9DeviceLock lock = m_device->LockDevice();
    if (flush) {
      m_device->Flush();
      m_device->SynchronizeCsThread();
    }
    auto device = m_device->GetDXVKDevice();
    device->lockSubmission();
    const VkResult result = device->vkd()->vkQueueWaitIdle(device->queues().graphics.queueHandle);
    device->unlockSubmission();
    return result == VK_SUCCESS ? D3D_OK : D3DERR_NOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE GetOXRVkDeviceDesc(OXR_VK_DEVICE_DESC* desc) {
    if (desc == nullptr)
      return D3DERR_INVALIDCALL;

    auto device = m_device->GetDXVKDevice();
    desc->Device = device->handle();
    desc->PhysicalDevice = device->adapter()->handle();
    desc->Instance = device->instance()->handle();
    desc->QueueIndex = device->queues().graphics.queueIndex;
    desc->QueueFamilyIndex = device->queues().graphics.queueFamily;
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Flush() {
    m_device->Flush();
    m_device->SynchronizeCsThread();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE LockSubmissionQueue() {
    const uint32_t threadId = GetCurrentThreadId();
    if (m_submissionQueueThreadId.load(std::memory_order_acquire) == threadId)
      return D3DERR_INVALIDCALL;

    D3D9DeviceLock deviceLock = m_device->LockDevice();
    m_device->GetDXVKDevice()->lockSubmission();
    m_submissionQueueDeviceLock = std::move(deviceLock);
    m_submissionQueueThreadId.store(threadId, std::memory_order_release);
    AddRefPrivate();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() {
    if (m_vrSubmitActive.load(std::memory_order_acquire)
     || m_submissionQueueThreadId.load(std::memory_order_acquire) != GetCurrentThreadId())
      return D3DERR_INVALIDCALL;

    m_submissionQueueThreadId.store(0, std::memory_order_release);
    m_device->GetDXVKDevice()->unlockSubmission();
    m_submissionQueueDeviceLock = D3D9DeviceLock();
    ReleasePrivate();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE ImportFence(HANDLE handle, uint64_t value) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
      return D3DERR_INVALIDCALL;

    DxvkFenceCreateInfo info = {};
    info.initialValue = value;
    info.sharedType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    info.sharedHandle = handle;

    try {
      m_fence = m_device->GetDXVKDevice()->createFence(info);
    } catch (const DxvkError&) {
      m_fence = nullptr;
      return E_FAIL;
    }

    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE SignalFence(uint64_t value) {
    if (m_fence == nullptr)
      return D3DERR_INVALIDCALL;

    return m_fence->signal(value) == VK_SUCCESS ? D3D_OK : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE GetShaderHash(IDirect3DVertexShader9* d3dShader, char** out) {
    if (d3dShader == nullptr || out == nullptr)
      return D3DERR_INVALIDCALL;

    auto* shader = reinterpret_cast<D3D9Shader<IDirect3DVertexShader9>*>(d3dShader);
    auto dxvkShader = shader->GetCommonShader()->GetShader(D3D9ShaderPermutations::None);
    if (dxvkShader == nullptr)
      return D3DERR_INVALIDCALL;

    const std::string key = dxvkShader->getShaderKey().toString();
    if (key.size() >= 64)
      return D3DERR_INVALIDCALL;

    char* buffer = reinterpret_cast<char*>(out);
    std::memcpy(buffer, key.data(), key.size());
    buffer[key.size()] = '\0';
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE PatchSPIRVToVertexShader(
      IDirect3DVertexShader9* d3dShader, const uint32_t* data, uint32_t size) {
    if (d3dShader == nullptr || data == nullptr || size < 5
     || data[0] != spv::MagicNumber)
      return D3DERR_INVALIDCALL;

    auto* shader = static_cast<D3D9Shader<IDirect3DVertexShader9>*>(d3dShader);
    auto dxvkShader = shader->GetCommonShader()->GetShader(D3D9ShaderPermutations::None);
    if (dxvkShader == nullptr || dxvkShader->stage() != VK_SHADER_STAGE_VERTEX_BIT)
      return D3DERR_INVALIDCALL;

    dxvkShader->replaceCode(SpirvCodeBuffer(size, data));
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateMultiViewRenderTarget(
      UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample,
      DWORD multisampleQuality, BOOL lockable, IDirect3DSurface9** surface,
      HANDLE* sharedHandle, UINT views) {
    if (surface == nullptr || views != 2)
      return D3DERR_INVALIDCALL;

    *surface = nullptr;

    D3D9_COMMON_TEXTURE_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = 1;
    desc.ArraySize = views;
    desc.MipLevels = 1;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Format = EnumerateFormat(format);
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Discard = FALSE;
    desc.MultiSample = multiSample;
    desc.MultisampleQuality = multisampleQuality;
    desc.IsBackBuffer = FALSE;
    desc.IsAttachmentOnly = TRUE;
    // D3D9_COMMON_TEXTURE_DESC has no lockability field; match CreateRenderTargetEx.
    return m_device->CreateRenderTargetFromDesc(&desc, surface, sharedHandle);
  }
  HRESULT STDMETHODCALLTYPE CreateMultiViewDepthStencilSurface(
      UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample,
      DWORD multisampleQuality, BOOL discard, IDirect3DSurface9** surface,
      HANDLE* sharedHandle, UINT views) {
    if (surface == nullptr || views != 2)
      return D3DERR_INVALIDCALL;

    *surface = nullptr;
    D3D9_COMMON_TEXTURE_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = 1;
    desc.ArraySize = views;
    desc.MipLevels = 1;
    desc.Usage = D3DUSAGE_DEPTHSTENCIL;
    desc.Format = EnumerateFormat(format);
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Discard = discard != FALSE;
    desc.MultiSample = multiSample;
    desc.MultisampleQuality = multisampleQuality;
    desc.IsBackBuffer = FALSE;
    desc.IsAttachmentOnly = TRUE;
    return m_device->CreateRenderTargetFromDesc(&desc, surface, sharedHandle);
  }
  HRESULT STDMETHODCALLTYPE CopySurfaceLayers(IDirect3DSurface9* source, IDirect3DSurface9** destinations, UINT layerCount) {
    if (source == nullptr || destinations == nullptr || layerCount == 0)
      return D3DERR_INVALIDCALL;

    auto* src = static_cast<D3D9Surface*>(source);
    const UINT arraySize = src->GetCommonTexture()->Desc()->ArraySize;
    if (layerCount > arraySize)
      return D3DERR_INVALIDCALL;

    D3D9DeviceLock lock = m_device->LockDevice();
    const UINT originalLayer = src->GetFace();

    HRESULT result = D3D_OK;
    for (UINT layer = 0; layer < layerCount; layer++) {
      if (destinations[layer] == nullptr) {
        result = D3DERR_INVALIDCALL;
        break;
      }

      result = src->SetMultiviewSurfaceLayer(layer);
      if (FAILED(result))
        break;

      result = m_device->StretchRect(source, nullptr, destinations[layer], nullptr, D3DTEXF_NONE);
      if (FAILED(result))
        break;
    }

    const HRESULT restoreResult = src->SetMultiviewSurfaceLayer(originalLayer);
    return FAILED(result) ? result : restoreResult;
  }
  HRESULT STDMETHODCALLTYPE GetSPIRVShaderCode(IDirect3DVertexShader9* d3dShader, uint32_t* out, uint32_t* size) {
    if (d3dShader == nullptr || size == nullptr)
      return D3DERR_INVALIDCALL;

    auto* shader = reinterpret_cast<D3D9Shader<IDirect3DVertexShader9>*>(d3dShader);
    auto dxvkShader = shader->GetCommonShader()->GetShader(D3D9ShaderPermutations::None);
    if (dxvkShader == nullptr)
      return D3DERR_INVALIDCALL;

    const SpirvCodeBuffer code = dxvkShader->getRawCode();
    const uint32_t required = code.dwords();
    if (out == nullptr) {
      *size = required;
      return D3D_OK;
    }
    if (*size < required) {
      *size = required;
      return D3DERR_MOREDATA;
    }

    std::memcpy(out, code.data(), code.size());
    *size = required;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE GetShaderConstantCount(IDirect3DVertexShader9* d3dShader, uint32_t* out) {
    if (d3dShader == nullptr || out == nullptr)
      return D3DERR_INVALIDCALL;

    auto* shader = reinterpret_cast<D3D9Shader<IDirect3DVertexShader9>*>(d3dShader);
    *out = shader->GetCommonShader()->GetMeta().maxConstIndexF;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetShaderConstantCount(IDirect3DVertexShader9* d3dShader, uint32_t count) {
    if (d3dShader == nullptr)
      return D3DERR_INVALIDCALL;

    auto* shader = reinterpret_cast<D3D9Shader<IDirect3DVertexShader9>*>(d3dShader);
    shader->GetCommonShader_mut()->SetMaxFloatConstantIndex(count);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE EnableMultiView(bool enable) {
    D3D9DeviceLock lock = m_device->LockDevice();
    if (enable && (!m_device->GetDXVKDevice()->features().vulkan11Features.multiview
                || m_device->GetSoftwareVertexProcessing()))
      return D3DERR_NOTAVAILABLE;
    m_device->SetMultiViewFF(enable);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateTextureFromNTHandle(
      UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
      D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* sharedHandle) {
    if (sharedHandle == nullptr || *sharedHandle == nullptr || *sharedHandle == INVALID_HANDLE_VALUE)
      return D3DERR_INVALIDCALL;
    return m_device->CreateTextureWithHandleType(width, height, levels, usage, format, pool,
      texture, sharedHandle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT);
  }

  HRESULT STDMETHODCALLTYPE SetMultiviewSurfaceLayer(IDirect3DSurface9* surface, uint32_t layer) {
    if (surface == nullptr)
      return D3DERR_INVALIDCALL;
    D3D9DeviceLock lock = m_device->LockDevice();
    return static_cast<D3D9Surface*>(surface)->SetMultiviewSurfaceLayer(layer);
  }

  HRESULT STDMETHODCALLTYPE InitializeOpenVRScene() {
    if (m_openVrSceneSession)
      return D3D_OK;

    D3DDEVICE_CREATION_PARAMETERS params = {};
    const HRESULT paramsResult = m_device->GetCreationParameters(&params);
    if (FAILED(paramsResult))
      return paramsResult;

    if (!VrInstance::s_instance.initializeSceneSession(this, params.AdapterOrdinal, true))
      return D3DERR_NOTAVAILABLE;

    m_openVrSceneSession = true;
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE AcquireOpenVRFrame(D3D9_OPENVR_FRAME_STATE* frame) {
    if (frame == nullptr)
      return D3DERR_INVALIDCALL;
    if (!m_openVrSceneSession)
      return D3DERR_NOTAVAILABLE;

    auto device = m_device->GetDXVKDevice();
    device->lockSubmission();
    OpenVrFrameState snapshot = {};
    const bool acquired = VrInstance::s_instance.acquireFrame(snapshot);
    device->unlockSubmission();
    if (!acquired)
      return D3DERR_NOTAVAILABLE;

    const auto& pose = snapshot.renderPoses[vr::k_unTrackedDeviceIndex_Hmd];
    frame->Version = 1;
    frame->Flags = pose.bPoseIsValid ? 1u : 0u;
    frame->FrameIndex = snapshot.frameIndex;
    frame->RecommendedWidth = snapshot.recommendedWidth;
    frame->RecommendedHeight = snapshot.recommendedHeight;
    for (uint32_t row = 0; row < 3; row++) {
      for (uint32_t column = 0; column < 4; column++) {
        frame->DeviceToAbsoluteTracking[row * 4 + column] = pose.mDeviceToAbsoluteTracking.m[row][column];
        for (uint32_t eye = 0; eye < 2; eye++)
          frame->EyeToHead[eye][row * 4 + column] = snapshot.eyeToHead[eye].m[row][column];
      }
    }
    for (uint32_t eye = 0; eye < 2; eye++) {
      frame->ProjectionRaw[eye][0] = snapshot.projection[eye].left;
      frame->ProjectionRaw[eye][1] = snapshot.projection[eye].right;
      frame->ProjectionRaw[eye][2] = snapshot.projection[eye].top;
      frame->ProjectionRaw[eye][3] = snapshot.projection[eye].bottom;
    }
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE SubmitOpenVRFrame(IDirect3DSurface9* left, IDirect3DSurface9* right) {
    if (!left || !right || !m_openVrSceneSession)
      return D3DERR_INVALIDCALL;

    const HRESULT leftTransfer = TransferSurfaceForVR(left);
    if (FAILED(leftTransfer))
      return leftTransfer;

    const HRESULT rightTransfer = TransferSurfaceForVR(right);
    if (FAILED(rightTransfer)) {
      const HRESULT begin = BeginVRSubmit();
      if (SUCCEEDED(begin))
        EndVRSubmit();
      return rightTransfer;
    }

    D3D9_TEXTURE_VR_DESC descriptors[2] = {};
    HRESULT result = GetVRDesc(left, &descriptors[0]);
    if (SUCCEEDED(result))
      result = GetVRDesc(right, &descriptors[1]);
    if (FAILED(result)) {
      const HRESULT begin = BeginVRSubmit();
      if (SUCCEEDED(begin))
        EndVRSubmit();
      return result;
    }

    std::array<vr::VRVulkanTextureData_t, 2> textures = {};
    for (size_t i = 0; i < textures.size(); i++) {
      textures[i].m_nImage = descriptors[i].Image;
      textures[i].m_pDevice = reinterpret_cast<VkDevice_T*>(descriptors[i].Device);
      textures[i].m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(descriptors[i].PhysicalDevice);
      textures[i].m_pInstance = reinterpret_cast<VkInstance_T*>(descriptors[i].Instance);
      textures[i].m_pQueue = reinterpret_cast<VkQueue_T*>(descriptors[i].Queue);
      textures[i].m_nQueueFamilyIndex = descriptors[i].QueueFamilyIndex;
      textures[i].m_nWidth = descriptors[i].Width;
      textures[i].m_nHeight = descriptors[i].Height;
      textures[i].m_nFormat = static_cast<uint32_t>(descriptors[i].Format);
      textures[i].m_nSampleCount = descriptors[i].SampleCount;
    }

    const HRESULT beginResult = BeginVRSubmit();
    if (FAILED(beginResult))
      return beginResult;

    const bool submitted = VrInstance::s_instance.submitFrame(textures, vr::ColorSpace_Auto);
    const HRESULT endResult = EndVRSubmit();
    if (FAILED(endResult))
      return endResult;
    return submitted ? D3D_OK : D3DERR_NOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE ResetOpenVRView() {
    if (!m_openVrSceneSession)
      return D3DERR_NOTAVAILABLE;
    return VrInstance::s_instance.resetView() ? D3D_OK : D3DERR_NOTAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE GetOpenVRFrameTiming(D3D9_OPENVR_FRAME_TIMING* timing) {
    if (timing == nullptr)
      return D3DERR_INVALIDCALL;
    *timing = {};
    if (!m_openVrSceneSession)
      return D3DERR_NOTAVAILABLE;

    vr::Compositor_FrameTiming frameTiming = {};
    if (!VrInstance::s_instance.getFrameTiming(frameTiming))
      return D3DERR_NOTAVAILABLE;

    timing->GpuPreSubmit = frameTiming.m_flPreSubmitGpuMs;
    timing->GpuPostSubmit = frameTiming.m_flPostSubmitGpuMs;
    timing->CompositorGpu = frameTiming.m_flCompositorRenderGpuMs;
    timing->CompositorCpu = frameTiming.m_flCompositorRenderCpuMs;
    timing->CompositorSubmitFrame = frameTiming.m_flSubmitFrameMs;
    timing->GpuTotal = frameTiming.m_flTotalRenderGpuMs;
    timing->FrameInterval = frameTiming.m_flClientFrameIntervalMs;
    timing->CpuPresentCall = frameTiming.m_flPresentCallCpuMs;
    timing->CpuWaitForPresent = frameTiming.m_flWaitForPresentCpuMs;
    timing->ReprojectionFlags = frameTiming.m_nReprojectionFlags;
    timing->MispresentedFrames = frameTiming.m_nNumMisPresented;
    timing->DroppedFrames = frameTiming.m_nNumDroppedFrames;
    return D3D_OK;
  }

private:
  Com<D3D9DeviceEx, false> m_device;
  bool m_openVrSceneSession = false;
  D3D9DeviceLock m_deviceLock;
  std::atomic<bool> m_deviceLockHeld = { false };
  std::atomic<uint32_t> m_deviceLockThreadId = { 0 };
  D3D9DeviceLock m_vrSubmitDeviceLock;
  D3D9DeviceLock m_submissionQueueDeviceLock;
  std::atomic<uint32_t> m_submissionQueueThreadId = { 0 };
  std::atomic<bool> m_vrSubmitActive = { false };
  struct VrImageLayout {
    Rc<DxvkImage> image;
    VkImageLayout originalLayout;
    VkPipelineStageFlags originalStages;
    VkAccessFlags originalAccess;
  };

  std::vector<VrImageLayout> m_vrImages;
  Rc<DxvkFence> m_fence;
};

}

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9* device, IDirect3DVR9** interfaceOut) {
  if (device == nullptr || interfaceOut == nullptr)
    return D3DERR_INVALIDCALL;

  *interfaceOut = nullptr;
  *interfaceOut = dxvk::ref(new dxvk::D3D9VR(device));
  return *interfaceOut != nullptr ? D3D_OK : E_OUTOFMEMORY;
}
