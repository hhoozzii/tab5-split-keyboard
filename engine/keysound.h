// Key-press sounds: a tiny polyphonic synthesiser. Hardware independent: key events go in (from one task),
// render() fills 16-bit mono samples (called from the audio task).
//  One-shot sounds (press only): Click, Tick (soft, high), Clack (typewriter).
//  Tones (pitch follows the key, they fade out on release):
//   Piano (settles to a held level), Organ (steady while held), Marimba / Bell / Pluck (ring out and die away).
// Pitch follows the key position: columns climb a C major scale, the top row is the highest.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace kb {

enum class SoundKind : uint8_t { Click = 0, Piano = 1, Organ = 2, Marimba = 3, Bell = 4, Tick = 5, Clack = 6, Pluck = 7 };
constexpr int kSoundKinds = 8;   // the split link carries the kind in 3 bits: do not add more
constexpr int kMaxVolume = 10;

inline const char* soundName(uint8_t k) {
  static const char* const n[kSoundKinds] = {"Click", "Piano", "Organ", "Marimba", "Bell", "Tick", "Clack", "Pluck"};
  return k < kSoundKinds ? n[k] : "?";
}

// MIDI note for a key position (0-9 top row, 10-19 home, 20-29 bottom, 30-35 thumbs).
inline int noteForPos(int pos) {
  static const int major[7] = {0, 2, 4, 5, 7, 9, 11};
  int step;
  if (pos >= 30) step = 1 + (pos - 30);                           // the small speakers cannot play low notes:
  else step = (pos % 10) + 4 * (1 - pos / 10);                     // the lowest key sounds about 350 Hz
  const int oct = (step >= 0) ? step / 7 : -((-step + 6) / 7);
  const int deg = step - oct * 7;
  return 72 + 12 * oct + major[deg];
}

// Tonal voices: additive partials. tauP = decay time of each partial (0 = none); gtau/floor shape the overall level
// (level = floor + (1-floor) * exp(-t/gtau), gtau 0 = flat); rel = fade-out after the key is released;
// maxT = a ringing sound is dropped after this long (0 = never).
struct ToneDef {
  float ratio[5], amp[5], tauP[5];
  float attack, gtau, floor, rel, maxT;
};
inline const ToneDef& toneDef(uint8_t kind) {
  static const ToneDef piano  = {{1, 2, 3, 4, 5}, {1.0f, 0.75f, 0.45f, 0.25f, 0.12f}, {0, 0.5f, 0.25f, 0.1667f, 0.125f}, 0.002f, 0.30f, 0.45f, 0.18f, 0};
  static const ToneDef organ  = {{1, 2, 3, 4, 5}, {1.0f, 0.45f, 0.30f, 0.20f, 0.0f},  {0, 0, 0, 0, 0},                  0.008f, 0, 0, 0.18f, 0};
  static const ToneDef marimba = {{1, 4, 10, 0, 0}, {1.0f, 0.5f, 0.15f, 0, 0},       {0.40f, 0.09f, 0.03f, 0, 0},      0.001f, 0, 0, 0.10f, 1.6f};
  static const ToneDef bell   = {{1, 2.0f, 2.76f, 5.4f, 8.93f}, {1.0f, 0.55f, 0.45f, 0.25f, 0.12f}, {1.6f, 1.0f, 0.7f, 0.4f, 0.25f}, 0.001f, 0, 0, 0.35f, 4.5f};
  static const ToneDef pluck  = {{1, 2, 3, 4, 5}, {1.0f, 0.6f, 0.4f, 0.25f, 0.15f}, {0.7f, 0.35f, 0.22f, 0.15f, 0.1f}, 0.001f, 0, 0, 0.10f, 2.6f};
  switch (SoundKind(kind)) {
    case SoundKind::Organ: return organ;
    case SoundKind::Marimba: return marimba;
    case SoundKind::Bell: return bell;
    case SoundKind::Pluck: return pluck;
    default: return piano;
  }
}
inline bool isShot(uint8_t kind) { return kind == uint8_t(SoundKind::Click) || kind == uint8_t(SoundKind::Tick) || kind == uint8_t(SoundKind::Clack); }
// One-shot sounds: a decaying sine plus a decaying noise burst.
struct ShotDef { float freq, tauSin, aSin, tauNoise, aNoise, gain, len; };
inline const ShotDef& shotDef(uint8_t kind) {
  static const ShotDef click = {2300.0f, 0.0035f, 0.55f, 0.0035f, 0.35f, 2.2f, 0.030f};
  static const ShotDef tick  = {3300.0f, 0.0018f, 0.60f, 0.0010f, 0.06f, 1.8f, 0.014f};
  static const ShotDef clack = {950.0f,  0.0050f, 0.50f, 0.0040f, 0.55f, 2.4f, 0.045f};
  return kind == uint8_t(SoundKind::Tick) ? tick : (kind == uint8_t(SoundKind::Clack) ? clack : click);
}

class KeySynth {
 public:
  explicit KeySynth(int sampleRate = 22050) : sr_(sampleRate) {
    for (int i = 0; i < kTab; i++) tab_[i] = sinf(6.28318530718f * i / kTab);
  }

  // ---- settings (any task)
  void setOn(bool on) { on_ = on; }
  bool on() const { return on_; }
  void setVolume(int v) { vol_ = v < 0 ? 0 : (v > kMaxVolume ? kMaxVolume : v); }
  int volume() const { return vol_; }
  void setKind(uint8_t k) { kind_ = k % kSoundKinds; }
  void setTrim(float t) { trim_ = t; }   // fixed per-speaker level match (1.0 = none)
  uint8_t kind() const { return kind_; }

