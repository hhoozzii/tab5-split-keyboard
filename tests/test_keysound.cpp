#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include "../engine/keysound.h"
using namespace kb;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static float rms(const std::vector<int16_t>& v, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b && i < v.size(); i++) s += double(v[i]) * v[i]; return sqrtf(float(s / (b - a))); }
static std::vector<int16_t> run(KeySynth& s, int blocks) { std::vector<int16_t> o; int16_t b[128]; for (int i = 0; i < blocks; i++) { s.render(b, 128); o.insert(o.end(), b, b + 128); } return o; }
int main() {
  CHECK(noteForPos(10) == 72); CHECK(noteForPos(11) == 74); CHECK(noteForPos(0) > noteForPos(10)); CHECK(noteForPos(20) < noteForPos(10));
  for (int p = 0; p < 36; p++) { int n = noteForPos(p); CHECK(n >= 64 && n <= 100); }
  { KeySynth s; s.setKind(0); s.setVolume(5); s.keyEvent(12, true); auto o = run(s, 40);   // click: short, silent after
    CHECK(rms(o, 0, 400) > 500); CHECK(rms(o, 2000, 5000) < 1); CHECK(!s.busy()); }
  { KeySynth s; s.setKind(1); s.setVolume(5); s.keyEvent(12, true); auto o = run(s, 60);    // piano: sustains while held
    CHECK(rms(o, 6000, 7000) > 300); CHECK(s.busy());
    s.keyEvent(12, false); auto o2 = run(s, 60); CHECK(rms(o2, 0, 200) > 100); CHECK(rms(o2, 5000, 7000) < 1); CHECK(!s.busy()); }
  { KeySynth s; s.setKind(2); s.setVolume(10); for (int p = 0; p < 12; p++) s.keyEvent(p, true); auto o = run(s, 80);   // many voices: no clipping past int16
    int mx = 0; for (auto v : o) mx = std::max(mx, abs(int(v))); CHECK(mx < 32000); CHECK(mx > 3000); }
  { KeySynth s; s.setOn(false); s.keyEvent(5, true); auto o = run(s, 10); CHECK(rms(o, 0, 1280) == 0); }
  { KeySynth s; s.setKind(1); s.setVolume(5); s.keyEvent(5, true); run(s, 10); s.setOn(false); s.keyEvent(5, false); run(s, 60); CHECK(!s.busy()); }   // release still lands when switched off
  { KeySynth s; s.setVolume(0); s.keyEvent(5, true); auto o = run(s, 4); CHECK(rms(o, 0, 512) == 0); }
  { KeySynth s; s.setVolume(3); CHECK(s.volume() == 3); s.setVolume(99); CHECK(s.volume() == 10); s.setVolume(-4); CHECK(s.volume() == 0); s.setKind(11); CHECK(s.kind() < kSoundKinds); }
  for (int k = 0; k < kSoundKinds; k++) {   // every sound: audible, within range, ends after release
    KeySynth s; s.setKind(k); s.setVolume(7); s.keyEvent(12, true);
    auto o = run(s, 20); int mx = 0; for (auto v : o) mx = std::max(mx, abs(int(v)));
    CHECK(mx > 800); CHECK(mx < 32000);
    s.keyEvent(12, false); run(s, 400);
    CHECK(!s.busy());
    CHECK(std::string(soundName(k)).size() >= 3);
  }
  { KeySynth s; s.setKind(3); s.setVolume(7); s.keyEvent(12, true); auto o = run(s, 120); CHECK(rms(o, 100, 600) > rms(o, 14000, 15000)); }   // marimba dies away while held
  { KeySynth s; s.setKind(4); s.setVolume(7); s.keyEvent(12, true); auto o = run(s, 200); CHECK(rms(o, 9000, 10000) > 150); }   // bell still rings after 0.4 s
  { KeySynth s; s.setKind(7); s.setVolume(7); s.keyEvent(12, true); run(s, 1500); CHECK(!s.busy()); }   // a held pluck dies out by itself
  printf(fails ? "%d FAILED\n" : "all keysound checks passed\n", fails);
  return fails != 0;
}
