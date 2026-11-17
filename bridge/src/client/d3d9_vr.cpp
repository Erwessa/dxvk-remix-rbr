#include "pch.h"

#include "d3d9_device_base.h"
#include "d3d9_surface.h"
#include "d3d9_vertexshader.h"
#include "d3d9_util.h"
#include "d3d9vr.h"
#include "util_common.h"
#include "util_devicecommand.h"

#include <atomic>
#include <vector>

using namespace bridge_util;

namespace {

class Direct3DVR9_LSS final : public IDirect3DVR9 {
public:
  explicit Direct3DVR9_LSS(BaseDirect3DDevice9Ex_LSS* device)
  : m_device(device)
  , m_id(static_cast<uint32_t>(D3dBaseIdFactory::getNextId())) {
    m_device->AddRef();
  }

  HRESULT createRemote() {
    DeviceBridge::ResponseTransaction transaction;
    ClientMessage command(Commands::D3D9VR_Create, m_id);
    const auto uid = command.get_uid();
    command.send_data(static_cast<uint32_t>(m_device->getId()));

    WAIT_FOR_SERVER_RESPONSE("Direct3DCreateVR", E_FAIL, uid);
    const HRESULT result = static_cast<HRESULT>(DeviceBridge::get_data());
    DeviceBridge::pop_front();
    return result;
  }

  HRESULT invoke(D3D9VRCommand method) {
    DeviceBridge::ResponseTransaction transaction;
    ClientMessage command(Commands::D3D9VR_Invoke, m_id);
    const auto uid = command.get_uid();
    command.send_data(static_cast<uint32_t>(method));
    WAIT_FOR_SERVER_RESPONSE("IDirect3DVR9 method", E_FAIL, uid);
    const HRESULT result = static_cast<HRESULT>(DeviceBridge::get_data());
    DeviceBridge::pop_front();
    return result;
  }

  template<typename Writer>
  HRESULT invoke(D3D9VRCommand method, Writer&& writer) {
    DeviceBridge::ResponseTransaction transaction;
    ClientMessage command(Commands::D3D9VR_Invoke, m_id);
    const auto uid = command.get_uid();
    command.send_data(static_cast<uint32_t>(method));
    writer(command);
    WAIT_FOR_SERVER_RESPONSE("IDirect3DVR9 method", E_FAIL, uid);
    const HRESULT result = static_cast<HRESULT>(DeviceBridge::get_data());
    DeviceBridge::pop_front();
    return result;
  }

  HRESULT invokeFrame(D3D9VRCommand method, void* output, size_t outputSize) {
    DeviceBridge::ResponseTransaction transaction;
    ClientMessage command(Commands::D3D9VR_Invoke, m_id);
    const auto uid = command.get_uid();
    command.send_data(static_cast<uint32_t>(method));
    if (DeviceBridge::waitForCommand(Commands::Bridge_Response, GlobalOptions::getAckTimeout(), nullptr, true, uid) != Result::Success)
      return E_FAIL;

    const HRESULT result = static_cast<HRESULT>(DeviceBridge::get_data());
    if (SUCCEEDED(result)) {
      void* data = nullptr;
      const auto size = DeviceBridge::get_data(&data);
      if (size != outputSize || data == nullptr) {
        DeviceBridge::pop_front();
        return E_FAIL;
      }
      std::memcpy(output, data, outputSize);
    }
    DeviceBridge::pop_front();
    return result;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVR9)) {
      *object = static_cast<IDirect3DVR9*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return ++m_refCount;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = --m_refCount;
    if (!count) {
      ClientMessage(Commands::D3D9VR_Destroy, m_id);
      delete this;
    }
    return count;
  }

