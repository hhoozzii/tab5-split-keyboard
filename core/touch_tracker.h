// Turns per-frame touch point lists into finger down/move/up events.
// Does not rely on the controller's track IDs: fingers are matched frame to
// frame by nearest distance (greedy), so it works the same on any panel.
#pragma once
#include <cstdint>

namespace kb {

struct Pt { int16_t x, y; };
constexpr int kMaxFingers = 5;

struct FingerEvent {
  enum Type : uint8_t { Down, Move, Up } type;
  uint8_t slot;     // 0..kMaxFingers-1, stable while the finger stays down
  int16_t x, y;
};

class TouchTracker {
 public:
  // maxJumpPx: furthest a finger may travel between two frames and still be
  // the same finger. Poll fast (<= 10 ms) and this is very generous.
  explicit TouchTracker(int maxJumpPx = 200) : maxJump2_(maxJumpPx * maxJumpPx) {}

  // pts/n: points reported this frame (extra points beyond kMaxFingers ignored).
  // Writes events to out (Ups first, then Moves, then Downs); returns the count.
  // outCap should be >= 2 * kMaxFingers.
  int update(const Pt* pts, int n, FingerEvent* out, int outCap);

  int activeCount() const;

 private:
  struct Slot { bool active = false; int16_t x = 0, y = 0; };
  Slot s_[kMaxFingers];
  int maxJump2_;
};

}  // namespace kb