  // ---- key events (one producer task)
  void keyEvent(uint8_t pos, bool down) {
    if (!on_ || vol_ == 0) { if (down) return; }   // a release must still reach held tones
    push(pos, down);
  }
  // true while something is sounding or queued (the audio task idles otherwise)
  bool busy() const {
    if (qh_.load() != qt_.load()) return true;
    for (const Voice& v : v_) if (v.active) return true;
    return false;
  }

  // ---- audio task: fill n mono samples
  void render(int16_t* out, int n) {
    drain();
    const float master = gain();
    for (int i = 0; i < n; i++) {
      float s = 0;
      for (Voice& v : v_) if (v.active) s += voiceSample(v);
      s *= master;
      s = s / (1.0f + fabsf(s));   // soft limiter
      out[i] = int16_t(s * 30000.0f);
    }
  }

 private:
  static constexpr int kTab = 512, kVoices = 8, kPartials = 5, kQ = 64;
  struct Ev { uint8_t pos; bool down; };
  struct Voice {
    bool active = false, held = false;
    uint8_t pos = 0, kind = 0;
    uint32_t age = 0, relAge = 0, serial = 0;
    float ph[kPartials] = {0};
    float inc[kPartials] = {0};
    float amp[kPartials] = {0};
    float rnd = 1;
  };

  float gain() const {
    const float x = float(vol_) / kMaxVolume;
    return 0.9f * x * sqrtf(x) * trim_;
  }
  void push(uint8_t pos, bool down) {
    const uint8_t h = qh_.load(), nh = (h + 1) % kQ;
    if (nh == qt_.load()) return;   // full: drop
    q_[h] = {pos, down};
    qh_.store(nh);
  }
  void drain() {
    while (qt_.load() != qh_.load()) {
      const Ev e = q_[qt_.load()];
      qt_.store((qt_.load() + 1) % kQ);
      if (e.down) start(e.pos); else stop(e.pos);
    }
  }
  void start(uint8_t pos) {
    if (!on_ || vol_ == 0) return;
    Voice* slot = nullptr;
    for (Voice& v : v_) if (!v.active) { slot = &v; break; }
    if (!slot) { slot = &v_[0]; for (Voice& v : v_) if (v.serial < slot->serial) slot = &v; }
    Voice& v = *slot;
    v = Voice();
    v.active = true; v.held = true; v.pos = pos; v.kind = kind_; v.serial = ++serial_;
    if (isShot(v.kind)) { seed_ = seed_ * 1664525u + 1013904223u; v.rnd = 0.9f + 0.2f * ((seed_ >> 16) & 0xFF) / 255.0f; return; }
    const float f = 440.0f * powf(2.0f, (noteForPos(pos) - 69) / 12.0f);
    const ToneDef& d = toneDef(v.kind);
    for (int k = 0; k < kPartials; k++) {
      v.inc[k] = f * d.ratio[k] / sr_;
      if (d.amp[k] == 0 || v.inc[k] >= 0.45f) v.amp[k] = 0;   // above ~Nyquist/2: leave out
      else v.amp[k] = d.amp[k] * 0.22f;
    }
  }
  void stop(uint8_t pos) {
    for (Voice& v : v_) if (v.active && v.held && v.pos == pos && !isShot(v.kind)) v.held = false;
  }
  float voiceSample(Voice& v) {
    const float t = v.age / float(sr_);
    if (isShot(v.kind)) {
      const ShotDef& d = shotDef(v.kind);
      seed_ = seed_ * 1664525u + 1013904223u;
      const float noise = (int32_t(seed_) >> 8) * (1.0f / 8388608.0f);
      const float s = (d.aSin * expf(-t / d.tauSin) * sinf(6.28318530718f * d.freq * v.rnd * t) +
                       d.aNoise * expf(-t / d.tauNoise) * noise) * d.gain;
      if (++v.age > uint32_t(d.len * sr_)) v.active = false;
      return s;
    }
    const ToneDef& d = toneDef(v.kind);
    if (d.maxT > 0 && t > d.maxT) { v.active = false; return 0; }
    float env = t < d.attack ? t / d.attack : 1.0f;
    if (d.gtau > 0) env *= d.floor + (1.0f - d.floor) * expf(-t / d.gtau);   // struck, then settles to a held level
    if (!v.held) {
      const float r = v.relAge / (d.rel * sr_);
      if (r >= 1.0f) { v.active = false; return 0; }
      env *= 1.0f - r;
      v.relAge++;
    }
    float s = 0;
    for (int k = 0; k < kPartials; k++) {
      if (v.amp[k] == 0) continue;
      float a = v.amp[k];
      if (d.tauP[k] > 0) a *= expf(-t / d.tauP[k]);   // each partial dies away at its own rate
      s += a * tab_[int(v.ph[k] * kTab) & (kTab - 1)];
      v.ph[k] += v.inc[k];
      if (v.ph[k] >= 1.0f) v.ph[k] -= 1.0f;
    }
    v.age++;
    return s * env;
  }

  int sr_;
  float tab_[kTab];
  Voice v_[kVoices];
  uint32_t serial_ = 0, seed_ = 12345;
  std::atomic<bool> on_{true};
  std::atomic<int> vol_{7};
  float trim_ = 1.0f;
  std::atomic<uint8_t> kind_{uint8_t(SoundKind::Click)};
  Ev q_[kQ];
  std::atomic<uint8_t> qh_{0}, qt_{0};
};

}  // namespace kb
