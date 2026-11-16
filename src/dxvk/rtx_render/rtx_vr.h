#pragma once

#include <cstdint>
#include <memory>

#include "rtx_option.h"

namespace dxvk {

  class CameraManager;
  class DxvkDevice;
  struct OpenVrFrameState;

  // Settings for rendering the scene on an OpenVR headset.
  // See docs/openvr-stereo-rendering.md.
  class RtxVr {
  public:
    RTX_OPTION("rtx.vr", bool, enable, false,
               "Claims an OpenVR scene session and prepares to render for a headset. "
               "SteamVR or another OpenVR runtime has to be installed. "
               "The setting is read once when the graphics device is created, so a change takes effect after "
               "the game restarts. When off, or when no headset runtime is found, rendering is unchanged.");

    RTX_OPTION("rtx.vr", bool, headTrackMainCamera, false,
               "Moves the game's main camera with the headset, so the head pose is visible in the game window. "
               "Takes effect from the second frame after the headset session starts.");

    static void showImguiSettings();
  };

  // The headset frame latched for the game frame being rendered.
  class RtxVrFrame {
  public:
    RtxVrFrame();
    ~RtxVrFrame();

    RtxVrFrame(const RtxVrFrame&) = delete;
    RtxVrFrame& operator=(const RtxVrFrame&) = delete;

    // Waits for the compositor and latches the poses and eye parameters for this game frame.
    // Does nothing when no OpenVR scene session is active.
    void acquire(DxvkDevice& device);

    // Updates both eye cameras once from the game's Main camera and the latched headset pose.
    // The Main camera has to be valid for the frame. The headset pose is applied on top of the game's camera, with
    // the headset's seated origin placed at the game camera. Does nothing when no headset frame is latched.
    void updateEyeCameras(CameraManager& cameraManager, uint32_t frameId);

    // Returns whether the latched frame holds a valid headset pose.
    bool isValid() const { return m_valid; }

    // Returns the runtime's recommended per-eye render size for this latched frame.
    // Returns false when no valid frame or no usable size is available.
    bool getRecommendedRenderTargetSize(uint32_t& width, uint32_t& height) const;

    // Returns the latched frame. Only meaningful while isValid() returns true.
    const OpenVrFrameState& get() const { return *m_pFrame; }

  private:
    std::unique_ptr<OpenVrFrameState> m_pFrame;
    bool m_valid = false;
  };

}
