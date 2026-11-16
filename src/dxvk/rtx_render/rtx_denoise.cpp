/*
* Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "rtx_denoise.h"
#include "dxvk_device.h"
#include "rtx_nrd_context.h"
#include "rtx/pass/nrd_args.h"
#include <cassert>

namespace dxvk {
  
  DxvkDenoise::DxvkDenoise(DxvkDevice* device, DenoiserType type)
  : CommonDeviceObject(device) {
    for (auto& context : m_nrdContexts)
      context = std::make_unique<NRDContext>(device, type);
  }

  DxvkDenoise::~DxvkDenoise() {
  }

  void DxvkDenoise::onDestroy() {
    for (auto& context : m_nrdContexts)
      context->onDestroy();
  }

  void DxvkDenoise::dispatch(
    Rc<DxvkContext> ctx,
    DxvkBarrierSet& barriers,
    const Resources::RaytracingOutput& rtOutput,
    const Input& inputs,
    Output& outputs,
    uint32_t eyeIndex) 
  {
    const SceneManager& sceneManager = device()->getCommon()->getSceneManager();
    const CameraManager& cameraManager = sceneManager.getCameraManager();

    assert(eyeIndex < m_nrdContexts.size());
    const RtCamera& renderCamera = cameraManager.areVrEyeCamerasValid(device()->getCurrentFrameId())
      ? cameraManager.getVrEyeCamera(static_cast<RtxEye::Enum>(eyeIndex))
      : cameraManager.getMainCamera();
    m_nrdContexts[eyeIndex]->dispatch(ctx, barriers, sceneManager, renderCamera, rtOutput, inputs, outputs);
  }

  void DxvkDenoise::releaseResources() {
    for (auto& context : m_nrdContexts)
      context->release();
  }

  void DxvkDenoise::copyNrdSettingsFrom(
    const DxvkDenoise& refDenoiser, uint32_t eyeIndex) {
    assert(eyeIndex < m_nrdContexts.size());
    m_nrdContexts[eyeIndex]->setNrdSettings(refDenoiser.m_nrdContexts[eyeIndex]->getNrdSettings());
  }

  void DxvkDenoise::showImguiSettings() {
    m_nrdContexts[0]->showImguiSettings();
    m_nrdContexts[1]->copyConfigurationFrom(m_nrdContexts[0]->getNrdSettings());
  }

  NrdArgs DxvkDenoise::getNrdArgs(uint32_t eyeIndex) const {
    assert(eyeIndex < m_nrdContexts.size());
    return m_nrdContexts[eyeIndex]->getNrdArgs();
  }

  Resources::Resource DxvkDenoise::getNrdValidationTexture(uint32_t eyeIndex) const {
    assert(eyeIndex < m_nrdContexts.size());
    return m_nrdContexts[eyeIndex]->getValidationTexture();
  }

  bool DxvkDenoise::isReferenceDenoiserEnabled(uint32_t eyeIndex) const {
    assert(eyeIndex < m_nrdContexts.size());
    return m_nrdContexts[eyeIndex]->isReferenceDenoiserEnabled();
  }
}
