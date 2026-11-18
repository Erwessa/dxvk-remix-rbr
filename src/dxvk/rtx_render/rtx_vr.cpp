#include "rtx_vr.h"

#include <cmath>

#include "rtx_camera_manager.h"
#include "rtx_imgui.h"
#include "rtx_options.h"
#include "rtx_vr_math.h"
#include "../dxvk_device.h"
#include "../dxvk_openvr.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"
#include "../../util/util_defer.h"

namespace dxvk {

  namespace {

    // Converts an OpenVR transform in metres to the game's units and handedness.
    Matrix4 toGameBasis(const OpenVrMatrix4& source, float unitsPerMeter, bool isLHS) {
      Matrix4 result = matrixFromRowMajor(source.elements);
      for (std::size_t row = 0; row < 3; row++) {
        result[3][row] *= unitsPerMeter;
      }
      return isLHS ? flipHandedness(result) : result;
    }

    // Checks all four frustum planes for either camera handedness.
    bool projectionMatchesBounds(const OpenVrProjectionBounds& bounds, bool isLHS) {
      const float distance = 10.0f;
      const float forwardSign = isLHS ? 1.0f : -1.0f;
      const float upperTangent = -bounds.top;
      const float lowerTangent = -bounds.bottom;
      const Matrix4 projection = projectionFromRawBounds(
        bounds.left, bounds.right, bounds.top, bounds.bottom, 0.1f, 1000.0f, isLHS);

      const Vector4 left = projection * Vector4(bounds.left * distance, 0.0f, forwardSign * distance, 1.0f);
      const Vector4 right = projection * Vector4(bounds.right * distance, 0.0f, forwardSign * distance, 1.0f);
      const Vector4 top = projection * Vector4(0.0f, upperTangent * distance, forwardSign * distance, 1.0f);
      const Vector4 bottom = projection * Vector4(0.0f, lowerTangent * distance, forwardSign * distance, 1.0f);

      return std::abs(left.x / left.w + 1.0f) < 1e-3f
          && std::abs(right.x / right.w - 1.0f) < 1e-3f
          && std::abs(top.y / top.w - 1.0f) < 1e-3f
          && std::abs(bottom.y / bottom.w + 1.0f) < 1e-3f;
    }

  }

  void RtxVr::showImguiSettings() {
    RemixGui::Checkbox("OpenVR Headset (restart required)", &enableObject());
    RemixGui::Checkbox("Headset Moves Main Camera", &headTrackMainCameraObject());
  }

  RtxVrFrame::RtxVrFrame() : m_pFrame(std::make_unique<OpenVrFrameState>()) { }

  RtxVrFrame::~RtxVrFrame() = default;

  void RtxVrFrame::acquire(DxvkDevice& device) {
    m_valid = false;

    // A D3D9 plugin may own the shared OpenVR scene session independently.
    // Do not activate RTX stereo rendering or submit a second eye pair unless
    // the RTX VR option itself was explicitly enabled.
    if (!RtxVr::enable() || !VrInstance::s_instance.isSceneSessionActive()) {
      return;
    }

    // WaitGetPoses may use the Vulkan queue, so it runs inside the device's submission gate.
    // Keep the gate exception-safe and release it before processing or logging the latched frame.
    {
      device.lockSubmission();
      DEFER(unlockSubmission, device.unlockSubmission(););
      m_valid = VrInstance::s_instance.acquireFrame(*m_pFrame);
    }

    if (!m_valid) {
      return;
    }

    const OpenVrFrameState& frame = *m_pFrame;
    if (frame.frameIndex == 1 || frame.frameIndex % 600 == 0) {
      Logger::info(str::format(
        "OpenVR frame ", frame.frameIndex, ": recommended eye size ", frame.recommendedWidth, "x", frame.recommendedHeight,
        ", head position (", frame.trackingFromHead.at(0, 3), ", ", frame.trackingFromHead.at(1, 3), ", ", frame.trackingFromHead.at(2, 3), ")"));
    }

    if (frame.frameIndex == 1) {
      Logger::info(str::format(
        "OpenVR eye projection bounds, left eye: ", frame.projection[0].left, " ", frame.projection[0].right, " ",
        frame.projection[0].top, " ", frame.projection[0].bottom, ", right eye: ", frame.projection[1].left, " ",
        frame.projection[1].right, " ", frame.projection[1].top, " ", frame.projection[1].bottom));

      for (uint32_t eye = 0; eye < RtxEye::Count; eye++) {
        if (!projectionMatchesBounds(frame.projection[eye], true)
            || !projectionMatchesBounds(frame.projection[eye], false)) {
          Logger::warn("OpenVR: the projection built from the raw bounds does not map all frustum planes to the clip range.");
          break;
        }
      }
    }
  }