  HRESULT STDMETHODCALLTYPE GetVRDesc(IDirect3DSurface9*, D3D9_TEXTURE_VR_DESC*) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE TransferSurfaceForVR(IDirect3DSurface9*) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE BeginVRSubmit() override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE EndVRSubmit() override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE LockDevice() override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE UnlockDevice() override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE WaitDeviceIdle(BOOL flush) override {
    return invoke(D3D9VRCommand::WaitDeviceIdle, [&](ClientMessage& c) { c.send_data(static_cast<uint32_t>(flush)); });
  }
  HRESULT STDMETHODCALLTYPE WaitGraphicsQueueIdle(BOOL) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE GetOXRVkDeviceDesc(OXR_VK_DEVICE_DESC*) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE Flush() override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE LockSubmissionQueue() override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE ImportFence(HANDLE, uint64_t) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE SignalFence(uint64_t) override { return D3DERR_NOTAVAILABLE; }

  HRESULT STDMETHODCALLTYPE PatchSPIRVToVertexShader(IDirect3DVertexShader9* shader, const uint32_t* data, uint32_t size) override {
    if (!shader || !data || !size)
      return D3DERR_INVALIDCALL;
    if (size > 16u * 1024u * 1024u)
      return E_OUTOFMEMORY;
    auto* bridgeShader = bridge_cast<Direct3DVertexShader9_LSS*>(shader);
    if (!bridgeShader)
      return D3DERR_INVALIDCALL;
    return invoke(D3D9VRCommand::PatchSPIRVToVertexShader, [&](ClientMessage& c) {
      c.send_many(static_cast<uint32_t>(bridgeShader->getId()), size);
      c.send_data(size * sizeof(uint32_t), data);
    });
  }

