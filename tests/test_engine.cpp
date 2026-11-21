// Desktop tests for the keymap engine against the generated Miryoku QWERTY map.
// Build: cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
#include <cstdio>
#include <string>
#include <vector>

#include "../engine/keymap_engine.h"
#include "../engine/miryoku_keymap.h"
#include "../engine/specials.h"

using namespace kb;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    g_checks++;                                                            \
    if (!(cond)) { g_fail++; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// HID usages used below
enum : uint8_t { A = 0x04, D = 0x07, F = 0x09, H = 0x0B, J = 0x0D, S = 0x16, SPACE = 0x2C, LEFT = 0x50 };
// Key positions (Miryoku 36-key order)
enum : uint8_t {
  P_A = 10, P_S = 11, P_D = 12, P_F = 13,          // left home row
  P_H = 15, P_J = 16,                              // right home row
  P_NAV_THUMB = 31,                                // Space / NAV
  P_TAP_JUMP = 1,                                  // &u_to_U_TAP on NAV
  P_NAV_LEFT = 16,                                 // &kp LEFT on NAV (right home row, 2nd key)
};

struct Rig {
  Engine e{kMiryokuKeymap};
  std::vector<HidReport> reports;
  std::vector<uint8_t> layers;
  Rig() {
    e.onReport([this](const HidReport& r) { reports.push_back(r); });
    e.onLayer([this](uint8_t l) { layers.push_back(l); });
  }
  void down(uint8_t p, uint32_t t) { e.keyEvent(p, true, t); }
  void up(uint8_t p, uint32_t t) { e.keyEvent(p, false, t); }
  bool anyReportHasMods() const {
    for (auto& r : reports) if (r.mods) return true;
    return false;
  }
};

static void test_tap_home_key() {
  std::puts("tap on home key sends the letter, no modifier");
  Rig r;
  r.down(P_A, 0);
  r.up(P_A, 80);
  CHECK(r.reports.size() == 2);
  CHECK(r.reports[0].keys[0] == A && r.reports[0].mods == 0);
  CHECK(r.reports[1].keys[0] == 0 && r.reports[1].mods == 0);
}

static void test_hold_home_key() {
  std::puts("holding a home key past 200 ms makes it a modifier");
  Rig r;
  r.down(P_A, 0);
  r.e.tick(199);
  CHECK(r.reports.empty());
  r.e.tick(200);
  CHECK(r.reports.size() == 1 && r.reports[0].mods == MOD_LGUI);
  r.up(P_A, 400);
  CHECK(r.e.report().mods == 0);
}

static void test_cross_hand_chord() {
  std::puts("hold left Shift (F), then tap right H: Shift+H, across both halves");
  Rig r;
  r.down(P_F, 0);
  r.down(P_H, 250);  // timer fires first -> F becomes Shift, then H is pressed
  CHECK(r.e.report().mods == MOD_LSHFT && r.e.report().keys[0] == H);
  r.up(P_H, 300);
  CHECK(r.e.report().mods == MOD_LSHFT && r.e.report().keys[0] == 0);
  r.up(P_F, 350);
  CHECK(r.e.report().mods == 0);
}

static void test_fast_roll_is_all_taps() {
  std::puts("rolling A then S inside the term types 'as' (tap-preferred)");
  Rig r;
  r.down(P_A, 0);
  r.down(P_S, 40);   // A resolves to tap now
  r.up(P_A, 80);
  r.up(P_S, 120);
  CHECK(!r.anyReportHasMods());
  CHECK(r.reports.front().keys[0] == A);
  bool sawS = false;
  for (auto& x : r.reports) for (int i = 0; i < 6; i++) if (x.keys[i] == S) sawS = true;
  CHECK(sawS);
}

static void test_same_hand_mods_need_staggered_holds() {
  std::puts("two same-hand mods: second must be pressed after the first has resolved to hold");
  Rig r;
  r.down(P_D, 0);
  r.down(P_F, 250);  // D already Ctrl; F now pending
  r.e.tick(450);     // F becomes Shift
  CHECK(r.e.report().mods == (MOD_LCTRL | MOD_LSHFT));
}

static void test_layer_hold_and_latch() {
  std::puts("hold NAV thumb, press LEFT, release thumb first: LEFT still releases cleanly");
  Rig r;
  r.down(P_NAV_THUMB, 0);
  r.e.tick(200);
  CHECK(r.e.layer() == L_NAV);
  r.down(P_NAV_LEFT, 300);
  CHECK(r.e.report().keys[0] == LEFT);
  r.up(P_NAV_THUMB, 350);
  CHECK(r.e.layer() == L_BASE);
  r.up(P_NAV_LEFT, 400);  // key latched on NAV, so this releases LEFT, not 'J'
  CHECK(r.e.report().keys[0] == 0);
}

static void test_thumb_tap_is_space() {
  std::puts("quick tap of the NAV thumb types Space and never changes layer");
  Rig r;
  r.down(P_NAV_THUMB, 0);
  r.up(P_NAV_THUMB, 90);
  CHECK(r.layers.empty());
  CHECK(r.reports.size() == 2 && r.reports[0].keys[0] == SPACE);
}

static void test_to_layer_needs_double_tap() {
  std::puts("u_to_U_TAP in NAV: single tap does nothing, double tap jumps");
  Rig r;
  r.down(P_NAV_THUMB, 0);
  r.e.tick(200);
  r.down(P_TAP_JUMP, 300);
  r.up(P_TAP_JUMP, 340);
  r.up(P_NAV_THUMB, 500);
  CHECK(r.e.layer() == L_BASE);

  Rig q;
  q.down(P_NAV_THUMB, 0);
  q.e.tick(200);
  q.down(P_TAP_JUMP, 300);
  q.up(P_TAP_JUMP, 340);
  q.down(P_TAP_JUMP, 420);  // inside 200 ms of the first release
  CHECK(q.e.layer() == L_TAP);
  q.up(P_TAP_JUMP, 450);
  q.up(P_NAV_THUMB, 500);   // the held NAV was dropped by the jump
  CHECK(q.e.layer() == L_TAP);
}

static void test_deadline_for_scheduler() {
  std::puts("nextDeadline lets the firmware sleep until the hold decision");
  Rig r;
  uint32_t at = 0;
  CHECK(!r.e.nextDeadline(at));
  r.down(P_A, 1000);
  CHECK(r.e.nextDeadline(at) && at == 1200);
  r.up(P_A, 1050);
  CHECK(!r.e.nextDeadline(at));
}

static void test_layer_content_spot_checks() {
  std::puts("generated table matches Miryoku");
  const Keymap& m = kMiryokuKeymap;
  CHECK(m.numLayers == 10);
  CHECK(m.at(L_BASE, P_A).kind == Kind::MT && m.at(L_BASE, P_A).a == kbd(0xE3));      // LGUI
  CHECK(m.at(L_BASE, 31).kind == Kind::LT && m.at(L_BASE, 31).a == L_NAV);
  CHECK(m.at(L_BASE, 30).kind == Kind::LT && m.at(L_BASE, 30).a == L_MEDIA);
  CHECK(m.at(L_BASE, 35).kind == Kind::LT && m.at(L_BASE, 35).a == L_FUN);
  CHECK(m.at(L_NAV, P_NAV_LEFT).kind == Kind::Key);
}

// ---------------------------------------------------------------- specials

static int posOfSpecial(uint8_t layer, Special sp) {
  for (int p = 0; p < 36; p++) {
    const Binding& b = kMiryokuKeymap.at(layer, p);
    if (b.kind == Kind::Special && Special(b.a) == sp) return p;
  }
  return -1;
}

static void test_mouse_move_and_ramp() {
  puts("mouse movement ramps up and stops");
  Specials sp;
  std::vector<MouseReport> out;
  sp.onMouse([&](const MouseReport& r) { out.push_back(r); });
  uint32_t t = 1000;
  sp.special(Special::MOUSE_RIGHT, true, t);
  CHECK(out.size() == 1 && out[0].dx == 1 && out[0].dy == 0);           // immediate nudge
  int early = 0, late = 0;
  for (int i = 0; i < 100; i++) { t += 10; sp.tick(t); }                // first second
  int total1 = 0; for (auto& r : out) total1 += r.dx;
  for (auto& r : out) CHECK(r.dy == 0 && r.dx >= 0);
  out.clear();
  for (int i = 0; i < 100; i++) { t += 10; sp.tick(t); }                // second second: at full speed
  for (auto& r : out) late += r.dx;
  CHECK(late >= 1150 && late <= 1250);                                  // about 1200 px/s
  CHECK(total1 > 600 && total1 < 1100);                                 // ramped from 300 up
  early = total1;
  (void)early;
  sp.special(Special::MOUSE_RIGHT, false, t);
  out.clear();
  for (int i = 0; i < 20; i++) { t += 10; sp.tick(t); }
  CHECK(out.empty());                                                    // stops when released
  // diagonal and opposite directions
  sp.special(Special::MOUSE_LEFT, true, t); sp.special(Special::MOUSE_UP, true, t);
  out.clear();
  for (int i = 0; i < 50; i++) { t += 10; sp.tick(t); }
  int sx = 0, sy = 0; for (auto& r : out) { sx += r.dx; sy += r.dy; }
  CHECK(sx < -100 && sy < -100);
  sp.special(Special::MOUSE_LEFT, false, t); sp.special(Special::MOUSE_UP, false, t);
}

static void test_mouse_buttons_and_wheel() {
  puts("mouse buttons and wheel");
  Specials sp;
  std::vector<MouseReport> out;
  sp.onMouse([&](const MouseReport& r) { out.push_back(r); });
  sp.special(Special::MOUSE_BTN1, true, 0);
  sp.special(Special::MOUSE_BTN2, true, 0);
  CHECK(out.size() == 2 && out.back().buttons == 0x03);
  sp.special(Special::MOUSE_BTN1, false, 0);
  CHECK(out.back().buttons == 0x02 && sp.buttons() == 0x02);
  sp.special(Special::MOUSE_BTN2, false, 0);
  CHECK(out.back().buttons == 0);
  out.clear();
  uint32_t t = 100;
  sp.special(Special::WHEEL_DOWN, true, t);
  CHECK(out.size() == 1 && out[0].wheel == -1);
  out.clear();
  for (int i = 0; i < 100; i++) { t += 10; sp.tick(t); }                // 1 s at 8 notches/s
  int w = 0; for (auto& r : out) { w += r.wheel; CHECK(r.dx == 0 && r.dy == 0); }
  CHECK(w <= -7 && w >= -9);
  sp.special(Special::WHEEL_DOWN, false, t);
  sp.special(Special::WHEEL_RIGHT, true, t);
  CHECK(out.back().pan == 1);
  sp.special(Special::WHEEL_RIGHT, false, t);
}

static HidReport rep(uint8_t mods, uint8_t k0 = 0, uint8_t k1 = 0) {
  HidReport r; r.mods = mods; r.keys[0] = k0; r.keys[1] = k1; return r;
}

static void test_caps_word() {
  puts("caps word");
  Specials sp;
  uint32_t t = 0;
  CHECK(sp.filter(rep(0, 0x04), t) == rep(0, 0x04));                    // off: untouched
  sp.special(Special::CAPS_WORD, true, t); sp.special(Special::CAPS_WORD, false, t);
  CHECK(sp.capsWord());
  CHECK(sp.filter(rep(0, 0x04), ++t) == rep(0x02, 0x04));               // letters get shift
  CHECK(sp.filter(rep(0), ++t) == rep(0));                              // release: no shift
  CHECK(sp.filter(rep(0, 0x1E), ++t) == rep(0, 0x1E));                  // digits pass, unshifted
  CHECK(sp.filter(rep(0, 0x2D), ++t) == rep(0, 0x2D));                  // minus continues
  CHECK(sp.filter(rep(0, 0x2A), ++t) == rep(0, 0x2A));                  // backspace continues
  CHECK(sp.capsWord());
  CHECK(sp.filter(rep(0x02, 0x04), ++t) == rep(0x02, 0x04));            // a user's own shift is fine
  CHECK(sp.capsWord());
  CHECK(sp.filter(rep(0, 0x2C), ++t) == rep(0, 0x2C));                  // space ends it, and passes through
  CHECK(!sp.capsWord());
  CHECK(sp.filter(rep(0, 0x04), ++t) == rep(0, 0x04));
  // ctrl ends it; second tap toggles off; idle ends it
  sp.special(Special::CAPS_WORD, true, t);
  CHECK(sp.filter(rep(0x01, 0x04), ++t) == rep(0x01, 0x04) && !sp.capsWord());
  sp.special(Special::CAPS_WORD, true, t); sp.special(Special::CAPS_WORD, true, t);
  CHECK(!sp.capsWord());
  sp.special(Special::CAPS_WORD, true, t);
  sp.tick(t + 4000); CHECK(sp.capsWord());
  sp.tick(t + 5000); CHECK(!sp.capsWord());
  // typing keeps it alive past the idle time
  t += 10000;
  sp.special(Special::CAPS_WORD, true, t);
  for (int i = 1; i <= 4; i++) { sp.filter(rep(0, 0x05), t + i * 3000); sp.tick(t + i * 3000); }
  CHECK(sp.capsWord());
}

static void test_actions() {
  puts("profile select and boot actions");
  Specials sp;
  std::vector<std::pair<Action, int>> acts;
  sp.onAction([&](Action a, int n) { acts.push_back({a, n}); });
  sp.special(Special::BT_SEL_2, true, 0);
  sp.special(Special::BT_SEL_2, false, 0);                              // release does nothing
  sp.special(Special::BT_SEL_0, true, 0);
  sp.special(Special::BOOT, true, 0);
  sp.special(Special::RGB_TOG, true, 0);                                // ignored, no crash
  CHECK(acts.size() == 3);
  CHECK(acts[0].first == Action::Profile && acts[0].second == 2);
  CHECK(acts[1].first == Action::Profile && acts[1].second == 0);
  CHECK(acts[2].first == Action::Reboot);
}

static void test_mouse_layer_through_engine() {
  puts("mouse layer keys drive the pointer through the engine");
  Engine e(kMiryokuKeymap);
  Specials sp;
  std::vector<MouseReport> out;
  sp.onMouse([&](const MouseReport& r) { out.push_back(r); });
  e.onSpecial([&](Special s, bool d) { sp.special(s, d, 0); });
  int mouseThumb = -1;
  for (int p = 30; p < 36; p++) {
    const Binding& b = kMiryokuKeymap.at(L_BASE, p);
    if (b.kind == Kind::LT && b.a == L_MOUSE) mouseThumb = p;
  }
  const int right = posOfSpecial(L_MOUSE, Special::MOUSE_RIGHT), click = posOfSpecial(L_MOUSE, Special::MOUSE_BTN1);
  CHECK(mouseThumb >= 0 && right >= 0 && click >= 0);
  uint32_t t = 0;
  e.keyEvent(uint8_t(mouseThumb), true, t);
  e.tick(t + 250);                                                       // hold decided: Mouse layer on
  CHECK(e.layer() == L_MOUSE);
  e.keyEvent(uint8_t(right), true, t + 300);
  CHECK(!out.empty() && out.back().dx == 1);
  e.keyEvent(uint8_t(right), false, t + 320);
  e.keyEvent(uint8_t(click), true, t + 340);
  CHECK(out.back().buttons == 0x01);
  e.keyEvent(uint8_t(click), false, t + 360);
  CHECK(out.back().buttons == 0);
  // all the directions, wheel and the three buttons exist on the Mouse layer
  for (Special s : {Special::MOUSE_LEFT, Special::MOUSE_RIGHT, Special::MOUSE_UP, Special::MOUSE_DOWN,
                    Special::WHEEL_LEFT, Special::WHEEL_RIGHT, Special::WHEEL_UP, Special::WHEEL_DOWN,
                    Special::MOUSE_BTN1, Special::MOUSE_BTN2, Special::MOUSE_BTN3})
    CHECK(posOfSpecial(L_MOUSE, s) >= 0);
  for (int n = 0; n < 4; n++) CHECK(posOfSpecial(L_MEDIA, Special(int(Special::BT_SEL_0) + n)) >= 0);
  CHECK(posOfSpecial(L_BUTTON, Special::MOUSE_BTN1) >= -1);              // (just must not crash)
}

int main() {
  test_layer_content_spot_checks();
  test_tap_home_key();
  test_hold_home_key();
  test_cross_hand_chord();
  test_fast_roll_is_all_taps();
  test_same_hand_mods_need_staggered_holds();
  test_layer_hold_and_latch();
  test_thumb_tap_is_space();
  test_to_layer_needs_double_tap();
  test_deadline_for_scheduler();
  test_mouse_move_and_ramp();
  test_mouse_buttons_and_wheel();
  test_caps_word();
  test_actions();
  test_mouse_layer_through_engine();
  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
