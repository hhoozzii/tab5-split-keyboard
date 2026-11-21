#include "key_layout.h"

namespace kb {

int KeyLayout::posOfIndex(int idx) const {
  const bool right = half_ == Half::Right;
  if (idx < 15) return (idx / 5) * 10 + (idx % 5) + (right ? 5 : 0);
  return 30 + (right ? 3 : 0) + (idx - 15);
}

Rect KeyLayout::statusRect() const {
  const bool left = half_ == Half::Left;
  const int pinky = left ? 10 : 14;                       // lowest key of the outer column
  const int outerThumb = left ? 15 : 17;
  const int top = cy_[pinky] + capH() / 2 + 14;
  // the outer thumb's rotated face may reach further out than its centre suggests
  const double hw = capW() / 2.0, hh = capH() / 2.0;
  const double ex = std::fabs(cos_[outerThumb]) * hw + std::fabs(sin_[outerThumb]) * hh;
  if (left) {
    const int x0 = clL_;
    const int x1 = cx_[outerThumb] - int(ex) - 24;
    return {x0, top, x1 - x0, h_ - m_ - top};
  }
  const int x0 = cx_[outerThumb] + int(ex) + 24;
  return {x0, top, clR_ - x0, h_ - m_ - top};
}

// A touch belongs to the key whose drawn (rotated) face contains it. Failing that, the key whose
// pitch-sized cell contains it; failing that, the nearest centre within 0.55 pitch.
int KeyLayout::hit(int x, int y) const {
  int best = -1;
  float bestD = 1e30f;
  for (int pass = 0; pass < 3 && best < 0; pass++) {
    const float hx = (pass == 0 ? capW() : px_) * 0.5f, hy = (pass == 0 ? capH() : py_) * 0.5f;
    for (int i = 0; i < kHalfKeys; i++) {
      const float dx = float(x - cx_[i]), dy = float(y - cy_[i]);
      const float nx = dx / px_, ny = dy / py_;
      const float d2 = nx * nx + ny * ny;                // distance in key pitches, squared
      if (pass < 2) {
        const float lx = dx * cos_[i] + dy * sin_[i];
        const float ly = -dx * sin_[i] + dy * cos_[i];
        if (lx < -hx || lx >= hx || ly < -hy || ly >= hy) continue;
      } else if (d2 > 0.3f) {
        continue;
      }
      if (d2 < bestD) { bestD = d2; best = i; }
    }
  }
  return best < 0 ? -1 : posOfIndex(best);
}

}  // namespace kb