  bool RtxVrFrame::getRecommendedRenderTargetSize(uint32_t& width, uint32_t& height) const {
    if (!m_valid || m_pFrame->recommendedWidth == 0 || m_pFrame->recommendedHeight == 0) {
      return false;
    }

    width = m_pFrame->recommendedWidth;
    height = m_pFrame->recommendedHeight;
    return true;
  }

  void RtxVrFrame::updateEyeCameras(CameraManager& cameraManager, uint32_t frameId) {
    if (!m_valid) {
      // Do not keep applying the last headset pose after a failed pose acquisition or session shutdown.
      cameraManager.clearVrHeadPose();
      return;
    }

    const OpenVrFrameState& frame = *m_pFrame;
    const RtCamera& mainCamera = cameraManager.getMainCamera();
    const bool isLHS = mainCamera.isLHS();
    const float unitsPerMeter = RtxOptions::getMeterToWorldUnitScale();
    Matrix4 gameWorldToView = mainCamera.getWorldToViewf(false);
    cameraManager.getGameWorldToView(gameWorldToView);
    const Matrix4 gameViewToWorld = inverse(gameWorldToView);
    const Matrix4 headToSeat = toGameBasis(frame.trackingFromHead, unitsPerMeter, isLHS);

    if (RtxVr::headTrackMainCamera()) {
      cameraManager.setVrHeadPose(headToSeat, frameId);
    } else {
      cameraManager.clearVrHeadPose();
    }

    Vector3 eyePositions[RtxEye::Count];
    for (uint32_t eye = 0; eye < RtxEye::Count; eye++) {
      const OpenVrProjectionBounds& bounds = frame.projection[eye];
      const Matrix4 eyeToHead = toGameBasis(OpenVrMatrix4::fromHmdMatrix34(frame.eyeToHead[eye]), unitsPerMeter, isLHS);
      const Matrix4 eyeViewToWorld = gameViewToWorld * headToSeat * eyeToHead;
      const Matrix4 projection = projectionFromRawBounds(
        bounds.left, bounds.right, bounds.top, bounds.bottom, mainCamera.getNearPlane(), mainCamera.getFarPlane(), isLHS);

      // The free camera belongs to the Main camera, so the eye cameras skip its update.
      // The raw vertical bounds are flipped, so the upper tangent is -top and the lower tangent is -bottom.
      const float fov = std::atan(bounds.bottom) - std::atan(bounds.top);
      const float aspectRatio = (bounds.right - bounds.left) / (bounds.bottom - bounds.top);

      cameraManager.getVrEyeCamera(static_cast<RtxEye::Enum>(eye)).update(
        frameId, inverse(eyeViewToWorld), projection, fov, aspectRatio,
        mainCamera.getNearPlane(), mainCamera.getFarPlane(), isLHS,
        RtCamera::IncrementShakingFrame | RtCamera::UpdateJitterFrame);

      eyePositions[eye] = Vector3(eyeViewToWorld[3].x, eyeViewToWorld[3].y, eyeViewToWorld[3].z);
    }

    if (frame.frameIndex == 1 || frame.frameIndex % 600 == 0) {
      const Vector3 separation = eyePositions[RtxEye::Right] - eyePositions[RtxEye::Left];
      Logger::info(str::format(
        "OpenVR eye separation ", length(separation) / unitsPerMeter, " m (an adult's is about 0.063 m), game units per metre ",
        unitsPerMeter, ", game camera handedness ", isLHS ? "left" : "right"));
    }
  }

}
