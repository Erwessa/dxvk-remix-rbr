#pragma once

#include <cstdint>
#include <memory>

#include "rtx_option.h"

namespace dxvk {

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

    // Returns whether the latched frame holds a valid headset pose.
    bool isValid() const { return m_valid; }

    // Returns the latched frame. Only meaningful while isValid() returns true.
    const OpenVrFrameState& get() const { return *m_pFrame; }

  private:
    std::unique_ptr<OpenVrFrameState> m_pFrame;
    bool m_valid = false;
  };

}
