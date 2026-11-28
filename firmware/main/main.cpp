// Tab5 split keyboard.
//  - draws this half's 18 keys on the ST7123 screen as a 1280x720 landscape UI (LVGL rotates it onto the portrait panel)
//  - reads raw multi-touch itself, tracks up to 5 fingers, latches each to the key it landed on
//  LEFT  half: runs the Miryoku engine, sends HID reports to the PC over Bluetooth, and receives the
//              right half's key events over a second BLE link (they go into the same engine)
//  RIGHT half: sends its key down/up events to the left half; shows the layer the left half reports
#include <math.h>
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <cstdio>
#include <cstring>

#include "bsp/m5stack_tab5.h"
#include "esp_lcd_touch.h"
#if __has_include("bsp/touch.h")
#include "bsp/touch.h"   // declares bsp_touch_new()
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "ble_kb.h"
#include "keymap_engine.h"
#include "orientation.h"
#include "autorotate.h"
#include "specials.h"
#include "esp_system.h"
#include "imu.h"
#include "keysound.h"
#include "miryoku_keymap.h"
#include "touch_keys.h"

static const char* TAG = "tab5kb";

// Which half this Tab5 is. One firmware serves both units: it is chosen on the first boot (choose_half) and
// kept in flash. Hold a finger on the screen while powering on to choose again.
static kb::Half kHalf = kb::Half::Left;
static esp_lcd_touch_handle_t g_tp = nullptr;

// Landscape UI. Deg270 turns the logical top toward the Tab5's camera edge; the menuconfig
// "flip" option turns it the other way round.
#ifdef CONFIG_TAB5KB_ROTATE_FLIP
static constexpr kb::Rot kRotStart = kb::Rot::Deg90;
#else
static constexpr kb::Rot kRotStart = kb::Rot::Deg270;
#endif
// The accelerometer flips the picture by 180 degrees when the Tab5 is turned around. The BMI270's axes
// relative to the panel are not documented for the Tab5, so these two settings pick which sensor axis
// lines up with the panel's short side (0=x 1=y 2=z) and whether its sign is reversed. The live readings are
// shown in the status text: with the Tab5 upright in landscape, the axis that reads about +-1000 is the one.
static constexpr bool kAutoRotate = true;
static constexpr int kImuAxis = 0;
static constexpr bool kImuInvert = false;
static volatile kb::Rot g_rot = kRotStart;     // current rotation (touch mapping follows it)
static lv_display_t* g_disp = nullptr;
static volatile int16_t g_imuMg[3] = {0, 0, 0};
static volatile bool g_imuOk = false;
static constexpr int kLogicalW = 1280, kLogicalH = 720;
static constexpr int kKeyRadius = 16;   // rounded key corners
// thumb keys get larger text; use the biggest Montserrat sizes this LVGL build has
#if defined(LV_FONT_MONTSERRAT_36) && LV_FONT_MONTSERRAT_36
#define kThumbBigFont (&lv_font_montserrat_36)
#elif defined(LV_FONT_MONTSERRAT_32) && LV_FONT_MONTSERRAT_32
#define kThumbBigFont (&lv_font_montserrat_32)
#else
#define kThumbBigFont (&lv_font_montserrat_28)
#endif
#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
#define kThumbSmallFont (&lv_font_montserrat_20)
#elif defined(LV_FONT_MONTSERRAT_18) && LV_FONT_MONTSERRAT_18
#define kThumbSmallFont (&lv_font_montserrat_18)
#elif defined(LV_FONT_MONTSERRAT_16) && LV_FONT_MONTSERRAT_16
#define kThumbSmallFont (&lv_font_montserrat_16)
#else
#define kThumbSmallFont (&lv_font_montserrat_14)
#endif
static kb::KeyLayout g_layout(kb::Half::Left, kLogicalW, kLogicalH);   // rebuilt for the chosen half in app_main
static kb::Engine g_engine(kb::kMiryokuKeymap);
static kb::Specials g_specials;   // mouse keys, caps word, Bluetooth profile select

// ---- key-press sounds: each half plays its own touches on its own speaker. The settings live on the left half
// (its keys change them, it saves them) and are sent to the right half over the split link.
static constexpr int kSndRate = 22050;
static constexpr int kSndBlock = 256;   // frames per write (about 12 ms): enough slack to ride out a screen redraw
static kb::KeySynth g_synth(kSndRate);
// The two Tab5 speakers are not equally loud: the right one is turned down to match the left (measured by ear:
// clearly louder, so about -4 dB). Change this one number to tune it; 1.0 leaves a speaker as it is.
static constexpr float kRightSpeakerTrim = 0.6f;
static volatile bool g_sndSave = false;     // settings changed: the audio task writes them to flash (left half)
static volatile bool g_sndPush = false;     // settings changed or link came up: send them to the right half
// Split link byte: bit7 valid (set by ble_kb), bits4-6 sound (0-7), bits0-3 volume (0-10), 15 = muted.
static uint8_t sound_pack() { return uint8_t(((g_synth.kind() & 7) << 4) | (g_synth.on() ? (g_synth.volume() & 0x0F) : 0x0F)); }
static void sound_unpack(uint8_t b) {
  const uint8_t v = b & 0x0F;
  g_synth.setKind((b >> 4) & 7);
  if (v == 0x0F) { g_synth.setOn(false); return; }
  g_synth.setOn(true);
  g_synth.setVolume(v);
}
static void sound_key(uint8_t pos, bool down) { g_synth.keyEvent(pos, down); }
// The Media layer's five RGB keys (unused by this keyboard) are the sound controls.
static bool sound_special(kb::Special s, bool down) {
  switch (s) {
    case kb::Special::RGB_TOG: if (down) g_synth.setOn(!g_synth.on()); break;
    case kb::Special::RGB_EFF: if (down) g_synth.setKind(uint8_t(g_synth.kind() + 1)); break;
    case kb::Special::RGB_HUI: if (down) g_synth.setVolume(g_synth.volume() - 1); break;
    case kb::Special::RGB_SAI: if (down) g_synth.setVolume(g_synth.volume() + 1); break;
    case kb::Special::RGB_BRI: return true;   // "Test": the key's own note is the test
    default: return false;
  }
  if (down) { g_sndSave = true; g_sndPush = true; }
  return true;
}
static kb::KeyLabel label_at(uint8_t layer, int pos) {
  const kb::Binding& b = kb::kMiryokuKeymap.at(layer, pos);
  if (b.kind == kb::Kind::Special) {
    switch (kb::Special(b.a)) {
      case kb::Special::RGB_TOG: return {"Snd", "on/off"};
      case kb::Special::RGB_EFF: return {"Tone", "next"};
      case kb::Special::RGB_HUI: return {"Vol-", ""};
      case kb::Special::RGB_SAI: return {"Vol+", ""};
      case kb::Special::RGB_BRI: return {"Test", ""};
      default: break;
    }
  }
  return kb::kMiryokuKeymap.labelAt(layer, pos);
}