  HRESULT STDMETHODCALLTYPE CreateMultiViewRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample, DWORD quality, BOOL lockable, IDirect3DSurface9** out, HANDLE* sharedHandle, UINT views) override {
    return createMultiViewSurface(width, height, format, multiSample, quality, lockable, out, sharedHandle, views, false);
  }

  HRESULT STDMETHODCALLTYPE CreateMultiViewDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample, DWORD quality, BOOL discard, IDirect3DSurface9** out, HANDLE* sharedHandle, UINT views) override {
    return createMultiViewSurface(width, height, format, multiSample, quality, discard, out, sharedHandle, views, true);
  }

  HRESULT STDMETHODCALLTYPE CopySurfaceLayers(IDirect3DSurface9* source, IDirect3DSurface9** destinations, UINT count) override {
    if (!source || !destinations || count == 0)
      return D3DERR_INVALIDCALL;
    auto* src = bridge_cast<Direct3DSurface9_LSS*>(source);
    if (!src)
      return D3DERR_INVALIDCALL;
    std::vector<uint32_t> ids;
    ids.reserve(count);
    for (UINT i = 0; i < count; i++) {
      auto* dst = bridge_cast<Direct3DSurface9_LSS*>(destinations[i]);
      if (!dst)
        return D3DERR_INVALIDCALL;
      ids.push_back(static_cast<uint32_t>(dst->getId()));
    }
    return invoke(D3D9VRCommand::CopySurfaceLayers, [&](ClientMessage& c) {
      c.send_many(static_cast<uint32_t>(src->getId()), count);
      for (auto id : ids)
        c.send_data(id);
    });
  }

  HRESULT STDMETHODCALLTYPE GetSPIRVShaderCode(IDirect3DVertexShader9* shader, uint32_t* out, uint32_t* size) override {
    if (!shader || !size)
      return D3DERR_INVALIDCALL;
    auto* bridgeShader = bridge_cast<Direct3DVertexShader9_LSS*>(shader);
    if (!bridgeShader)
      return D3DERR_INVALIDCALL;

    DeviceBridge::ResponseTransaction transaction;
    ClientMessage command(Commands::D3D9VR_Invoke, m_id);
    const auto uid = command.get_uid();
    command.send_many(static_cast<uint32_t>(D3D9VRCommand::GetSPIRVShaderCode),
                      static_cast<uint32_t>(bridgeShader->getId()), out ? *size : 0u);
    if (DeviceBridge::waitForCommand(Commands::Bridge_Response, GlobalOptions::getAckTimeout(), nullptr, true, uid) != Result::Success)
      return E_FAIL;
    const HRESULT result = static_cast<HRESULT>(DeviceBridge::get_data());
    const uint32_t required = DeviceBridge::get_data();
    if (required > 16u * 1024u * 1024u) {
      DeviceBridge::pop_front();
      return E_OUTOFMEMORY;
    }
    *size = required;
    if (SUCCEEDED(result) && out && required) {
      void* data = nullptr;
      const size_t bytes = DeviceBridge::get_data(&data);
      if (bytes != size_t(required) * sizeof(uint32_t) || !data) {
        DeviceBridge::pop_front();
        return E_FAIL;
      }
      std::memcpy(out, data, bytes);
    }
    DeviceBridge::pop_front();
    return result;
  }

  HRESULT STDMETHODCALLTYPE GetShaderConstantCount(IDirect3DVertexShader9* shader, uint32_t* out) override {
    if (!shader || !out)
      return D3DERR_INVALIDCALL;
    auto* bridgeShader = bridge_cast<Direct3DVertexShader9_LSS*>(shader);
    if (!bridgeShader)
      return D3DERR_INVALIDCALL;
    DeviceBridge::ResponseTransaction transaction;
    ClientMessage command(Commands::D3D9VR_Invoke, m_id);
    const auto uid = command.get_uid();
    command.send_many(static_cast<uint32_t>(D3D9VRCommand::GetShaderConstantCount),
                      static_cast<uint32_t>(bridgeShader->getId()));
    if (DeviceBridge::waitForCommand(Commands::Bridge_Response, GlobalOptions::getAckTimeout(), nullptr, true, uid) != Result::Success)
      return E_FAIL;
    const HRESULT result = static_cast<HRESULT>(DeviceBridge::get_data());
    if (SUCCEEDED(result))
      *out = DeviceBridge::get_data();
    DeviceBridge::pop_front();
    return result;
  }

  HRESULT STDMETHODCALLTYPE SetShaderConstantCount(IDirect3DVertexShader9* shader, uint32_t count) override {
    if (!shader)
      return D3DERR_INVALIDCALL;
    auto* bridgeShader = bridge_cast<Direct3DVertexShader9_LSS*>(shader);
    if (!bridgeShader)
      return D3DERR_INVALIDCALL;
    return invoke(D3D9VRCommand::SetShaderConstantCount, [&](ClientMessage& c) {
      c.send_many(static_cast<uint32_t>(bridgeShader->getId()), count);
    });
  }

  HRESULT STDMETHODCALLTYPE EnableMultiView(bool enable) override {
    return invoke(D3D9VRCommand::EnableMultiView, [&](ClientMessage& c) {
      c.send_data(static_cast<uint32_t>(enable));
    });
  }
  HRESULT STDMETHODCALLTYPE SetMultiviewSurfaceLayer(IDirect3DSurface9*, uint32_t) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE CreateTextureFromNTHandle(UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*) override { return D3DERR_NOTAVAILABLE; }

  HRESULT STDMETHODCALLTYPE GetShaderHash(IDirect3DVertexShader9* shader, char** out) override {
    if (!shader || !out)
      return D3DERR_INVALIDCALL;
    auto* bridgeShader = bridge_cast<Direct3DVertexShader9_LSS*>(shader);
    if (!bridgeShader)
      return D3DERR_INVALIDCALL;
    DeviceBridge::ResponseTransaction transaction;
    ClientMessage command(Commands::D3D9VR_Invoke, m_id);
    const auto uid = command.get_uid();
    command.send_many(static_cast<uint32_t>(D3D9VRCommand::GetShaderHash),
                      static_cast<uint32_t>(bridgeShader->getId()));
    if (DeviceBridge::waitForCommand(Commands::Bridge_Response, GlobalOptions::getAckTimeout(), nullptr, true, uid) != Result::Success)
      return E_FAIL;
    const HRESULT result = static_cast<HRESULT>(DeviceBridge::get_data());
    if (SUCCEEDED(result)) {
      void* data = nullptr;
      const size_t bytes = DeviceBridge::get_data(&data);
      if (bytes != 64 || !data) {
        DeviceBridge::pop_front();
        return E_FAIL;
      }
      std::memcpy(reinterpret_cast<char*>(out), data, bytes);
    }
    DeviceBridge::pop_front();
    return result;
  }

  HRESULT STDMETHODCALLTYPE InitializeOpenVRScene() override {
    return invoke(D3D9VRCommand::InitializeOpenVRScene);
  }

  HRESULT STDMETHODCALLTYPE AcquireOpenVRFrame(D3D9_OPENVR_FRAME_STATE* frame) override {
    if (!frame)
      return D3DERR_INVALIDCALL;
    *frame = {};
    return invokeFrame(D3D9VRCommand::AcquireOpenVRFrame, frame, sizeof(*frame));
  }

  HRESULT STDMETHODCALLTYPE SubmitOpenVRFrame(IDirect3DSurface9* left, IDirect3DSurface9* right) override {
    if (!left || !right)
      return D3DERR_INVALIDCALL;
    auto* leftSurface = bridge_cast<Direct3DSurface9_LSS*>(left);
    auto* rightSurface = bridge_cast<Direct3DSurface9_LSS*>(right);
    if (!leftSurface || !rightSurface)
      return D3DERR_INVALIDCALL;
    return invoke(D3D9VRCommand::SubmitOpenVRFrame, [&](ClientMessage& c) {
      c.send_many(static_cast<uint32_t>(leftSurface->getId()), static_cast<uint32_t>(rightSurface->getId()));
    });
  }

  HRESULT STDMETHODCALLTYPE ResetOpenVRView() override {
    return invoke(D3D9VRCommand::ResetOpenVRView);
  }

  HRESULT STDMETHODCALLTYPE GetOpenVRFrameTiming(D3D9_OPENVR_FRAME_TIMING* timing) override {
    if (!timing)
      return D3DERR_INVALIDCALL;
    *timing = {};
    return invokeFrame(D3D9VRCommand::GetOpenVRFrameTiming, timing, sizeof(*timing));
  }

