// On-screen layout for ONE half (18 keys) of the 36-key split keyboard, in the Tab5's landscape
// UI coordinates (1280 x 720), modelled on the Miryoku ZMK diagram:
//   - 5 columns x 3 rows, columns staggered (pinky lowest, middle finger highest)
//   - 3 thumb keys on an arc under the inner columns: the outer one is level, the next two tilt
//     further and sit lower (13 and 25 degrees; clockwise on the left half, counter-clockwise on
//     the right half)
//   - the cluster sits against the screen edge that faces the other half; the free space below the
//     outer columns (left of the thumbs) holds the status text.
// Keys are stretched: the horizontal pitch is a fixed pixel size (about 19 mm on the Tab5 panel), the
// vertical pitch is whatever lets four rows plus the lowered thumbs fit the screen height.
// hit() tests rotated key shapes, so the tilted thumbs are touched where they are drawn.
#pragma once
#include <cmath>
#include <cstdint>

namespace kb {

enum class Half : uint8_t { Left, Right };
constexpr int kHalfKeys = 18;  // index 0-14 = row*5+col, 15-17 = thumbs

struct Rect { int x, y, w, h; };
struct Center { int x, y; };

// ---- layout constants, left half. Positions are in 1/1000 of a key pitch (x: horizontal pitch,
// y: vertical pitch). Origin = centre of column 2, row 0.
// Column drop below the highest column, outer (pinky) -> inner (index).
constexpr int kColDrop[5] = {500, 130, 0, 130, 260};
// Thumb keys outer -> inner: centre x, centre y, tilt in 1/10 degree (clockwise).
struct ThumbSpec { int x, y, angleDeci; };
constexpr ThumbSpec kThumb[3] = {{2500, 3150, 0}, {3630, 3270, 130}, {4640, 3500, 250}};
constexpr int kCapPermille = 940;      // drawn key size / pitch
constexpr int kDefaultPitchXpx = 220;  // horizontal key pitch in pixels (19 mm at 0.0865 mm per pixel)

class KeyLayout {
 public:
  explicit KeyLayout(Half half, int screenW = 1280, int screenH = 720, int margin = 10,
                     int pitchXpx = kDefaultPitchXpx)
      : half_(half), w_(screenW), h_(screenH), m_(margin), px_(pitchXpx) {
    const bool right = half == Half::Right;
    for (int i = 0; i < kHalfKeys; i++) {
      if (i < 15) {
        const int uc = right ? 4 - (i % 5) : (i % 5);           // column in left-half units
        ux_[i] = uc * 1000; uy_[i] = kColDrop[uc] + (i / 5) * 1000; ang_[i] = 0;
      } else {
        const int t = right ? 2 - (i - 15) : (i - 15);        // right half: idx 15 is the inner key
        ux_[i] = kThumb[t].x; uy_[i] = kThumb[t].y; ang_[i] = kThumb[t].angleDeci;
      }
      if (right) { ux_[i] = 4000 - ux_[i]; ang_[i] = -ang_[i]; }
      const double rad = ang_[i] * 3.14159265358979 / 1800.0;
      cos_[i] = float(std::cos(rad));
      sin_[i] = float(std::sin(rad));
    }
    // horizontal extent at the wanted pitch; shrink the pitch if the cluster would not fit the width
    double minx, maxx, miny, maxy;
    extents(px_, 150.0, minx, maxx, miny, maxy);
    while (maxx - minx > w_ - 2 * m_ && px_ > 100) { px_--; extents(px_, 150.0, minx, maxx, miny, maxy); }
    // largest vertical pitch whose rotated keys still fit the screen height
    double lo = 40.0, hi = 400.0;
    for (int it = 0; it < 40; it++) {
      const double mid = 0.5 * (lo + hi);
      extents(px_, mid, minx, maxx, miny, maxy);
      if (maxy - miny <= h_ - 2 * m_) lo = mid; else hi = mid;
    }
    py_ = int(std::floor(lo));
    extents(px_, py_, minx, maxx, miny, maxy);
    ox_ = right ? m_ - int(std::floor(minx)) : (w_ - m_) - int(std::ceil(maxx));
    oy_ = m_ - int(std::floor(miny));
    clL_ = ox_ + int(std::floor(minx));
    clR_ = ox_ + int(std::ceil(maxx));
    for (int i = 0; i < kHalfKeys; i++) {
      cx_[i] = ox_ + ux_[i] * px_ / 1000;
      cy_[i] = oy_ + uy_[i] * py_ / 1000;
    }
  }

  // Touch point -> global key position 0..35 (Miryoku order), or -1 outside the cluster.
  int hit(int x, int y) const;
  // Local index 0..17 -> global key position 0..35.
  int posOfIndex(int idx) const;
  Center center(int idx) const { return {cx_[idx], cy_[idx]}; }
  int angleDeci(int idx) const { return ang_[idx]; }       // clockwise, 1/10 degree
  int pitchX() const { return px_; }
  int pitchY() const { return py_; }
  int capW() const { return px_ * kCapPermille / 1000; }   // drawn key size (before rotation)
  int capH() const { return py_ * kCapPermille / 1000; }
  Rect rect(int idx) const {                               // unrotated drawn rectangle
    const int cw = capW(), ch = capH();
    return {cx_[idx] - cw / 2, cy_[idx] - ch / 2, cw, ch};
  }
  // Empty area under the outer columns, beside the thumb keys, for the status text.
  Rect statusRect() const;
  int clusterLeft() const { return clL_; }
  int clusterRight() const { return clR_; }

 private:
  // Bounding box (relative to ox = 0, oy = 0) of all rotated key faces for a pair of pitches.
  void extents(double px, double py, double& minx, double& maxx, double& miny, double& maxy) const {
    minx = miny = 1e30; maxx = maxy = -1e30;
    const double hw = px * kCapPermille / 2000.0, hh = py * kCapPermille / 2000.0;
    for (int i = 0; i < kHalfKeys; i++) {
      const double ex = std::fabs(cos_[i]) * hw + std::fabs(sin_[i]) * hh;
      const double ey = std::fabs(sin_[i]) * hw + std::fabs(cos_[i]) * hh;
      const double cx = ux_[i] * px / 1000.0, cy = uy_[i] * py / 1000.0;
      if (cx - ex < minx) minx = cx - ex;
      if (cx + ex > maxx) maxx = cx + ex;
      if (cy - ey < miny) miny = cy - ey;
      if (cy + ey > maxy) maxy = cy + ey;
    }
  }

  Half half_;
  int w_, h_, m_, px_, py_ = 0, ox_ = 0, oy_ = 0, clL_ = 0, clR_ = 0;
  int ux_[kHalfKeys], uy_[kHalfKeys], cx_[kHalfKeys], cy_[kHalfKeys], ang_[kHalfKeys];
  float cos_[kHalfKeys], sin_[kHalfKeys];
};

}  // namespace kb
