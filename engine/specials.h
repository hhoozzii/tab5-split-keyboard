// Handlers for the Miryoku "special" keys that are not plain HID keys:
//  - mouse movement, wheel and buttons (the Mouse layer)
//  - caps word (letters come out shifted until something else is typed)
//  - Bluetooth profile select and "Boot" (reboot)
// Hardware independent: feed it Engine events, it calls back with mouse reports and actions.
#pragma once
#include <cstdint>
#include <functional>
#include "keymap_engine.h"
#include "miryoku_keymap.h"

namespace kb {

struct MouseReport {
  uint8_t buttons = 0;          // bit0 left, bit1 right, bit2 middle
  int8_t dx = 0, dy = 0;        // pixels
  int8_t wheel = 0, pan = 0;    // + = up / right
  bool operator==(const MouseReport& o) const {
    return buttons == o.buttons && dx == o.dx && dy == o.dy && wheel == o.wheel && pan == o.pan;
  }
};

enum class Action : uint8_t { Profile, Reboot };

struct SpecialsConfig {
  int moveStartPxS = 300;       // pointer speed when a direction key goes down
  int moveMaxPxS = 1200;        // ... ramping up to this over moveRampMs
  uint32_t moveRampMs = 500;
  int wheelPerS = 8;            // wheel notches per second while a wheel key is held
  uint32_t capsIdleMs = 5000;   // caps word ends after this long without a typed key
  uint32_t reportMs = 12;       // at most one movement report per this many ms (about one BLE interval)
};

class Specials {
 public:
  using MouseFn = std::function<void(const MouseReport&)>;
  using ActionFn = std::function<void(Action, int arg)>;

  explicit Specials(SpecialsConfig cfg = {}) : cfg_(cfg) {}
  void onMouse(MouseFn f) { mouseFn_ = std::move(f); }
  void onAction(ActionFn f) { actionFn_ = std::move(f); }

  // From Engine::onSpecial.
  void special(Special s, bool down, uint32_t now) {
    switch (s) {
      case Special::MOUSE_BTN1: button(0x01, down); break;
      case Special::MOUSE_BTN2: button(0x02, down); break;
      case Special::MOUSE_BTN3: button(0x04, down); break;
      case Special::MOUSE_LEFT: dir(kL, down, now); break;
      case Special::MOUSE_RIGHT: dir(kR, down, now); break;
      case Special::MOUSE_UP: dir(kU, down, now); break;
      case Special::MOUSE_DOWN: dir(kD, down, now); break;
      case Special::WHEEL_LEFT: dir(kWL, down, now); break;
      case Special::WHEEL_RIGHT: dir(kWR, down, now); break;
      case Special::WHEEL_UP: dir(kWU, down, now); break;
      case Special::WHEEL_DOWN: dir(kWD, down, now); break;
      case Special::CAPS_WORD:
        if (down) { caps_ = !caps_; capsAt_ = now; }
        break;
      case Special::BT_SEL_0: if (down && actionFn_) actionFn_(Action::Profile, 0); break;
      case Special::BT_SEL_1: if (down && actionFn_) actionFn_(Action::Profile, 1); break;
      case Special::BT_SEL_2: if (down && actionFn_) actionFn_(Action::Profile, 2); break;
      case Special::BT_SEL_3: if (down && actionFn_) actionFn_(Action::Profile, 3); break;
      case Special::BOOT: if (down && actionFn_) actionFn_(Action::Reboot, 0); break;
      default: break;   // RGB, external power, output toggle: no hardware for these
    }
  }