private:
  HRESULT createMultiViewSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample,
      DWORD quality, BOOL flag, IDirect3DSurface9** out, HANDLE* sharedHandle, UINT views, bool depthStencil) {
    if (!out || width == 0 || height == 0 || views != 2)
      return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (sharedHandle)
      *sharedHandle = nullptr;

    D3DSURFACE_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.MultiSampleType = multiSample;
    desc.MultiSampleQuality = quality;
    desc.Usage = depthStencil ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Type = D3DRTYPE_SURFACE;

    auto* surface = new (std::nothrow) Direct3DSurface9_LSS(m_device, desc);
    if (!surface)
      return E_OUTOFMEMORY;
    trackWrapper(surface);
    const HRESULT result = invoke(depthStencil
        ? D3D9VRCommand::CreateMultiViewDepthStencilSurface
        : D3D9VRCommand::CreateMultiViewRenderTarget,
      [&](ClientMessage& c) {
        c.send_many(width, height, static_cast<uint32_t>(format), static_cast<uint32_t>(multiSample),
                    quality, static_cast<uint32_t>(flag), static_cast<uint32_t>(surface->getId()), views);
      });
    if (FAILED(result)) {
      surface->Release();
      return result;
    }
    *out = surface;
    return D3D_OK;
  }

  ~Direct3DVR9_LSS() {
    m_device->Release();
  }

  std::atomic<ULONG> m_refCount = 1;
  BaseDirect3DDevice9Ex_LSS* m_device;
  const uint32_t m_id;
};

}

extern "C" DLLEXPORT HRESULT WINAPI Direct3DCreateVR(
    IDirect3DDevice9* device,
    IDirect3DVR9** interfaceOut) {
  if (!device || !interfaceOut)
    return D3DERR_INVALIDCALL;
  *interfaceOut = nullptr;

  auto* bridgeDevice = bridge_cast<BaseDirect3DDevice9Ex_LSS*>(device);
  if (!bridgeDevice)
    return D3DERR_INVALIDCALL;

  auto* proxy = new (std::nothrow) Direct3DVR9_LSS(bridgeDevice);
  if (!proxy)
    return E_OUTOFMEMORY;

  const HRESULT result = proxy->createRemote();
  if (FAILED(result)) {
    proxy->Release();
    return result;
  }

  *interfaceOut = proxy;
  return D3D_OK;
}
