// Decides when to turn the landscape UI by 180 degrees from the accelerometer.
// Input: gravity along the panel's short axis (raw panel +x), in milli-g. In landscape that axis is
// "up/down": a clearly positive value means panel +x points down (Rot::Deg90 is upright),
// a clearly negative value means it points up (Rot::Deg270 is upright).
// Values near zero (device lying flat) and short wobbles never change anything.
#pragma once
#include <cstdint>
#include "orientation.h"

namespace kb {

class AutoRotate {
 public:
  explicit AutoRotate(Rot initial, int thresholdMg = 600, uint32_t settleMs = 600)
      : cur_(initial), thr_(thresholdMg), settle_(settleMs) {}

  // Feed one sample. Returns true when rot() just changed.
  bool update(int gravityMg, uint32_t nowMs) {
    const Rot want = gravityMg > 0 ? Rot::Deg90 : Rot::Deg270;
    const int mag = gravityMg < 0 ? -gravityMg : gravityMg;
    if (mag < thr_ || want == cur_) { pending_ = false; return false; }
    if (!pending_ || pendWant_ != want) { pending_ = true; pendWant_ = want; since_ = nowMs; return false; }
    if (nowMs - since_ >= settle_) { cur_ = want; pending_ = false; return true; }
    return false;
  }
  Rot rot() const { return cur_; }

 private:
  Rot cur_, pendWant_ = Rot::Deg270;
  int thr_;
  uint32_t settle_, since_ = 0;
  bool pending_ = false;
};

}  // namespace kb
