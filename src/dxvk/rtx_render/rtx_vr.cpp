#include "rtx_vr.h"

#include <cmath>

#include "rtx_imgui.h"
#include "rtx_vr_math.h"
#include "../dxvk_device.h"
#include "../dxvk_openvr.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

namespace dxvk {

  namespace {

    // Returns whether the projection maps the left and right planes to -1 and 1 for a left-handed camera.
    bool projectionMatchesBounds(const OpenVrProjectionBounds& bounds) {
      const float distance = 10.0f;
      const Matrix4 projection = projectionFromRawBounds(bounds.left, bounds.right, bounds.top, bounds.bottom, 0.1f, 1000.0f, true);
      const Vector4 left = projection * Vector4(bounds.left * distance, 0.0f, distance, 1.0f);
      const Vector4 right = projection * Vector4(bounds.right * distance, 0.0f, distance, 1.0f);
      return std::abs(left.x / left.w + 1.0f) < 1e-3f && std::abs(right.x / right.w - 1.0f) < 1e-3f;
    }

  }

  void RtxVr::showImguiSettings() {
    RemixGui::Checkbox("OpenVR Headset (restart required)", &enableObject());
  }

  RtxVrFrame::RtxVrFrame() : m_pFrame(std::make_unique<OpenVrFrameState>()) { }

  RtxVrFrame::~RtxVrFrame() = default;

  void RtxVrFrame::acquire(DxvkDevice& device) {
    m_valid = false;

    if (!VrInstance::s_instance.isSceneSessionActive()) {
      return;
    }

    // WaitGetPoses may use the Vulkan queue, so it runs inside the device's submission gate.
    device.lockSubmission();
    m_valid = VrInstance::s_instance.acquireFrame(*m_pFrame);
    device.unlockSubmission();

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

      if (!projectionMatchesBounds(frame.projection[0]) || !projectionMatchesBounds(frame.projection[1])) {
        Logger::warn("OpenVR: the projection built from the raw bounds does not map the side planes to the clip range.");
      }
    }
  }

}
