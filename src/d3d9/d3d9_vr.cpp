#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"
#include "d3d9_include.h"
#include "d3d9_surface.h"
#include "d3d9_device.h"
#include "d3d9_shader.h"

namespace dxvk {

class D3D9VR final : public ComObjectClamp<IDirect3DVR9> {
public:
  explicit D3D9VR(IDirect3DDevice9* device)
  : m_device(static_cast<D3D9DeviceEx*>(device)) { }

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

    auto* d3dSurface = static_cast<D3D9Surface*>(surface);
    auto* texture = d3dSurface->GetCommonTexture();
    const auto& image = texture->GetImage();
    if (image == nullptr)
      return D3DERR_INVALIDCALL;

    auto device = texture->Device()->GetDXVKDevice();
    const auto* textureDesc = texture->Desc();

    desc->Image = uint64_t(image->handle());
    desc->Device = device->handle();
    desc->PhysicalDevice = device->adapter()->handle();
    desc->Instance = device->instance()->handle();
    desc->Queue = device->queues().graphics.queueHandle;
    desc->QueueFamilyIndex = device->queues().graphics.queueFamily;
    desc->Width = textureDesc->Width;
    desc->Height = textureDesc->Height;
    desc->Format = texture->GetFormatMapping().FormatColor;
    desc->SampleCount = uint32_t(image->info().sampleCount);
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE TransferSurfaceForVR(IDirect3DSurface9* surface) {
    if (surface == nullptr)
      return D3DERR_INVALIDCALL;

    auto* texture = static_cast<D3D9Surface*>(surface)->GetCommonTexture();
    const auto& image = texture->GetImage();
    if (image == nullptr)
      return D3DERR_INVALIDCALL;

    const VkImageSubresourceRange range = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0, image->info().mipLevels, 0, image->info().numLayers
    };
    m_device->TransformImage(texture, &range, image->info().layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE BeginVRSubmit() {
    m_device->SynchronizeCsThread();
    m_device->GetDXVKDevice()->lockSubmission();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE EndVRSubmit() {
    m_device->GetDXVKDevice()->unlockSubmission();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE LockDevice() {
    if (m_deviceLockHeld)
      return D3DERR_INVALIDCALL;
    m_deviceLock = m_device->LockDevice();
    m_deviceLockHeld = true;
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE UnlockDevice() {
    if (!m_deviceLockHeld)
      return D3DERR_INVALIDCALL;
    m_deviceLock = D3D9DeviceLock();
    m_deviceLockHeld = false;
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE WaitDeviceIdle(BOOL flush) {
    if (flush) {
      m_device->Flush();
      m_device->SynchronizeCsThread();
    }
    m_device->GetDXVKDevice()->waitForIdle();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE WaitGraphicsQueueIdle(BOOL flush) {
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
    m_device->GetDXVKDevice()->lockSubmission();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() {
    m_device->GetDXVKDevice()->unlockSubmission();
    return D3D_OK;
  }

  // These features require Remix-side fence interop, shader replacement and
  // multiview support. Fail explicitly rather than reporting false success.
  HRESULT STDMETHODCALLTYPE ImportFence(HANDLE, uint64_t) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SignalFence(uint64_t) { return E_NOTIMPL; }
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
  HRESULT STDMETHODCALLTYPE PatchSPIRVToVertexShader(IDirect3DVertexShader9*, const uint32_t*, uint32_t) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateMultiViewRenderTarget(
      UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample,
      DWORD multisampleQuality, BOOL lockable, IDirect3DSurface9** surface,
      HANDLE* sharedHandle, UINT views) {
    (void) lockable;
    if (surface == nullptr || views < 2)
      return D3DERR_INVALIDCALL;

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
    return m_device->CreateRenderTargetFromDesc(&desc, surface, sharedHandle);
  }
  HRESULT STDMETHODCALLTYPE CreateMultiViewDepthStencilSurface(
      UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample,
      DWORD multisampleQuality, BOOL discard, IDirect3DSurface9** surface,
      HANDLE* sharedHandle, UINT views) {
    if (surface == nullptr || views < 2)
      return D3DERR_INVALIDCALL;

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
    m_device->SetMultiViewFF(enable);
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetMultiviewSurfaceLayer(IDirect3DSurface9* surface, uint32_t layer) {
    if (surface == nullptr)
      return D3DERR_INVALIDCALL;
    return static_cast<D3D9Surface*>(surface)->SetMultiviewSurfaceLayer(layer);
  }

private:
  D3D9DeviceEx* m_device;
  D3D9DeviceLock m_deviceLock;
  bool m_deviceLockHeld = false;
};

}

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9* device, IDirect3DVR9** interfaceOut) {
  if (device == nullptr || interfaceOut == nullptr)
    return D3DERR_INVALIDCALL;

  *interfaceOut = nullptr;
  *interfaceOut = new dxvk::D3D9VR(device);
  return *interfaceOut != nullptr ? D3D_OK : E_OUTOFMEMORY;
}
