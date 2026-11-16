#include "rtx_vr.h"

#include "rtx_imgui.h"

namespace dxvk {

  void RtxVr::showImguiSettings() {
    RemixGui::Checkbox("OpenVR Headset (restart required)", &enableObject());
  }

}