static lv_obj_t* g_keyObj[kb::kHalfKeys];
struct TextLayer { uint8_t* buf = nullptr; uint32_t stride = 0; size_t bytes = 0; int w = 0, h = 0; lv_obj_t* cv = nullptr; };
static TextLayer g_txt[kb::kHalfKeys];        // tilted keys: the key's text, already rotated, drawn above the face
static constexpr int kTmpW = 220, kTmpH = 48;   // scratch picture one label is drawn into before it is rotated
static constexpr int kLayerCount = 10;
static uint8_t* g_txtCache[kLayerCount][kb::kHalfKeys];   // tilted keys' finished text, one picture per layer (upright keys: unused)
static uint8_t* g_txtTmp = nullptr;
static uint32_t g_txtTmpStride = 0;
static lv_obj_t* g_txtTmpCv = nullptr;
static lv_obj_t* g_keyImg[kb::kHalfKeys];    // tilted keys: pre-rendered rotated face (null on upright keys)
static lv_obj_t* g_keyMain[kb::kHalfKeys];   // what a tap does
static lv_obj_t* g_keySub[kb::kHalfKeys];    // what a hold does / layer name
static lv_obj_t* g_keyLeg[kb::kHalfKeys][3];  // small coloured legends: what the key does on the other layers

// Miryoku layer ids (see kMiryokuLayerNames) and their colours. The left half shows the layers whose
// keys live on the left (Num, Sym, Fun, reached with the right thumb); the right half shows Nav, Mouse, Media.
static constexpr uint8_t kLegendLayersLeft[3] = {7, 8, 9};
static constexpr uint8_t kLegendLayersRight[3] = {4, 5, 6};
static uint32_t layer_color(uint8_t layer) {
  switch (layer) {
    case 4: return 0xE8C547;   // Nav    yellow
    case 5: return 0xF08A3C;   // Mouse  orange
    case 6: return 0xB57BEE;   // Media  purple
    case 7: return 0x5B9BFF;   // Num    blue
    case 8: return 0x4CC76B;   // Sym    green
    case 9: return 0xF0605A;   // Fun    red
    default: return 0xFFFFFF;
  }
}
// Thumb keys (positions 30-35) are layer-tap keys: on the base layers their centre text takes the colour of
// the layer they hold. Esc->Media purple, Space->Nav yellow, Tab->Mouse orange, Ent->Sym green, Bksp->Num blue, Del->Fun red.
static uint8_t thumb_hold_layer(int pos) {
  switch (pos) {
    case 30: return 6;
    case 31: return 4;
    case 32: return 5;
    case 33: return 8;
    case 34: return 7;
    case 35: return 9;
    default: return 0;
  }
}
static uint32_t main_text_color(int pos, uint8_t layer) {
  const uint8_t h = (layer <= 3) ? thumb_hold_layer(pos) : 0;
  return layer_color(h ? h : layer);
}
static int g_labelsLayer = -1;               // layer the key labels currently show
static int g_posToIdx[kb::kNumKeys];
static lv_obj_t* g_status;    // status text, first column
static lv_obj_t* g_status2;   // second column

// shared between the touch task (writer) and the UI refresh (same task), so no locking needed
static volatile bool g_pressed[kb::kNumKeys];
static kb::HidReport g_report;
static volatile uint8_t g_layerNow = 0;
static int g_fingers = 0;
static volatile bool g_uiDirty = true;
static volatile int g_bleState = 0;      // HID link to the PC (left half)
static volatile int g_splitState = 0;    // left <-> right link

// Key events from the right half arrive on the BLE host task; the engine lives in the touch task.
struct RemoteEv { uint8_t pos; uint8_t kind; };   // kind: 0 up, 1 down, 2 release everything from the right half
static QueueHandle_t g_remoteQ;
static bool g_remoteDown[kb::kNumKeys];

static uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }

// ---- pairing prompt (numeric comparison): the BLE host task fills these in, the touch task answers
static lv_obj_t* g_pairBox;
static lv_obj_t* g_pairLabel;
static volatile bool g_pairActive = false;
static volatile uint32_t g_pairSince = 0;
static uint32_t g_pairCode = 0;
static char g_pairWho[24] = "";
static constexpr int kYesX = 200, kNoX = 700, kBtnY = 430, kBtnW = 380, kBtnH = 220;
static bool inBox(const kb::Pt& p, int x, int y) {
  return p.x >= x && p.x < x + kBtnW && p.y >= y && p.y < y + kBtnH;
}

// A tilted key face: LVGL's own rotation of widgets crashed at boot, so the rotated rounded rectangle is
// drawn once into an ARGB8888 buffer here (white with an anti-aliased edge) and shown as a canvas; the key
// colour is applied with the image recolor style, which keeps the alpha.
static void tilted_bbox(int angleDeci, int capW, int capH, int& w, int& h) {
  const double a = angleDeci * 3.14159265358979 / 1800.0;
  w = int(ceil(capW * fabs(cos(a)) + capH * fabs(sin(a)))) + 4;
  h = int(ceil(capW * fabs(sin(a)) + capH * fabs(cos(a)))) + 4;
}

static lv_obj_t* make_tilted_face(lv_obj_t* parent, int angleDeci, int capW, int capH, int radius) {
  const float a = angleDeci * 3.14159265f / 1800.0f;
  const float c = cosf(a), sn = sinf(a);
  int w, h;
  tilted_bbox(angleDeci, capW, capH, w, h);
  const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888);
  size_t bytes = size_t(stride) * h;
  bytes = (bytes + 63) & ~size_t(63);
  uint8_t* buf = (uint8_t*)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
  if (!buf) { ESP_LOGE(TAG, "no memory for a tilted key face (%u bytes)", unsigned(bytes)); return nullptr; }
  memset(buf, 0, bytes);
  const float hw = capW / 2.0f, hh = capH / 2.0f;
  for (int y = 0; y < h; y++) {
    uint32_t* row = (uint32_t*)(buf + size_t(y) * stride);
    for (int x = 0; x < w; x++) {
      const float px = x + 0.5f - w / 2.0f, py = y + 0.5f - h / 2.0f;
      const float lx = px * c + py * sn, ly = -px * sn + py * c;     // into the key's own frame
      const float qx = fabsf(lx) - (hw - radius), qy = fabsf(ly) - (hh - radius);
      const float d = hypotf(fmaxf(qx, 0.0f), fmaxf(qy, 0.0f)) + fminf(fmaxf(qx, qy), 0.0f) - radius;
      float cov = 0.5f - d;
      if (cov <= 0) continue;
      if (cov > 1) cov = 1;
      row[x] = (uint32_t(cov * 255.0f + 0.5f) << 24) | 0x00FFFFFF;
    }
  }
  esp_cache_msync(buf, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  lv_obj_t* cv = lv_canvas_create(parent);
  lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_ARGB8888);
  lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_image_recolor_opa(cv, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(cv, lv_color_hex(0x2B2B2B), 0);
  lv_obj_set_pos(cv, 0, 0);   // the parent has exactly the picture's size
  return cv;
}

