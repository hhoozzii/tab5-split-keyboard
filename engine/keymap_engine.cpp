#include "keymap_engine.h"

#include <algorithm>
#include <cstring>

namespace kb {

bool HidReport::operator==(const HidReport& o) const {
  return mods == o.mods && consumer == o.consumer && std::memcmp(keys, o.keys, sizeof keys) == 0;
}

Engine::Engine(const Keymap& km, EngineConfig cfg) : km_(km), cfg_(cfg) {}

void Engine::keyEvent(uint8_t pos, bool down, uint32_t now) {
  if (pos >= kNumKeys) return;
  tick(now);  // a hold decision due before this event happens first
  if (down) press(pos, now); else release(pos, now);
}

void Engine::tick(uint32_t now) {
  if (undecided_ >= 0 && uint32_t(now - undecidedAt_) >= cfg_.tappingTermMs) {
    decide(uint8_t(undecided_), true);
  }
}

bool Engine::nextDeadline(uint32_t& at) const {
  if (undecided_ < 0) return false;
  at = undecidedAt_ + cfg_.tappingTermMs;
  return true;
}

void Engine::press(uint8_t pos, uint32_t now) {
  KeyState& ks = keys_[pos];
  if (ks.down) return;
  ks.down = true;

  // tap-preferred: any other key press while a hold-tap is pending means "tap".
  if (undecided_ >= 0) decide(uint8_t(undecided_), false);

  ks.b = km_.at(layer_, pos);  // latched until release
  switch (ks.b.kind) {
    case Kind::None:
      ks.phase = Phase::Plain;
      break;
    case Kind::Key:
      ks.phase = Phase::Plain;
      pushHeld(pos, ks.b.a);
      break;
    case Kind::MT:
    case Kind::LT:
      ks.phase = Phase::Undecided;
      undecided_ = pos;
      undecidedAt_ = now;
      break;
    case Kind::To:
      ks.phase = Phase::Plain;
      if (guardPos_ == pos && uint32_t(now - guardReleasedAt_) < cfg_.doubleTapTermMs) {
        guardPos_ = -1;
        jumpToLayer(uint8_t(ks.b.a));
      }
      break;
    case Kind::Special:
      ks.phase = Phase::Plain;
      if (specialFn_) specialFn_(Special(ks.b.a), true);
      break;
  }
}

void Engine::release(uint8_t pos, uint32_t now) {
  KeyState& ks = keys_[pos];
  if (!ks.down) return;
  ks.down = false;

  switch (ks.phase) {
    case Phase::Undecided:  // released inside the term: tap
      decide(pos, false);
      dropHeld(pos);
      break;
    case Phase::Tap:
      dropHeld(pos);
      break;
    case Phase::Hold:
      if (ks.b.kind == Kind::MT) {
        dropHeld(pos);
      } else if (ks.b.kind == Kind::LT && ks.layerLive) {
        if (momentary_[ks.b.a] > 0) momentary_[ks.b.a]--;
        ks.layerLive = false;
        recomputeLayer();
      }
      break;
    case Phase::Plain:
      if (ks.b.kind == Kind::Key) {
        dropHeld(pos);
      } else if (ks.b.kind == Kind::Special) {
        if (specialFn_) specialFn_(Special(ks.b.a), false);
      } else if (ks.b.kind == Kind::To) {
        guardPos_ = pos;
        guardReleasedAt_ = now;
      }
      break;
    case Phase::Idle:
      break;
  }
  ks.phase = Phase::Idle;
}

void Engine::decide(uint8_t pos, bool hold) {
  KeyState& ks = keys_[pos];
  undecided_ = -1;
  if (hold) {
    ks.phase = Phase::Hold;
    if (ks.b.kind == Kind::MT) {
      pushHeld(pos, ks.b.a);
    } else {
      momentary_[ks.b.a]++;
      ks.layerLive = true;
      recomputeLayer();
    }
  } else {
    ks.phase = Phase::Tap;
    pushHeld(pos, ks.b.b);
  }
}

void Engine::jumpToLayer(uint8_t layer) {
  if (layer >= km_.numLayers) return;
  std::memset(momentary_, 0, sizeof momentary_);
  for (auto& k : keys_) k.layerLive = false;
  base_ = layer;
  recomputeLayer();
}

void Engine::recomputeLayer() {
  uint8_t top = base_;
  for (uint8_t i = 0; i < km_.numLayers && i < sizeof momentary_; i++)
    if (momentary_[i] > 0 && i > top) top = i;
  if (top != layer_) {
    layer_ = top;
    if (layerFn_) layerFn_(layer_);
  }
}

void Engine::pushHeld(uint8_t owner, Keycode code) {
  held_.push_back({int8_t(owner), code});
  emitReport();
}

void Engine::dropHeld(uint8_t owner) {
  held_.erase(std::remove_if(held_.begin(), held_.end(),
                             [&](const Held& h) { return h.owner == int8_t(owner); }),
              held_.end());
  emitReport();
}

void Engine::emitReport() {
  HidReport r;
  int n = 0;
  for (const Held& h : held_) {
    r.mods |= keycodeMods(h.code);
    const uint8_t page = keycodePage(h.code);
    const uint16_t usage = keycodeUsage(h.code);
    if (page == 0x07) {
      if (usage >= 0xE0 && usage <= 0xE7) {
        r.mods |= uint8_t(1u << (usage - 0xE0));
      } else if (n < 6 && std::find(r.keys, r.keys + n, uint8_t(usage)) == r.keys + n) {
        r.keys[n++] = uint8_t(usage);
      }
    } else if (page == 0x0C && r.consumer == 0) {
      r.consumer = usage;
    }
  }
  if (r != report_) {
    report_ = r;
    if (reportFn_) reportFn_(report_);
  }
}

}  // namespace kb
