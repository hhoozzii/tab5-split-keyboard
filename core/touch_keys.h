// Touch frames -> key down/up events. Each finger latches to the key it landed
// on and releases that key when it lifts, however far it drags.
#pragma once
#include <cstdint>
#include <functional>
#include "key_layout.h"
#include "touch_tracker.h"

namespace kb {

class TouchKeys {
 public:
  using KeyFn = std::function<void(uint8_t pos, bool down)>;
  TouchKeys(const KeyLayout& layout, KeyFn fn) : layout_(layout), fn_(std::move(fn)) {
    for (auto& l : latch_) l = -1;
  }

  // Call once per poll with whatever points the panel reports (n may be 0).
  void frame(const Pt* pts, int n) {
    FingerEvent ev[2 * kMaxFingers];
    int ne = tracker_.update(pts, n, ev, 2 * kMaxFingers);
    for (int i = 0; i < ne; i++) {
      const FingerEvent& e = ev[i];
      if (e.type == FingerEvent::Down) {
        latch_[e.slot] = int8_t(layout_.hit(e.x, e.y));
        if (latch_[e.slot] >= 0) fn_(uint8_t(latch_[e.slot]), true);
      } else if (e.type == FingerEvent::Up) {
        if (latch_[e.slot] >= 0) fn_(uint8_t(latch_[e.slot]), false);
        latch_[e.slot] = -1;
      }
    }
  }

  int fingers() const { return tracker_.activeCount(); }

 private:
  const KeyLayout& layout_;
  KeyFn fn_;
  TouchTracker tracker_;
  int8_t latch_[kMaxFingers];
};

}  // namespace kb
