// Panel <-> logical coordinates when the UI is drawn in landscape on the Tab5's portrait panel.
//
// The ST7123 panel is natively 720 x 1280 (portrait). The UI is a logical 1280 x 720 landscape
// image that LVGL rotates onto the panel (lv_display_set_rotation, clockwise). Our own touch
// reading gives raw panel coordinates, so map them back to logical ones here.
#pragma once

namespace kb {

enum class Rot { Deg90, Deg270 };   // the LV_DISPLAY_ROTATION_90 / _270 value passed to bsp_display_rotate

constexpr int kPanelW = 720, kPanelH = 1280;

// Raw panel point -> logical landscape point.
inline void panelToLogical(Rot r, int px, int py, int& lx, int& ly) {
  // Measured on the Tab5 (ST7123 + bsp_display_rotate): with LV_DISPLAY_ROTATION_270 the logical
  // top-left corner lands at the panel's top-right, so the picture is the opposite way round
  // from what the "clockwise" naming suggests. Touch must follow the picture.
  if (r == Rot::Deg270) {           // logical top-left = panel top-right
    lx = py;
    ly = kPanelW - 1 - px;
  } else {                          // Deg90: logical top-left = panel bottom-left
    lx = kPanelH - 1 - py;
    ly = px;
  }
}

}  // namespace kb
