// Desktop tests for touch tracking, layout hit-testing and finger->key latching.
#include <cmath>
#include <cstdio>
#include <vector>
#include "../core/touch_keys.h"
#include "../core/orientation.h"
#include "../core/autorotate.h"

using namespace kb;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

struct KeyLog {
  std::vector<std::pair<int,bool>> ev;
  bool has(int pos, bool down) const { for (auto& e : ev) if (e.first == pos && e.second == down) return true; return false; }
  int count(int pos, bool down) const { int c = 0; for (auto& e : ev) c += (e.first == pos && e.second == down); return c; }
};

// center of local key idx
static Pt center(const KeyLayout& l, int idx) { Center c = l.center(idx); return {int16_t(c.x), int16_t(c.y)}; }

int main() {
  KeyLayout L(Half::Left), R(Half::Right);

  // layout: every local key maps to the right global position and hit() round-trips
  for (int i = 0; i < kHalfKeys; i++) {
    Pt c = center(L, i);
    CHECK(L.hit(c.x, c.y) == L.posOfIndex(i));
    Pt d = center(R, i);
    CHECK(R.hit(d.x, d.y) == R.posOfIndex(i));
  }
  CHECK(L.posOfIndex(0) == 0 && L.posOfIndex(4) == 4 && L.posOfIndex(5) == 10 && L.posOfIndex(14) == 24);
  CHECK(L.posOfIndex(15) == 30 && L.posOfIndex(17) == 32);
  CHECK(R.posOfIndex(0) == 5 && R.posOfIndex(4) == 9 && R.posOfIndex(5) == 15 && R.posOfIndex(14) == 29);
  CHECK(R.posOfIndex(15) == 33 && R.posOfIndex(17) == 35);

  // keys: 19 mm wide pitch (220 px), tall enough to hit; every drawn key (rotated corners included) stays on screen
  CHECK(L.pitchX() >= 200 && L.pitchX() == R.pitchX() && L.pitchY() >= 130 && L.pitchY() == R.pitchY());
  CHECK(L.capW() < L.pitchX() && L.capH() < L.pitchY());
  for (int i = 0; i < kHalfKeys; i++) {
    for (const KeyLayout* l : {&L, &R}) {
      Rect r = l->rect(i);
      CHECK(r.w == l->capW() && r.h == l->capH());
      const double a = l->angleDeci(i) * 3.14159265358979 / 1800.0;
      const double ex = r.w / 2.0 * std::fabs(std::cos(a)) + r.h / 2.0 * std::fabs(std::sin(a));
      const double ey = r.w / 2.0 * std::fabs(std::sin(a)) + r.h / 2.0 * std::fabs(std::cos(a));
      Center c = l->center(i);
      CHECK(c.x - ex >= 0 && c.y - ey >= 0 && c.x + ex <= 1280 && c.y + ey <= 720);
    }
  }
  // stagger: pinky column lowest on the left half, mirrored on the right; middle column highest
  CHECK(L.center(0).y > L.center(1).y && L.center(1).y > L.center(2).y && L.center(3).y > L.center(2).y);
  CHECK(L.center(4).y > L.center(3).y);
  CHECK(L.center(0).y == R.center(4).y && L.center(2).y == R.center(2).y && L.center(4).y == R.center(0).y);
  // columns are one pitch apart, rows too
  for (int i = 0; i < 10; i++) {
    CHECK(std::abs(L.center(i + 5).y - L.center(i).y - L.pitchY()) <= 1 && L.center(i + 5).x == L.center(i).x);
  }
  for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++)
    CHECK(std::abs(L.center(r * 5 + c + 1).x - L.center(r * 5 + c).x - L.pitchX()) <= 1);
  // thumbs: outer level, inner two tilt 13 and 25 degrees and sit lower; right half is the mirror image
  CHECK(L.angleDeci(15) == 0 && L.angleDeci(16) == 130 && L.angleDeci(17) == 250);
  CHECK(R.angleDeci(15) == -250 && R.angleDeci(16) == -130 && R.angleDeci(17) == 0);
  CHECK(L.center(15).y < L.center(16).y && L.center(16).y < L.center(17).y);
  CHECK(L.center(15).x < L.center(16).x && L.center(16).x < L.center(17).x);
  CHECK(R.center(15).x < R.center(16).x && R.center(16).x < R.center(17).x);
  CHECK(R.center(15).y == L.center(17).y && R.center(17).y == L.center(15).y);
  // mirror: right half x offsets from its cluster edge equal the left's from its edge
  for (int i = 0; i < kHalfKeys; i++) {
    const int j = i < 15 ? (i / 5) * 5 + 4 - (i % 5) : 15 + 2 - (i - 15);   // the same finger on the other half
    CHECK(std::abs((R.center(i).x - R.clusterLeft()) - (L.clusterRight() - L.center(j).x)) <= 2);
    CHECK(R.center(i).y == L.center(j).y);
  }
  // thumbs are below the lowest key of the columns they sit under
  for (int t = 0; t < 3; t++) CHECK(L.center(15 + t).y > L.center(10 + 2 + t).y + L.pitchY() / 2 - 1);
  // clusters hug the edge facing the other half; the status area is empty space under the outer columns
  CHECK(L.clusterRight() >= 1280 - 12 && R.clusterLeft() <= 12);
  for (const KeyLayout* l : {&L, &R}) {
    const Rect s = l->statusRect();
    CHECK(s.w >= 480 && s.h >= 150 && s.x >= 0 && s.x + s.w <= 1280 && s.y + s.h <= 720);
    for (int y = s.y; y < s.y + s.h; y += 3)
      for (int x = s.x; x < s.x + s.w; x += 3) CHECK(l->hit(x, y) == -1);
  }
  CHECK(L.hit(-1, 300) == -1 && L.hit(1400, 700) == -1 && L.hit(10, 700) == -1);

  // faces of different keys never overlap (sampled), and every point inside a drawn face hits that key
  for (const KeyLayout* l : {&L, &R})
    for (int i = 0; i < kHalfKeys; i++) {
      const double a = l->angleDeci(i) * 3.14159265358979 / 1800.0;
      Center c = l->center(i);
      for (int fu = -9; fu <= 9; fu += 3)
        for (int fv = -9; fv <= 9; fv += 3) {
          const double u = fu / 10.0 * l->capW() / 2.0, v = fv / 10.0 * l->capH() / 2.0;
          const int x = int(std::lround(c.x + u * std::cos(a) - v * std::sin(a)));
          const int y = int(std::lround(c.y + u * std::sin(a) + v * std::cos(a)));
          CHECK(l->hit(x, y) == l->posOfIndex(i));
        }
    }
  // all 36 positions reachable; every point inside a key's drawn square hits that key
  {
    bool seen[36] = {};
    for (const KeyLayout* l : {&L, &R})
      for (int y = 0; y < 720; y += 2)
        for (int x = 0; x < 1280; x += 2) {
          int a = l->hit(x, y);
          if (a >= 0) seen[a] = true;
        }
    int n = 0; for (bool v : seen) n += v;
    CHECK(n == 36);
  }
  // each half only ever reports its own positions
  for (const KeyLayout* l : {&L, &R})
    for (int y = 0; y < 720; y += 5)
      for (int x = 0; x < 1280; x += 5) {
        int a = l->hit(x, y);
        if (a >= 0) { const int col = a % 10; CHECK((l == &L) == (a >= 30 ? a < 33 : col < 5)); }
      }

  // single tap
  {
    KeyLog log; TouchKeys tk(L, [&](uint8_t p, bool d) { log.ev.push_back({p, d}); });
    Pt c = center(L, 6);  // row1 col1 -> pos 11
    tk.frame(&c, 1);
    CHECK(log.has(11, true) && !log.has(11, false));
    tk.frame(nullptr, 0);
    CHECK(log.has(11, false) && tk.fingers() == 0);
  }
  // drag off the key: still releases the key it landed on, and presses nothing new
  {
    KeyLog log; TouchKeys tk(L, [&](uint8_t p, bool d) { log.ev.push_back({p, d}); });
    Pt p = center(L, 6);
    tk.frame(&p, 1);
    Pt q = center(L, 7);
    for (int s = 1; s <= 10; s++) { Pt m = {int16_t(p.x + (q.x - p.x) * s / 10), p.y}; tk.frame(&m, 1); }
    tk.frame(nullptr, 0);
    CHECK(log.ev.size() == 2 && log.has(11, true) && log.has(11, false));
  }
  // thumb + home-row key held together, released in either order
  {
    KeyLog log; TouchKeys tk(L, [&](uint8_t p, bool d) { log.ev.push_back({p, d}); });
    Pt thumb = center(L, 16), home = center(L, 8);   // pos 31 and 13
    Pt both[2] = {thumb, home};
    tk.frame(&thumb, 1);
    tk.frame(both, 2);
    CHECK(tk.fingers() == 2 && log.has(31, true) && log.has(13, true));
    Pt rest[1] = {home};            // thumb lifts first
    tk.frame(rest, 1);
    CHECK(log.has(31, false) && !log.has(13, false));
    tk.frame(nullptr, 0);
    CHECK(log.has(13, false));
  }
  // five fingers at once (sorted differently each frame) keep their identities
  {
    KeyLog log; TouchKeys tk(L, [&](uint8_t p, bool d) { log.ev.push_back({p, d}); });
    Pt f[5] = {center(L,5), center(L,6), center(L,7), center(L,8), center(L,9)};  // whole home row
    tk.frame(f, 5);
    CHECK(tk.fingers() == 5);
    for (int p : {10, 11, 12, 13, 14}) CHECK(log.count(p, true) == 1);
    Pt g[5] = {f[4], f[2], f[0], f[3], f[1]};     // reordered, tiny jitter
    for (auto& pt : g) pt.x += 3;
    tk.frame(g, 5);
    CHECK(log.ev.size() == 5);                      // no spurious up/down
    tk.frame(nullptr, 0);
    for (int p : {10, 11, 12, 13, 14}) CHECK(log.count(p, false) == 1);
  }
  // a sixth finger is ignored, not crashed on
  {
    KeyLog log; TouchKeys tk(L, [&](uint8_t p, bool d) { log.ev.push_back({p, d}); });
    Pt f[6]; for (int i = 0; i < 6; i++) f[i] = center(L, i);
    tk.frame(f, 6);
    CHECK(tk.fingers() == 5);
  }
  // same finger tapping the same key twice gives two down/up pairs
  {
    KeyLog log; TouchKeys tk(L, [&](uint8_t p, bool d) { log.ev.push_back({p, d}); });
    Pt c = center(L, 10);
    for (int i = 0; i < 2; i++) { tk.frame(&c, 1); tk.frame(nullptr, 0); }
    CHECK(log.count(20, true) == 2 && log.count(20, false) == 2);
  }

  // panel -> logical mapping: corners and round trip
  {
    int lx, ly;
    panelToLogical(Rot::Deg270, 719, 0, lx, ly);   CHECK(lx == 0 && ly == 0);        // logical top-left = panel top-right
    panelToLogical(Rot::Deg270, 0, 1279, lx, ly);  CHECK(lx == 1279 && ly == 719);
    panelToLogical(Rot::Deg90, 0, 1279, lx, ly);   CHECK(lx == 0 && ly == 0);        // logical top-left = panel bottom-left
    panelToLogical(Rot::Deg90, 719, 0, lx, ly);    CHECK(lx == 1279 && ly == 719);
    for (Rot r : {Rot::Deg90, Rot::Deg270})
      for (int px = 0; px < kPanelW; px += 7)
        for (int py = 0; py < kPanelH; py += 7) {
          panelToLogical(r, px, py, lx, ly);
          CHECK(lx >= 0 && lx < 1280 && ly >= 0 && ly < 720);
        }
  }

  // auto-rotate: needs a clear, steady tilt; ignores flat and wobble
  {
    AutoRotate ar(Rot::Deg270);
    uint32_t t = 1000;
    for (int i = 0; i < 40; i++, t += 50) CHECK(!ar.update(-1000, t) && ar.rot() == Rot::Deg270);   // already upright
    for (int i = 0; i < 40; i++, t += 50) CHECK(!ar.update(300, t));                                // flat-ish: nothing
    for (int i = 0; i < 8; i++, t += 50) CHECK(!ar.update(1000, t));                                // 400 ms: not yet
    CHECK(!ar.update(-1000, t)); t += 50;                                                           // wobble back resets
    bool flipped = false; uint32_t start = t;
    for (int i = 0; i < 30 && !flipped; i++, t += 50) flipped = ar.update(1000, t);
    CHECK(flipped && ar.rot() == Rot::Deg90 && t - start >= 600);
    for (int i = 0; i < 20; i++, t += 50) CHECK(!ar.update(1000, t));                               // stays put
    flipped = false;
    for (int i = 0; i < 30 && !flipped; i++, t += 50) flipped = ar.update(-900, t);
    CHECK(flipped && ar.rot() == Rot::Deg270);
    CHECK(!ar.update(599, t + 5000) && !ar.update(-599, t + 9000));                                 // below threshold
  }

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