// ---- text on tilted keys. LVGL cannot rotate a label safely here, so each label is drawn upright into a small
// scratch picture, then turned and scaled (bilinear) into the key's text picture with our own code.
static lv_obj_t* make_text_canvas(lv_obj_t* parent, TextLayer& t, int w, int h) {
  t.w = w; t.h = h;
  t.stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888);
  t.bytes = (size_t(t.stride) * h + 63) & ~size_t(63);
  t.buf = (uint8_t*)heap_caps_aligned_alloc(64, t.bytes, MALLOC_CAP_SPIRAM);
  if (!t.buf) { ESP_LOGE(TAG, "no memory for key text (%u bytes)", unsigned(t.bytes)); return nullptr; }
  memset(t.buf, 0, t.bytes);
  esp_cache_msync(t.buf, t.bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  t.cv = lv_canvas_create(parent);
  lv_canvas_set_buffer(t.cv, t.buf, w, h, LV_COLOR_FORMAT_ARGB8888);
  lv_obj_remove_flag(t.cv, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(t.cv, 0, 0);
  return t.cv;
}

static void make_text_scratch(lv_obj_t* scr) {
  g_txtTmpStride = lv_draw_buf_width_to_stride(kTmpW, LV_COLOR_FORMAT_ARGB8888);
  const size_t bytes = (size_t(g_txtTmpStride) * kTmpH + 63) & ~size_t(63);
  g_txtTmp = (uint8_t*)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
  if (!g_txtTmp) { ESP_LOGE(TAG, "no memory for the text scratch picture"); return; }
  memset(g_txtTmp, 0, bytes);
  g_txtTmpCv = lv_canvas_create(scr);
  lv_canvas_set_buffer(g_txtTmpCv, g_txtTmp, kTmpW, kTmpH, LV_COLOR_FORMAT_ARGB8888);
  lv_obj_add_flag(g_txtTmpCv, LV_OBJ_FLAG_HIDDEN);
}

// Draw one label upright into the scratch picture, then blend it, turned by the key's angle and scaled, into the
// picture `dst` (laid out like t) so that its centre lands at (tx, ty) in the key's own frame (origin = key centre,
// x right, y down, before rotation). Single-precision maths only: the P4's FPU has no double precision.
static void blit_label(uint8_t* dst, const TextLayer& t, float cosA, float sinA, const char* txt,
                       const lv_font_t* font, uint32_t rgb, float tx, float ty, float scale) {
  if (!g_txtTmpCv || !dst || !txt || !txt[0]) return;
  const int lh = lv_font_get_line_height(font);
  lv_point_t sz;
  lv_text_get_size(&sz, txt, font, 0, 0, kTmpW - 4, LV_TEXT_FLAG_NONE);
  const int tw = sz.x < kTmpW - 4 ? sz.x : kTmpW - 4;
  const int ry0 = (kTmpH - lh) / 2 - 1, ry1 = ry0 + lh + 2;                   // scratch rows in use
  const int rx0 = (kTmpW - tw) / 2 - 2, rx1 = rx0 + tw + 4;                    // scratch columns in use
  memset(g_txtTmp + size_t(ry0) * g_txtTmpStride, 0, size_t(ry1 - ry0 + 1) * g_txtTmpStride);
  lv_layer_t layer;
  lv_canvas_init_layer(g_txtTmpCv, &layer);
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.color = lv_color_hex(rgb);
  dsc.font = font;
  dsc.text = txt;
  dsc.align = LV_TEXT_ALIGN_CENTER;
  lv_area_t area;
  area.x1 = 0; area.x2 = kTmpW - 1;
  area.y1 = (kTmpH - lh) / 2; area.y2 = area.y1 + lh - 1;
  lv_draw_label(&layer, &dsc, &area);
  lv_canvas_finish_layer(g_txtTmpCv, &layer);

  // output window: the used part of the scratch picture, turned and scaled, clipped to the text picture
  const float hx = (rx1 - rx0) / 2.0f * scale, hy = (ry1 - ry0) / 2.0f * scale;
  const float cxo = t.w / 2.0f, cyo = t.h / 2.0f;
  // key frame -> picture frame: x = cxo + lx*cos - ly*sin, y = cyo + lx*sin + ly*cos
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  for (int k = 0; k < 4; k++) {
    const float lx = tx + ((k & 1) ? hx : -hx), ly = ty + ((k & 2) ? hy : -hy);
    const float x = cxo + lx * cosA - ly * sinA, y = cyo + lx * sinA + ly * cosA;
    minx = fminf(minx, x);
    maxx = fmaxf(maxx, x);
    miny = fminf(miny, y);
    maxy = fmaxf(maxy, y);
  }
  const int x0 = int(fmaxf(0.0f, floorf(minx))), x1 = int(fminf(float(t.w - 1), ceilf(maxx)));
  const int y0 = int(fmaxf(0.0f, floorf(miny))), y1 = int(fminf(float(t.h - 1), ceilf(maxy)));
  const float inv = 1.0f / scale;
  for (int y = y0; y <= y1; y++) {
    uint32_t* drow = (uint32_t*)(dst + size_t(y) * t.stride);
    for (int x = x0; x <= x1; x++) {
      const float px = x + 0.5f - cxo, py = y + 0.5f - cyo;
      const float lx = px * cosA + py * sinA, ly = -px * sinA + py * cosA;   // into the key frame
      const float sx = (lx - tx) * inv + kTmpW / 2.0f - 0.5f, sy = (ly - ty) * inv + kTmpH / 2.0f - 0.5f;
      const int ix = int(floorf(sx)), iy = int(floorf(sy));
      const float fx = sx - ix, fy = sy - iy;
      float wsum = 0, r = 0, g = 0, b = 0;
      for (int j = 0; j < 4; j++) {
        const int xx = ix + (j & 1), yy = iy + (j >> 1);
        if (xx < rx0 || yy < ry0 || xx >= rx1 || yy > ry1) continue;
        const uint32_t px32 = *(const uint32_t*)(g_txtTmp + size_t(yy) * g_txtTmpStride + size_t(xx) * 4);
        const float wgt = ((j & 1) ? fx : 1 - fx) * ((j >> 1) ? fy : 1 - fy);
        const float a = ((px32 >> 24) & 0xFF) * (1.0f / 255.0f) * wgt;
        wsum += a;
        r += a * float((px32 >> 16) & 0xFF);
        g += a * float((px32 >> 8) & 0xFF);
        b += a * float(px32 & 0xFF);
      }
      if (wsum <= 0.003f) continue;
      const float rw = 1.0f / wsum;
      const float sr = r * rw, sg = g * rw, sb = b * rw;
      const uint32_t d = drow[x];
      const float da = ((d >> 24) & 0xFF) * (1.0f / 255.0f);
      const float keep = da * (1 - wsum);
      const float oa = wsum + keep;
      const float ro = 1.0f / oa;
      const float oR = (sr * wsum + float((d >> 16) & 0xFF) * keep) * ro;
      const float oG = (sg * wsum + float((d >> 8) & 0xFF) * keep) * ro;
      const float oB = (sb * wsum + float(d & 0xFF) * keep) * ro;
      drow[x] = (uint32_t(oa * 255.0f + 0.5f) << 24) | (uint32_t(oR + 0.5f) << 16) | (uint32_t(oG + 0.5f) << 8) |
                uint32_t(oB + 0.5f);
    }
  }
}

// key face colour (works for upright and tilted keys); call with the display lock held
static void key_set_color(int i, uint32_t rgb) {
  static uint32_t last[kb::kHalfKeys];   // only touch LVGL when the colour really changes (a tilted key redraws a big picture)
  static bool init = false;
  if (!init) { for (uint32_t& v : last) v = 0xFFFFFFFF; init = true; }
  if (last[i] == rgb) return;
  last[i] = rgb;
  if (g_keyImg[i]) lv_obj_set_style_image_recolor(g_keyImg[i], lv_color_hex(rgb), 0);
  else lv_obj_set_style_bg_color(g_keyObj[i], lv_color_hex(rgb), 0);
}

static uint8_t* key_text_picture(int i, uint8_t layer);

static void build_ui() {
  for (int& v : g_posToIdx) v = -1;
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  make_text_scratch(scr);
  for (int i = 0; i < kb::kHalfKeys; i++) {
    kb::Rect r = g_layout.rect(i);
    int pos = g_layout.posOfIndex(i);
    g_posToIdx[pos] = i;
    const int capW = g_layout.capW(), capH = g_layout.capH();
    const bool tilted = g_layout.angleDeci(i) != 0;
    lv_obj_t* o = lv_obj_create(scr);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);  // LVGL's own pointer input must not react
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, kKeyRadius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x2B2B2B), 0);
    g_keyImg[i] = nullptr;
    if (tilted) {
      // the container is exactly as big as the rotated picture (so nothing is clipped) and only frames the labels
      int bw, bh;
      tilted_bbox(g_layout.angleDeci(i), capW, capH, bw, bh);
      const kb::Center cc = g_layout.center(i);
      lv_obj_set_pos(o, cc.x - bw / 2, cc.y - bh / 2);
      lv_obj_set_size(o, bw, bh);
      lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
      lv_obj_set_style_radius(o, 0, 0);
      g_keyImg[i] = make_tilted_face(o, g_layout.angleDeci(i), capW, capH, kKeyRadius);
      if (!g_keyImg[i]) lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);   // out of memory: plain box
      make_text_canvas(o, g_txt[i], bw, bh);
    } else {
      lv_obj_set_pos(o, r.x, r.y);
      lv_obj_set_size(o, capW, capH);
      make_text_canvas(o, g_txt[i], capW, capH);   // upright keys draw their text the same way as the tilted ones
    }
    lv_obj_t* m = lv_label_create(o);
    lv_obj_set_style_text_color(m, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_28, 0);
    lv_label_set_text(m, "");
    lv_obj_t* sub = lv_label_create(o);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8FA3B8), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_label_set_text(sub, "");
    const uint8_t* legLayers = (kHalf == kb::Half::Left) ? kLegendLayersLeft : kLegendLayersRight;
    for (int k = 0; k < 3; k++) {
      lv_obj_t* lg = lv_label_create(o);
      lv_obj_set_style_text_color(lg, lv_color_hex(layer_color(legLayers[k])), 0);
      lv_obj_set_style_text_font(lg, &lv_font_montserrat_14, 0);
      lv_label_set_text(lg, "");
      g_keyLeg[i][k] = lg;
    }
    g_keyObj[i] = o;
    g_keyMain[i] = m;
    g_keySub[i] = sub;
  }
  {
    // status text: two columns in the outer bottom corner of the screen, as far from the keys as it goes
    constexpr int kColW = 256, kGap = 8, kEdge = 10;
    const bool left = kHalf == kb::Half::Left;
    lv_obj_t** cols[2] = {&g_status, &g_status2};
    for (int k = 0; k < 2; k++) {
      lv_obj_t* l = lv_label_create(scr);
      lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
      lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
      lv_obj_set_width(l, kColW);
      if (left) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, kEdge + k * (kColW + kGap), -kEdge);
      } else {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_RIGHT, -kEdge - (1 - k) * (kColW + kGap), -kEdge);
      }
      *cols[k] = l;
    }
  }
  lv_label_set_text(g_status, "Tab5 keyboard\nwaiting for touch");
  lv_label_set_text(g_status2, "");
  // draw every layer's text for the tilted keys now (display lock is held), so switching layers later is instant
  for (int layer = 0; layer < kLayerCount; layer++)
    for (int i = 0; i < kb::kHalfKeys; i++)
      if (g_txt[i].cv && g_layout.angleDeci(i) != 0) key_text_picture(i, uint8_t(layer));

  // full-screen pairing prompt, hidden until a pairing request arrives
  g_pairBox = lv_obj_create(scr);
  lv_obj_remove_flag(g_pairBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(g_pairBox, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(g_pairBox, 0, 0);
  lv_obj_set_size(g_pairBox, kLogicalW, kLogicalH);
  lv_obj_set_style_radius(g_pairBox, 0, 0);
  lv_obj_set_style_border_width(g_pairBox, 0, 0);
  lv_obj_set_style_bg_color(g_pairBox, lv_color_hex(0x101010), 0);
  g_pairLabel = lv_label_create(g_pairBox);
  lv_obj_set_width(g_pairLabel, 1120);
  lv_obj_set_pos(g_pairLabel, 80, 70);
  lv_obj_set_style_text_align(g_pairLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(g_pairLabel, lv_color_hex(0xFFFFFF), 0);
  const struct { int x; uint32_t col; const char* txt; } btn[2] = {{kYesX, 0x1E8E3E, "YES"}, {kNoX, 0xC62828, "NO"}};
  for (const auto& b : btn) {
    lv_obj_t* o = lv_obj_create(g_pairBox);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, b.x, kBtnY);
    lv_obj_set_size(o, kBtnW, kBtnH);
    lv_obj_set_style_radius(o, 24, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(b.col), 0);
    lv_obj_t* l = lv_label_create(o);
    lv_label_set_text(l, b.txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
  }
  lv_obj_add_flag(g_pairBox, LV_OBJ_FLAG_HIDDEN);
}

// Draw key i's text for a layer into dst (already cleared).
static void render_key_text(uint8_t* dst, int i, uint8_t layer) {
  const TextLayer& t = g_txt[i];
  const uint8_t* legLayers = (kHalf == kb::Half::Left) ? kLegendLayersLeft : kLegendLayersRight;
  const bool baseLike = layer <= 3;
  const int pos = g_layout.posOfIndex(i);
  const kb::KeyLabel kl = label_at(layer, pos);
  const bool hasSub = kl.sub && kl.sub[0];
  const bool shortMain = kl.main && strlen(kl.main) <= 5;
  const float ang = g_layout.angleDeci(i) * 3.14159265f / 1800.0f, ca = cosf(ang), sa = sinf(ang);
  const float hw = g_layout.capW() / 2.0f, hh = g_layout.capH() / 2.0f;
  const float kMainScale = 1.3f, kSmallScale = 1.45f;   // 28 pt -> about 36 px, 14 pt -> about 20 px
  blit_label(dst, t, ca, sa, kl.main, shortMain ? &lv_font_montserrat_28 : &lv_font_montserrat_14,
             main_text_color(pos, layer), 0, 0, shortMain ? kMainScale : kSmallScale);
  if (hasSub) blit_label(dst, t, ca, sa, kl.sub, &lv_font_montserrat_14, 0x8FA3B8, hw - 42, hh - 20, kSmallScale);
  for (int k = 0; k < 3; k++) {
    const char* lt = baseLike ? label_at(legLayers[k], pos).main : "";
    const float dx = (k == 1) ? hw - 38 : -(hw - 38), dy = (k == 2) ? hh - 20 : -(hh - 20);
    blit_label(dst, t, ca, sa, lt, &lv_font_montserrat_14, layer_color(legLayers[k]), dx, dy, kSmallScale);
  }
  esp_cache_msync(dst, t.bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

// The text picture to show on key i for a layer. Upright keys redraw into their own picture (quick); the tilted
// keys are larger, so each layer's picture is drawn once and kept (switching layers is then instant).
static uint8_t* key_text_picture(int i, uint8_t layer) {
  TextLayer& t = g_txt[i];
  if (!t.buf) return nullptr;
  if (g_layout.angleDeci(i) == 0) {
    memset(t.buf, 0, t.bytes);
    render_key_text(t.buf, i, layer);
    return t.buf;
  }
  if (layer >= kLayerCount) return nullptr;
  if (g_txtCache[layer][i]) return g_txtCache[layer][i];
  uint8_t* pic = (uint8_t*)heap_caps_aligned_alloc(64, t.bytes, MALLOC_CAP_SPIRAM);
  if (!pic) { ESP_LOGE(TAG, "no memory for key text"); return nullptr; }
  memset(pic, 0, t.bytes);
  render_key_text(pic, i, layer);
  g_txtCache[layer][i] = pic;
  return pic;
}

// Write the labels of one layer onto this half's keys (call with the display lock held).
static void apply_labels(uint8_t layer) {
  const uint8_t* legLayers = (kHalf == kb::Half::Left) ? kLegendLayersLeft : kLegendLayersRight;
  const bool baseLike = layer <= 3;                 // Base/Extra/Tap/Button show the other layers' legends
  const lv_align_t legAlign[3] = {LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT};
  for (int i = 0; i < kb::kHalfKeys; i++) {
    const int pos = g_layout.posOfIndex(i);
    kb::KeyLabel kl = label_at(layer, pos);
    const bool hasSub = kl.sub && kl.sub[0];
    const bool shortMain = kl.main && strlen(kl.main) <= 5;
    if (g_txt[i].cv) {
      // the key's text picture: larger than LVGL's own labels, turned with the key when it is tilted
      uint8_t* pic = key_text_picture(i, layer);
      if (pic) {
        lv_canvas_set_buffer(g_txt[i].cv, pic, g_txt[i].w, g_txt[i].h, LV_COLOR_FORMAT_ARGB8888);
        lv_obj_set_pos(g_txt[i].cv, 0, 0);
        lv_obj_invalidate(g_txt[i].cv);
      }
      lv_label_set_text(g_keyMain[i], "");
      lv_label_set_text(g_keySub[i], "");
      for (int k = 0; k < 3; k++) lv_label_set_text(g_keyLeg[i][k], "");
      continue;
    }
    lv_obj_set_style_text_font(g_keyMain[i], shortMain ? &lv_font_montserrat_28 : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_keyMain[i], lv_color_hex(main_text_color(pos, layer)), 0);   // white on base layers except thumbs
    lv_label_set_text(g_keyMain[i], kl.main);
    lv_label_set_text(g_keySub[i], hasSub ? kl.sub : "");
    lv_obj_align(g_keyMain[i], LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(g_keySub[i], LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    for (int k = 0; k < 3; k++) {
      const char* t = baseLike ? label_at(legLayers[k], pos).main : "";
      lv_label_set_text(g_keyLeg[i][k], t ? t : "");
      lv_obj_align(g_keyLeg[i][k], legAlign[k], (k == 1) ? -8 : 8, (k == 2) ? -6 : 6);
    }
  }
  g_labelsLayer = layer;
}

static bool refresh_ui() {   // false when the display was busy; the caller tries again
  if (!bsp_display_lock(50)) return false;
  static bool pairShown = false;
  if (g_pairActive) {
    lv_label_set_text_fmt(g_pairLabel, "Pair with %s?   Code: %06u\n\nTap YES only if the %s shows the same code",
                          g_pairWho, unsigned(g_pairCode), g_pairWho);
    if (!pairShown) { lv_obj_remove_flag(g_pairBox, LV_OBJ_FLAG_HIDDEN); pairShown = true; }
  } else if (pairShown) {
    lv_obj_add_flag(g_pairBox, LV_OBJ_FLAG_HIDDEN);
    pairShown = false;
  }
  const uint8_t layer = g_layerNow;
  if (g_labelsLayer != layer) apply_labels(layer);
  for (int pos = 0; pos < kb::kNumKeys; pos++) {
    int idx = g_posToIdx[pos];
    if (idx < 0) continue;
    const kb::KeyLabel kl = label_at(layer, pos);
    const bool unused = !(kl.main && kl.main[0]) && !(kl.sub && kl.sub[0]);
    key_set_color(idx, g_pressed[pos] ? 0x2E7DFF : (unused ? 0x161616 : 0x2B2B2B));
  }
  char t1[96], t2[96];
  if (kHalf == kb::Half::Left) {
    snprintf(t1, sizeof t1, "LEFT half\nlayer: %s\nPC: %s\nright: %s",
             kb::kMiryokuLayerNames[g_layerNow], ble_kb_state_str((ble_kb_state_t)g_bleState),
             ble_split_state_str((ble_split_state_t)g_splitState));
    char snd[16];
    if (g_synth.on()) snprintf(snd, sizeof snd, " %.3s%d", kb::soundName(g_synth.kind()), g_synth.volume());
    else snprintf(snd, sizeof snd, " mute");
    snprintf(t2, sizeof t2, "BT %d%s%s\nfingers: %d\nmods %02X\nkeys %02X %02X %02X",
             int(ble_kb_profile()) + 1, snd, g_specials.capsWord() ? " CAPS" : "", g_fingers,
             g_report.mods, g_report.keys[0], g_report.keys[1], g_report.keys[2]);
  } else {
    snprintf(t1, sizeof t1, "RIGHT half\nlayer: %s\nleft: %s\nfingers: %d",
             kb::kMiryokuLayerNames[g_layerNow],
             ble_split_state_str((ble_split_state_t)g_splitState), g_fingers);
    char snd[16];
    if (g_synth.on()) snprintf(snd, sizeof snd, " %.3s%d", kb::soundName(g_synth.kind()), g_synth.volume());
    else snprintf(snd, sizeof snd, " mute");
    snprintf(t2, sizeof t2, "mods %02X%s\nkeys %02X %02X %02X", g_report.mods, snd, g_report.keys[0],
             g_report.keys[1], g_report.keys[2]);
  }
  static char last1[96], last2[96];   // setting a label's text redraws it even when nothing changed
  if (strcmp(t1, last1) != 0) { strcpy(last1, t1); lv_label_set_text(g_status, t1); }
  if (strcmp(t2, last2) != 0) { strcpy(last2, t2); lv_label_set_text(g_status2, t2); }
  bsp_display_unlock();
  return true;
}

static void touch_task(void*) {
  esp_lcd_touch_handle_t tp = g_tp;   // created in app_main (the half chooser needs it first)
  if (!tp) {
    ESP_LOGE(TAG, "no touch controller");
    vTaskDelete(nullptr);
  }

  if (kHalf == kb::Half::Left) {
    g_engine.onReport([](const kb::HidReport& raw) {
      const kb::HidReport r = g_specials.filter(raw, nowMs());   // caps word may add Shift
      g_report = r;
      g_uiDirty = true;   // mods/keys in the status text change without any touch event (holds time out)
      ESP_LOGI(TAG, "HID mods=%02X keys=%02X %02X %02X %02X %02X %02X cons=%04X", r.mods, r.keys[0],
               r.keys[1], r.keys[2], r.keys[3], r.keys[4], r.keys[5], r.consumer);
      // left half is the Bluetooth keyboard the PC sees
      static uint16_t lastConsumer = 0;
      const uint8_t b[8] = {r.mods, 0, r.keys[0], r.keys[1], r.keys[2], r.keys[3], r.keys[4], r.keys[5]};
      ble_kb_send_keyboard(b);
      { const uint8_t k3[3] = {r.keys[0], r.keys[1], r.keys[2]}; ble_split_send_report(r.mods, k3); }
      if (r.consumer != lastConsumer) {
        lastConsumer = r.consumer;
        ble_kb_send_consumer(r.consumer);
      }
    });
    g_engine.onLayer([](uint8_t l) {
      g_layerNow = l;
      g_uiDirty = true;
      ESP_LOGI(TAG, "layer -> %s", kb::kMiryokuLayerNames[l]);
      ble_split_send_layer(l);   // so the right half can show it
    });
    g_engine.onSpecial([](kb::Special s, bool down) {
      ESP_LOGI(TAG, "special %u %s", unsigned(s), down ? "down" : "up");
      if (sound_special(s, down)) { g_uiDirty = true; return; }
      g_specials.special(s, down, nowMs());
    });
    g_specials.onMouse([](const kb::MouseReport& m) { ble_kb_send_mouse(m.buttons, m.dx, m.dy, m.wheel, m.pan); });
    g_specials.onAction([](kb::Action a, int n) {
      if (a == kb::Action::Profile) {
        ESP_LOGI(TAG, "bluetooth profile -> %d", n + 1);
        ble_kb_select_profile(uint8_t(n));
        g_uiDirty = true;
      } else {
        ESP_LOGW(TAG, "Boot key: restarting");
        esp_restart();
      }
    });
  }

  kb::TouchKeys keys(g_layout, [](uint8_t pos, bool down) {
    g_pressed[pos] = down;
    ESP_LOGI(TAG, "key %2u %s", pos, down ? "DOWN" : "up");
    if (kHalf == kb::Half::Left) g_engine.keyEvent(pos, down, nowMs());
    else ble_split_send_key(pos, down);
    sound_key(pos, down);   // this half's own speaker
  });

  esp_lcd_touch_point_data_t raw[kb::kMaxFingers];
  bool dirty = true;
  int lastRawCnt = -1;
  bool pairHold = false;
  while (true) {
    // key events from the right half (left half only; the queue does not exist on the right)
    if (g_remoteQ) {
      RemoteEv ev;
      while (xQueueReceive(g_remoteQ, &ev, 0) == pdTRUE) {
        if (ev.kind == 2) {   // link lost: let go of everything the right half was holding
          for (int p = 0; p < kb::kNumKeys; p++)
            if (g_remoteDown[p]) { g_remoteDown[p] = false; g_engine.keyEvent(p, false, nowMs()); }
        } else if (ev.pos < kb::kNumKeys) {
          g_remoteDown[ev.pos] = ev.kind == 1;
          ESP_LOGI(TAG, "right key %2u %s", ev.pos, ev.kind == 1 ? "DOWN" : "up");
          g_engine.keyEvent(ev.pos, ev.kind == 1, nowMs());
        }
      }
    }
    uint8_t cnt = 0;
    if (esp_lcd_touch_read_data(tp) == ESP_OK &&
        esp_lcd_touch_get_data(tp, raw, &cnt, kb::kMaxFingers) == ESP_OK) {
      kb::Pt pts[kb::kMaxFingers];
      for (int i = 0; i < cnt; i++) {
        int lx, ly;
        kb::panelToLogical(g_rot, raw[i].x, raw[i].y, lx, ly);   // raw panel -> landscape UI coordinates
        pts[i] = {int16_t(lx), int16_t(ly)};
      }
      if (cnt != lastRawCnt) {   // log raw coordinates when the finger count changes, to verify axes/range
        lastRawCnt = cnt;
        for (int i = 0; i < cnt; i++) ESP_LOGI(TAG, "raw touch %d/%d: x=%d y=%d", i + 1, cnt, pts[i].x, pts[i].y);
      }
      if (g_pairActive) {
        // a pairing prompt is showing: touches answer it and never reach the keyboard
        keys.frame(pts, 0);
        g_fingers = 0;
        pairHold = true;
        if (nowMs() - g_pairSince > 30000) ble_pair_reply(false);   // nobody answered in time
        for (int i = 0; i < cnt && g_pairActive; i++) {
          if (inBox(pts[i], kYesX, kBtnY)) ble_pair_reply(true);
          else if (inBox(pts[i], kNoX, kBtnY)) ble_pair_reply(false);
        }
      } else if (pairHold) {
        // the prompt just closed: ignore the finger that is still down until it lifts
        keys.frame(pts, 0);
        g_fingers = 0;
        if (cnt == 0) pairHold = false;
      } else {
        keys.frame(pts, cnt);
        g_fingers = keys.fingers();
      }
      dirty = true;
    }
    if (kHalf == kb::Half::Left) { g_engine.tick(nowMs()); g_specials.tick(nowMs()); }
    if (dirty) { g_uiDirty = true; dirty = false; }   // the UI task redraws; this task only decides keys
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// Redraws the screen. Kept out of the touch task so waiting for the display never delays a key decision.
static void ui_task(void*) {
  for (;;) {
    if (g_uiDirty) {
      g_uiDirty = false;
      if (!refresh_ui()) g_uiDirty = true;   // display busy: try again
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

// Reads the accelerometer and turns the picture (and touch mapping) when the Tab5 is turned around.
static void imu_task(void*) {
  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  if (!bus) { bsp_i2c_init(); bus = bsp_i2c_get_handle(); }
  const int rc = imu_init(bus);
  if (rc != 0) {
    ESP_LOGW(TAG, "IMU not available (%d): staying in the fixed orientation", rc);
    vTaskDelete(nullptr);
    return;
  }
  g_imuOk = true;
  kb::AutoRotate ar(g_rot);
  bool pendingRot = false;
  uint32_t lastLog = 0;
  for (;;) {
    int16_t v[3];
    if (imu_read_mg(&v[0], &v[1], &v[2]) == 0) {
      for (int i = 0; i < 3; i++) g_imuMg[i] = v[i];
      const int g = kImuInvert ? -v[kImuAxis] : v[kImuAxis];
      const uint32_t t = nowMs();
      if (ar.update(g, t)) {
        ESP_LOGI(TAG, "turned: now %s (accel %d %d %d mg)", ar.rot() == kb::Rot::Deg90 ? "Deg90" : "Deg270", v[0], v[1], v[2]);
        pendingRot = true;
      }
      if (pendingRot && bsp_display_lock(200)) {   // retried until it works: the picture and touch must change together
        const kb::Rot want = ar.rot();
        bsp_display_rotate(g_disp, want == kb::Rot::Deg90 ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_270);
        bsp_display_unlock();
        g_rot = want;
        g_uiDirty = true;
        pendingRot = false;
      }
      if (t - lastLog > 3000) { lastLog = t; ESP_LOGI(TAG, "accel mg: x=%d y=%d z=%d", v[0], v[1], v[2]); }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Plays the key sounds. Writes only while something sounds, so the speaker stays silent otherwise.
static void sound_load_save(bool save) {
  if (save) {
    ble_kb_setting_set("snd_on", g_synth.on() ? 1 : 0);
    ble_kb_setting_set("snd_vol", uint8_t(g_synth.volume()));
    ble_kb_setting_set("snd_kind", g_synth.kind());
  } else {
    uint8_t v;
    if (ble_kb_setting_get("snd_on", &v)) g_synth.setOn(v != 0);
    if (ble_kb_setting_get("snd_vol", &v)) g_synth.setVolume(v);
    if (ble_kb_setting_get("snd_kind", &v)) g_synth.setKind(v);
  }
}

static void audio_task(void*) {
  bsp_feature_enable(BSP_FEATURE_SPEAKER, true);
  esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
  if (!spk) {
    ESP_LOGE(TAG, "speaker init failed: no key sounds");
    vTaskDelete(nullptr);
    return;
  }
  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = 2;
  fs.sample_rate = kSndRate;
  if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "speaker open failed: no key sounds");
    vTaskDelete(nullptr);
    return;
  }
  esp_codec_dev_set_out_vol(spk, 80);   // the codec stays at a fixed level; the volume keys scale the samples
  g_synth.setTrim(kHalf == kb::Half::Right ? kRightSpeakerTrim : 1.0f);
  ESP_LOGI(TAG, "key sounds ready");
  bool loaded = false;
  uint32_t t0 = nowMs();
  int16_t mono[kSndBlock], st[2 * kSndBlock];
  for (;;) {
    if (kHalf == kb::Half::Left) {   // the left half owns the settings; the right half follows
      if (!loaded && nowMs() - t0 > 4000) { loaded = true; sound_load_save(false); g_uiDirty = true; g_sndPush = true; }   // flash is up by now
      if (g_sndSave) { g_sndSave = false; if (loaded) sound_load_save(true); }
      if (g_sndPush) { g_sndPush = false; ble_split_send_sound(sound_pack()); }
    }
    if (!g_synth.busy()) { vTaskDelay(pdMS_TO_TICKS(3)); continue; }
    g_synth.render(mono, kSndBlock);
    for (int i = 0; i < kSndBlock; i++) st[2 * i] = st[2 * i + 1] = mono[i];
    esp_codec_dev_write(spk, st, sizeof st);
  }
}

// First boot (nothing saved) or a finger held on the screen at power-on: ask which half this Tab5 is and keep the
// answer in flash. Runs before the keyboard UI exists; the answer sets kHalf. Touches are read raw, like the keys.
static kb::Half choose_half(esp_lcd_touch_handle_t tp) {
  uint8_t saved = 0;
  const bool have = ble_kb_setting_get("half", &saved) && (saved == 1 || saved == 2);
  bool ask = !have;
  if (have && tp) {   // a finger on the screen right after power-on asks again
    vTaskDelay(pdMS_TO_TICKS(400));
    for (int i = 0; i < 5 && !ask; i++) {
      uint8_t n = 0;
      esp_lcd_touch_point_data_t p[1];
      if (esp_lcd_touch_read_data(tp) == ESP_OK && esp_lcd_touch_get_data(tp, p, &n, 1) == ESP_OK && n > 0) ask = true;
      vTaskDelay(pdMS_TO_TICKS(60));
    }
  }
  if (!ask) return saved == 2 ? kb::Half::Right : kb::Half::Left;

  const struct { int x; uint32_t col; const char* txt; } btn[2] = {{kYesX, 0x2E6FD8, "LEFT"}, {kNoX, 0xE0872B, "RIGHT"}};
  if (bsp_display_lock(0)) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);
    lv_obj_t* t = lv_label_create(scr);
    lv_obj_set_width(t, 1120);
    lv_obj_set_pos(t, 80, 70);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_label_set_text(t, "Which half is this Tab5?\n\nTap one. It is remembered.\n"
                         "To change it later, hold a finger on the screen while powering on.");
    for (const auto& b : btn) {
      lv_obj_t* o = lv_obj_create(scr);
      lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_pos(o, b.x, kBtnY);
      lv_obj_set_size(o, kBtnW, kBtnH);
      lv_obj_set_style_radius(o, 24, 0);
      lv_obj_set_style_border_width(o, 0, 0);
      lv_obj_set_style_bg_color(o, lv_color_hex(b.col), 0);
      lv_obj_t* l = lv_label_create(o);
      lv_label_set_text(l, b.txt);
      lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_text_font(l, kThumbBigFont, 0);
      lv_obj_center(l);
    }
    bsp_display_unlock();
  }
  kb::Half pick = kb::Half::Left;
  bool done = false;
  while (!done) {
    uint8_t n = 0;
    esp_lcd_touch_point_data_t p[kb::kMaxFingers];
    if (tp && esp_lcd_touch_read_data(tp) == ESP_OK && esp_lcd_touch_get_data(tp, p, &n, kb::kMaxFingers) == ESP_OK) {
      for (int i = 0; i < n && !done; i++) {
        int lx, ly;
        kb::panelToLogical(g_rot, p[i].x, p[i].y, lx, ly);
        const kb::Pt pt = {int16_t(lx), int16_t(ly)};
        if (inBox(pt, kYesX, kBtnY)) { pick = kb::Half::Left; done = true; }
        else if (inBox(pt, kNoX, kBtnY)) { pick = kb::Half::Right; done = true; }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  ble_kb_setting_set("half", pick == kb::Half::Right ? 2 : 1);
  ESP_LOGI(TAG, "half chosen: %s", pick == kb::Half::Right ? "right" : "left");
  // wait for the finger to lift so it does not land on a key, then clear the screen
  for (int i = 0; i < 200; i++) {
    uint8_t n = 0;
    esp_lcd_touch_point_data_t p[1];
    if (tp && esp_lcd_touch_read_data(tp) == ESP_OK && esp_lcd_touch_get_data(tp, p, &n, 1) == ESP_OK && n == 0) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (bsp_display_lock(0)) {
    lv_obj_clean(lv_screen_active());
    bsp_display_unlock();
  }
  return pick;
}

extern "C" void app_main(void) {
  lv_display_t* disp = bsp_display_start();
  g_disp = disp;
  if (!disp) {
    ESP_LOGE(TAG, "bsp_display_start failed");
    return;
  }
  bsp_display_backlight_on();
  if (bsp_display_lock(0)) {
    bsp_display_rotate(disp, g_rot == kb::Rot::Deg90 ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_270);
    bsp_display_unlock();
  }
  if (bsp_touch_new(nullptr, &g_tp) != ESP_OK) ESP_LOGE(TAG, "bsp_touch_new failed");
  kHalf = choose_half(g_tp);
  g_layout = kb::KeyLayout(kHalf, kLogicalW, kLogicalH);
  if (bsp_display_lock(0)) {
    build_ui();
    bsp_display_unlock();
  }
  if (kHalf == kb::Half::Left) g_remoteQ = xQueueCreate(64, sizeof(RemoteEv));
  if (g_disp) lv_timer_set_period(lv_display_get_refr_timer(g_disp), 10);   // default is 33 ms
  xTaskCreate(touch_task, "touch", 8192, nullptr, 5, nullptr);
  xTaskCreatePinnedToCore(ui_task, "ui", 8192, nullptr, 4, nullptr, 0);   // redraws stay off the audio core
  if (kAutoRotate) xTaskCreate(imu_task, "imu", 6144, nullptr, 3, nullptr);
  ESP_LOGI(TAG, "ready: %s half", kHalf == kb::Half::Left ? "left" : "right");

  // The C6 radio is powered through the Tab5's second I/O expander. The board package leaves it
  // alone, so switch it on explicitly before ESP-Hosted tries to talk to the C6.
  esp_err_t pe = bsp_feature_enable(BSP_FEATURE_WIFI, true);
  if (pe != ESP_OK) ESP_LOGW(TAG, "C6 power enable failed: %s", esp_err_to_name(pe));
  vTaskDelay(pdMS_TO_TICKS(500));
  // The audio task outranks everything else and has its own core, so a screen redraw (layer change) cannot starve it.
  xTaskCreatePinnedToCore(audio_task, "audio", 6144, nullptr, 10, nullptr, 1);

  ble_pair_set_callback([](const char* who, uint32_t code, bool active) {
    if (active) {
      snprintf(g_pairWho, sizeof g_pairWho, "%s", who);
      g_pairCode = code;
      g_pairSince = nowMs();
      g_pairActive = true;
    } else {
      g_pairActive = false;
    }
    g_uiDirty = true;
  });

  // These run on the BLE host task, so they only hand work to the touch task.
  ble_split_set_callbacks(
      [](ble_split_state_t st) {
        g_splitState = st;
        g_uiDirty = true;
        if (st == BLE_SPLIT_LINKED) g_sndPush = true;   // left: give the right half the sound settings
        if (st != BLE_SPLIT_LINKED && g_remoteQ) {   // right half gone: release its keys
          RemoteEv ev = {0, 2};
          xQueueSend(g_remoteQ, &ev, 0);
        }
      },
      [](uint8_t pos, bool down) {
        if (!g_remoteQ) return;
        RemoteEv ev = {pos, uint8_t(down ? 1 : 0)};
        if (xQueueSend(g_remoteQ, &ev, 0) != pdTRUE) ESP_LOGW(TAG, "remote key queue full");
      },
      [](uint8_t layer) {
        if (layer < 10) g_layerNow = layer;   // right half: shown in the status band
        g_uiDirty = true;
      });
  ble_split_set_sound_cb([](uint8_t packed) {   // right half: sound settings from the left half
    sound_unpack(packed);
    g_uiDirty = true;
  });
  ble_split_set_report_cb([](uint8_t mods, const uint8_t keys[3]) {   // right half: left's modifiers and keys
    g_report.mods = mods;
    for (int i = 0; i < 3; i++) g_report.keys[i] = keys[i];
    g_uiDirty = true;
  });

  // Blocks a few seconds while the C6 starts; the touch task is already running.
  const bool left = kHalf == kb::Half::Left;
  int rc = ble_kb_start(left ? "Tab5 KB L" : "Tab5 KB R", left ? BLE_KB_ROLE_LEFT : BLE_KB_ROLE_RIGHT,
                        [](ble_kb_state_t st) {
                          g_bleState = st;
                          g_uiDirty = true;
                        });
  if (rc != 0) ESP_LOGE(TAG, "ble_kb_start failed: %d", rc);
}
