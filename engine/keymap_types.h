// Hardware-independent types for the Tab5 split keyboard keymap engine.
// Key positions follow Miryoku ZMK's 36-key order:
//   0-9   top row    (left 0-4,   right 5-9)
//   10-19 home row   (left 10-14, right 15-19)
//   20-29 bottom row (left 20-24, right 25-29)
//   30-35 thumbs     (left 30-32, right 33-35)
#pragma once
#include <cstdint>

namespace kb {

constexpr int kNumKeys = 36;

// Keycode = (implicit mods << 24) | (HID usage page << 16) | usage id
using Keycode = uint32_t;
constexpr Keycode kbd(uint8_t usage, uint8_t mods = 0) {
  return (uint32_t(mods) << 24) | (0x07u << 16) | usage;
}
constexpr Keycode consumer(uint16_t usage) { return (0x0Cu << 16) | usage; }
constexpr uint8_t keycodePage(Keycode k) { return (k >> 16) & 0xFF; }
constexpr uint16_t keycodeUsage(Keycode k) { return k & 0xFFFF; }
constexpr uint8_t keycodeMods(Keycode k) { return k >> 24; }

// HID modifier bits
enum : uint8_t {
  MOD_LCTRL = 0x01, MOD_LSHFT = 0x02, MOD_LALT = 0x04, MOD_LGUI = 0x08,
  MOD_RCTRL = 0x10, MOD_RSHFT = 0x20, MOD_RALT = 0x40, MOD_RGUI = 0x80,
};

constexpr bool isLeftHalf(int pos) { return pos < 30 ? (pos % 10) < 5 : pos < 33; }

// Actions the engine cannot perform itself; the firmware handles them.
// Generated from the Miryoku layers (see tools/gen_keymap.py).
enum class Special : uint16_t;

enum class Kind : uint8_t {
  None,   // &none
  Key,    // &kp            a = keycode
  MT,     // hold-tap mod   a = hold keycode (a modifier), b = tap keycode
  LT,     // hold-tap layer a = layer, b = tap keycode
  To,     // &to behind a double-tap guard (Miryoku u_to_*)   a = layer
  Special // a = Special id
};

struct Binding {
  Kind kind = Kind::None;
  uint32_t a = 0;
  uint32_t b = 0;
};

constexpr Binding NONE() { return {Kind::None, 0, 0}; }
constexpr Binding K(Keycode k) { return {Kind::Key, k, 0}; }
constexpr Binding MT(Keycode hold, Keycode tap) { return {Kind::MT, hold, tap}; }
constexpr Binding LT(uint8_t layer, Keycode tap) { return {Kind::LT, layer, tap}; }
constexpr Binding TO(uint8_t layer) { return {Kind::To, layer, 0}; }
constexpr Binding SP(Special s) { return {Kind::Special, uint32_t(s), 0}; }

// Text drawn on a key: main = what a tap does, sub = what a hold does (or the layer name). Either may be "".
struct KeyLabel {
  const char* main;
  const char* sub;
};

struct Keymap {
  uint8_t numLayers;
  const Binding* bindings;      // numLayers * kNumKeys, row-major by layer
  const char* const* layerNames;
  const KeyLabel* labels = nullptr;   // numLayers * kNumKeys, same order as bindings (optional)
  const Binding& at(uint8_t layer, int pos) const { return bindings[layer * kNumKeys + pos]; }
  KeyLabel labelAt(uint8_t layer, int pos) const { return labels ? labels[layer * kNumKeys + pos] : KeyLabel{"", ""}; }
};

}  // namespace kb
