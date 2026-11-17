#include "pch.h"

#include "d3d9_device_base.h"
#include "d3d9_surface.h"
#include "d3d9vr.h"
#include "util_common.h"
#include "util_devicecommand.h"

#include <atomic>

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
  HRESULT STDMETHODCALLTYPE GetShaderHash(IDirect3DVertexShader9*, char**) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE PatchSPIRVToVertexShader(IDirect3DVertexShader9*, const uint32_t*, uint32_t) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE CreateMultiViewRenderTarget(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*, UINT) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE CreateMultiViewDepthStencilSurface(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*, UINT) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE CopySurfaceLayers(IDirect3DSurface9*, IDirect3DSurface9**, UINT) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE GetSPIRVShaderCode(IDirect3DVertexShader9*, uint32_t*, uint32_t*) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE GetShaderConstantCount(IDirect3DVertexShader9*, uint32_t*) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE SetShaderConstantCount(IDirect3DVertexShader9*, uint32_t) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE EnableMultiView(bool) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE SetMultiviewSurfaceLayer(IDirect3DSurface9*, uint32_t) override { return D3DERR_NOTAVAILABLE; }
  HRESULT STDMETHODCALLTYPE CreateTextureFromNTHandle(UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*) override { return D3DERR_NOTAVAILABLE; }

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
