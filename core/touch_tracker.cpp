#include "touch_tracker.h"

namespace kb {

int TouchTracker::activeCount() const {
  int c = 0;
  for (const Slot& s : s_) c += s.active;
  return c;
}

int TouchTracker::update(const Pt* pts, int n, FingerEvent* out, int outCap) {
  if (n > kMaxFingers) n = kMaxFingers;
  if (n < 0) n = 0;

  struct Pair { int d2; int8_t slot, pt; };
  Pair pairs[kMaxFingers * kMaxFingers];
  int np = 0;
  for (int i = 0; i < kMaxFingers; i++) {
    if (!s_[i].active) continue;
    for (int j = 0; j < n; j++) {
      int dx = pts[j].x - s_[i].x, dy = pts[j].y - s_[i].y;
      int d2 = dx * dx + dy * dy;
      if (d2 <= maxJump2_) pairs[np++] = {d2, int8_t(i), int8_t(j)};
    }
  }
  // insertion sort by distance (at most 25 pairs)
  for (int a = 1; a < np; a++) {
    Pair p = pairs[a];
    int b = a - 1;
    while (b >= 0 && pairs[b].d2 > p.d2) { pairs[b + 1] = pairs[b]; b--; }
    pairs[b + 1] = p;
  }

  bool slotTaken[kMaxFingers] = {false};
  bool ptTaken[kMaxFingers] = {false};
  int8_t ptSlot[kMaxFingers];
  for (int j = 0; j < kMaxFingers; j++) ptSlot[j] = -1;
  for (int a = 0; a < np; a++) {
    if (slotTaken[pairs[a].slot] || ptTaken[pairs[a].pt]) continue;
    slotTaken[pairs[a].slot] = true;
    ptTaken[pairs[a].pt] = true;
    ptSlot[pairs[a].pt] = pairs[a].slot;
  }

  int ne = 0;
  auto emit = [&](FingerEvent::Type t, int slot, int16_t x, int16_t y) {
    if (ne < outCap) out[ne++] = {t, uint8_t(slot), x, y};
  };

  // 1. Ups: active fingers nobody matched
  for (int i = 0; i < kMaxFingers; i++) {
    if (s_[i].active && !slotTaken[i]) {
      emit(FingerEvent::Up, i, s_[i].x, s_[i].y);
      s_[i].active = false;
    }
  }
  // 2. Moves
  for (int j = 0; j < n; j++) {
    int i = ptSlot[j];
    if (i < 0) continue;
    if (pts[j].x != s_[i].x || pts[j].y != s_[i].y) {
      s_[i].x = pts[j].x;
      s_[i].y = pts[j].y;
      emit(FingerEvent::Move, i, s_[i].x, s_[i].y);
    }
  }
  // 3. Downs: unmatched points take a free slot
  for (int j = 0; j < n; j++) {
    if (ptSlot[j] >= 0) continue;
    for (int i = 0; i < kMaxFingers; i++) {
      if (!s_[i].active) {
        s_[i].active = true;
        s_[i].x = pts[j].x;
        s_[i].y = pts[j].y;
        emit(FingerEvent::Down, i, s_[i].x, s_[i].y);
        break;
      }
    }
  }
  return ne;
}

}  // namespace kb
