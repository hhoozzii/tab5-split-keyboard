// Keymap engine: key events in, HID reports + layer changes out.
// Behaviour mirrors Miryoku ZMK:
//  - home-row mods and thumb layers are hold-taps with flavor "tap-preferred",
//    tapping term 200 ms: hold only if the key is still down when the term
//    expires; a release OR any other key press before that decides "tap".
//  - u_to_* layer jumps need a double tap within 200 ms.
//  - a key keeps the binding it had when pressed, so releasing a layer key
//    before the key pressed on that layer still releases the right key.
//  - the highest-numbered active layer wins.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "keymap_types.h"

namespace kb {

struct HidReport {
  uint8_t mods = 0;
  uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
  uint16_t consumer = 0;
  bool operator==(const HidReport& o) const;
  bool operator!=(const HidReport& o) const { return !(*this == o); }
};

struct EngineConfig {
  uint32_t tappingTermMs = 200;   // U_TAPPING_TERM in miryoku.h
  uint32_t doubleTapTermMs = 200; // u_to_* tap-dance term
};

class Engine {
 public:
  using ReportFn = std::function<void(const HidReport&)>;
  using LayerFn = std::function<void(uint8_t layer)>;
  using SpecialFn = std::function<void(Special, bool down)>;

  explicit Engine(const Keymap& km, EngineConfig cfg = {});

  void onReport(ReportFn f) { reportFn_ = std::move(f); }
  void onLayer(LayerFn f) { layerFn_ = std::move(f); }
  void onSpecial(SpecialFn f) { specialFn_ = std::move(f); }

  // pos: 0..35. nowMs: monotonic milliseconds. Call from one task only.
  void keyEvent(uint8_t pos, bool down, uint32_t nowMs);
  // Call periodically (e.g. every 5-10 ms) so hold decisions fire on time.
  void tick(uint32_t nowMs);
  // If a hold decision is pending, returns true and its deadline in `at`.
  bool nextDeadline(uint32_t& at) const;

  uint8_t layer() const { return layer_; }
  void jumpToLayer(uint8_t layer);  // like &to: drops momentary layers
  const HidReport& report() const { return report_; }

 private:
  enum class Phase : uint8_t { Idle, Undecided, Tap, Hold, Plain };
  struct KeyState {
    bool down = false;
    bool layerLive = false;  // this key currently holds a momentary layer
    Phase phase = Phase::Idle;
    Binding b{};
  };
  struct Held { int8_t owner; Keycode code; };

  void press(uint8_t pos, uint32_t now);
  void release(uint8_t pos, uint32_t now);
  void decide(uint8_t pos, bool hold);
  void pushHeld(uint8_t owner, Keycode code);
  void dropHeld(uint8_t owner);
  void emitReport();
  void recomputeLayer();

  const Keymap& km_;
  EngineConfig cfg_;
  KeyState keys_[kNumKeys];
  std::vector<Held> held_;
  uint8_t momentary_[16] = {0};
  uint8_t base_ = 0;
  uint8_t layer_ = 0;
  int undecided_ = -1;
  uint32_t undecidedAt_ = 0;
  int guardPos_ = -1;
  uint32_t guardReleasedAt_ = 0;
  HidReport report_;
  ReportFn reportFn_;
  LayerFn layerFn_;
  SpecialFn specialFn_;
};

}  // namespace kb