  // Call every 5-10 ms. Moves the pointer / wheel while direction keys are held; ends caps word on idle.
  void tick(uint32_t now) {
    if (caps_ && now - capsAt_ >= cfg_.capsIdleMs) caps_ = false;
    if (!(dirs_ & (kL | kR | kU | kD | kWL | kWR | kWU | kWD))) { last_ = now; return; }
    const uint32_t dt = now - last_;
    if (dt == 0) return;
    last_ = now;
    const bool due = now - lastEmit_ >= cfg_.reportMs;
    const uint32_t held = now - moveFrom_;
    const int speed = cfg_.moveStartPxS +
        int(int64_t(cfg_.moveMaxPxS - cfg_.moveStartPxS) * int64_t(held > cfg_.moveRampMs ? cfg_.moveRampMs : held) /
            int64_t(cfg_.moveRampMs));
    const float px = speed * dt / 1000.0f, notch = cfg_.wheelPerS * dt / 1000.0f;
    ax_ += ((dirs_ & kR) ? px : 0) - ((dirs_ & kL) ? px : 0);
    ay_ += ((dirs_ & kD) ? px : 0) - ((dirs_ & kU) ? px : 0);
    aw_ += ((dirs_ & kWU) ? notch : 0) - ((dirs_ & kWD) ? notch : 0);
    ap_ += ((dirs_ & kWR) ? notch : 0) - ((dirs_ & kWL) ? notch : 0);
    if (!due) return;                       // keep accumulating; send at the BLE-friendly rate
    MouseReport r;
    r.buttons = buttons_;
    r.dx = take(ax_); r.dy = take(ay_); r.wheel = take(aw_); r.pan = take(ap_);
    if (r.dx || r.dy || r.wheel || r.pan) { emit(r); lastEmit_ = now; }
  }

  // Run every keyboard report through this before sending it: caps word shifts letters.
  HidReport filter(HidReport in, uint32_t now) {
    if (!caps_) return in;
    bool letter = false, other = false;
    for (uint8_t k : in.keys) {
      if (!k) continue;
      if (k >= 0x04 && k <= 0x1D) letter = true;                                  // a-z
      else if ((k >= 0x1E && k <= 0x27) || k == 0x2D || k == 0x2A || k == 0x4C) {} // 1-0, minus, backspace, delete
      else other = true;                                                          // space, enter, arrows, ...
    }
    if (in.mods & ~0x22) other = true;                                            // ctrl/alt/gui end it, shift does not
    if (other) { caps_ = false; return in; }
    if (letter) in.mods |= 0x02;
    if (letter || in.keys[0]) capsAt_ = now;
    return in;
  }
  bool capsWord() const { return caps_; }
  uint8_t buttons() const { return buttons_; }

 private:
  enum : uint16_t { kL = 1, kR = 2, kU = 4, kD = 8, kWL = 16, kWR = 32, kWU = 64, kWD = 128 };

  static int8_t take(float& a) {
    int v = int(a);               // truncates toward zero
    if (v > 127) v = 127;
    if (v < -127) v = -127;
    a -= float(v);
    return int8_t(v);
  }
  void emit(const MouseReport& r) { if (mouseFn_) mouseFn_(r); }
  void button(uint8_t mask, bool down) {
    const uint8_t was = buttons_;
    buttons_ = down ? (buttons_ | mask) : (buttons_ & ~mask);
    if (buttons_ != was) { MouseReport r; r.buttons = buttons_; emit(r); }
  }
  void dir(uint16_t bit, bool down, uint32_t now) {
    const bool wasMoving = dirs_ != 0;
    if (down) {
      dirs_ |= bit;
      if (!wasMoving) { moveFrom_ = now; last_ = now; lastEmit_ = now; ax_ = ay_ = aw_ = ap_ = 0; }
      // an immediate first step, so a quick tap still nudges the pointer
      MouseReport r; r.buttons = buttons_;
      if (bit == kL) r.dx = -1; else if (bit == kR) r.dx = 1; else if (bit == kU) r.dy = -1; else if (bit == kD) r.dy = 1;
      else if (bit == kWU) r.wheel = 1; else if (bit == kWD) r.wheel = -1;
      else if (bit == kWR) r.pan = 1; else if (bit == kWL) r.pan = -1;
      emit(r);
    } else {
      dirs_ &= ~bit;
      if (!dirs_) ax_ = ay_ = aw_ = ap_ = 0;
    }
  }

  SpecialsConfig cfg_;
  MouseFn mouseFn_;
  ActionFn actionFn_;
  uint16_t dirs_ = 0;
  uint8_t buttons_ = 0;
  uint32_t moveFrom_ = 0, last_ = 0, lastEmit_ = 0, capsAt_ = 0;
  float ax_ = 0, ay_ = 0, aw_ = 0, ap_ = 0;
  bool caps_ = false;
};

}  // namespace kb
