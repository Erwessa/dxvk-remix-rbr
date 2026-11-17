#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"
#include "d3d9_include.h"
#include "d3d9_surface.h"
#include "d3d9_device.h"

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
    const VkResult result = device->vkd()->vkQueueWaitIdle(device->queues().graphics.queueHandle);
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
  HRESULT STDMETHODCALLTYPE GetShaderHash(IDirect3DVertexShader9*, char**) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PatchSPIRVToVertexShader(IDirect3DVertexShader9*, const uint32_t*, uint32_t) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateMultiViewRenderTarget(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*, UINT) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateMultiViewDepthStencilSurface(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*, UINT) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CopySurfaceLayers(IDirect3DSurface9*, IDirect3DSurface9**, UINT) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSPIRVShaderCode(IDirect3DVertexShader9*, uint32_t*, uint32_t*) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetShaderConstantCount(IDirect3DVertexShader9*, uint32_t*) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetShaderConstantCount(IDirect3DVertexShader9*, uint32_t) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnableMultiView(bool) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetMultiviewSurfaceLayer(IDirect3DSurface9*, uint32_t) { return E_NOTIMPL; }

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
