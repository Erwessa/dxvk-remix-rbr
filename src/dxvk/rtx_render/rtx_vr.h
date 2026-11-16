#pragma once

#include "rtx_option.h"

namespace dxvk {

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

}
