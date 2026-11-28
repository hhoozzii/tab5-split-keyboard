#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_hosted.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "ble_kb.h"

static const char *TAG = "ble_kb";

void ble_store_config_init(void);

/* ---------------------------------------------------------------- security */

static ble_pair_cb_t g_pair_cb;
static uint16_t g_pair_conn = BLE_HS_CONN_HANDLE_NONE;
static bool g_pair_active;
static bool g_split_secure;   // the other half is connected, authenticated and encrypted

void ble_pair_set_callback(ble_pair_cb_t cb) { g_pair_cb = cb; }

static void pair_show(uint16_t conn, uint32_t code, const char *who) {
  g_pair_conn = conn;
  g_pair_active = true;
  ESP_LOGI(TAG, "pairing with %s: code %06" PRIu32, who, code);
  if (g_pair_cb) g_pair_cb(who, code, true);
}

static void pair_dismiss(uint16_t conn) {
  if (!g_pair_active || conn != g_pair_conn) return;
  g_pair_active = false;
  g_pair_conn = BLE_HS_CONN_HANDLE_NONE;
  if (g_pair_cb) g_pair_cb(NULL, 0, false);
}

void ble_pair_reply(bool accept) {
  if (!g_pair_active) return;
  uint16_t conn = g_pair_conn;
  g_pair_active = false;
  g_pair_conn = BLE_HS_CONN_HANDLE_NONE;
  struct ble_sm_io io;
  memset(&io, 0, sizeof io);
  io.action = BLE_SM_IOACT_NUMCMP;
  io.numcmp_accept = accept ? 1 : 0;
  int rc = ble_sm_inject_io(conn, &io);
  ESP_LOGI(TAG, "pairing %s (rc=%d)", accept ? "accepted" : "rejected", rc);
  if (g_pair_cb) g_pair_cb(NULL, 0, false);
}

static void on_passkey(const struct ble_gap_event *event, const char *who) {
  if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
    pair_show(event->passkey.conn_handle, event->passkey.params.numcmp, who);
  } else {
    ESP_LOGW(TAG, "unsupported pairing action %d from %s: dropping", event->passkey.params.action, who);
    ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
  }
}

// The other half's address is remembered after the first authenticated pairing; from then on
// only that device is accepted.
static bool peer_load(const char *key, uint8_t out[6]) {
  nvs_handle_t h;
  if (nvs_open("tab5kb", NVS_READONLY, &h) != ESP_OK) return false;
  size_t n = 6;
  bool ok = nvs_get_blob(h, key, out, &n) == ESP_OK && n == 6;
  nvs_close(h);
  return ok;
}

static void peer_save(const char *key, const uint8_t v[6]) {
  nvs_handle_t h;
  if (nvs_open("tab5kb", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, key, v, 6);
  nvs_commit(h);
  nvs_close(h);
}

// true if this address is acceptable as the other half (and remembers it the first time)
static bool peer_check(const char *key, const uint8_t addr[6]) {
  uint8_t known[6];
  if (peer_load(key, known)) return memcmp(known, addr, 6) == 0;
  peer_save(key, addr);
  ESP_LOGI(TAG, "remembered the other half: %02x:%02x:%02x:%02x:%02x:%02x", addr[5], addr[4], addr[3],
           addr[2], addr[1], addr[0]);
  return true;
}

/* ---------------------------------------------------------------- HID data */

// Report ID 1: keyboard (8 byte input, 1 byte LED output). Report ID 2: consumer control. Report ID 3: mouse.
static const uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,  // modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                                              // reserved
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,                          // LEDs (out)
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,                                                              // LED padding
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,  // 6 keys
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,  // 1 x 16-bit usage
    0xC0,
    // Report ID 3: mouse. 5 bytes: buttons (3 bits + padding), x, y, wheel, horizontal wheel (AC Pan).
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,  // 3 buttons
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,                                                              // padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,  // x y wheel
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,                                            // pan
    0xC0, 0xC0,
};

typedef struct { const uint8_t *data; uint16_t len; } ro_val_t;   // read-only attribute value
typedef struct { uint8_t *buf; uint16_t len; } rw_val_t;          // small read/write attribute

static uint8_t g_kbd_in[8];
static uint8_t g_cons_in[2];
static uint8_t g_mouse_in[5];
static uint8_t g_kbd_led[1];
static uint8_t g_protocol_mode[1] = {1};   // 1 = report protocol
static uint8_t g_ctrl_point[1];
static uint8_t g_battery = 100;

static const uint8_t kHidInfo[4] = {0x11, 0x01, 0x00, 0x02};  // bcdHID 1.11, country 0, normally connectable
static const uint8_t kRefKbdIn[2] = {0x01, 0x01};   // report id 1, input
static const uint8_t kRefKbdOut[2] = {0x01, 0x02};  // report id 1, output
static const uint8_t kRefConsIn[2] = {0x02, 0x01};  // report id 2, input
static const uint8_t kRefMouseIn[2] = {0x03, 0x01}; // report id 3, input
static const uint8_t kPnpId[7] = {0x02, 0x3A, 0x30, 0x04, 0x40, 0x00, 0x01};  // USB-IF source, VID 0x303A, PID 0x4004, v1.0
static const char kMfr[] = "Tab5KB";
static const char kModel[] = "Tab5 split keyboard";

static const ro_val_t v_report_map = {kReportMap, sizeof kReportMap};
static const ro_val_t v_hid_info = {kHidInfo, sizeof kHidInfo};
static const ro_val_t v_ref_kbd_in = {kRefKbdIn, 2};
static const ro_val_t v_ref_kbd_out = {kRefKbdOut, 2};
static const ro_val_t v_ref_cons_in = {kRefConsIn, 2};
static const ro_val_t v_ref_mouse_in = {kRefMouseIn, 2};
static const ro_val_t v_mouse_in = {g_mouse_in, sizeof g_mouse_in};
static const ro_val_t v_kbd_in = {g_kbd_in, sizeof g_kbd_in};
static const ro_val_t v_cons_in = {g_cons_in, sizeof g_cons_in};
static const ro_val_t v_battery = {&g_battery, 1};
static const ro_val_t v_pnp = {kPnpId, sizeof kPnpId};
static const ro_val_t v_mfr = {(const uint8_t *)kMfr, sizeof kMfr - 1};
static const ro_val_t v_model = {(const uint8_t *)kModel, sizeof kModel - 1};
static const rw_val_t v_protocol = {g_protocol_mode, 1};
static const rw_val_t v_led = {g_kbd_led, 1};
static const rw_val_t v_ctrl = {g_ctrl_point, 1};

static uint16_t h_kbd_in, h_cons_in, h_mouse_in, h_battery;
static bool g_sub_mouse;

/* ------------------------------------------------------------------- state */

static uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
static bool g_encrypted, g_sub_kbd;
static uint8_t g_own_addr_type;
static ble_kb_state_t g_state = BLE_KB_IDLE;
static ble_kb_state_cb_t g_cb;
static ble_kb_role_t g_role = BLE_KB_ROLE_LEFT;

/* ----------------------------------------------------------------- split link */

// 128-bit UUIDs of the custom left<->right service (bytes are little endian):
//   service 7a1c0001-7b5e-4c3a-9d2f-5a0e11b3f001, key events ...0002, layer ...0003
#define SPLIT_UUID(n) BLE_UUID128_INIT(0x01, 0xf0, 0xb3, 0x11, 0x0e, 0x5a, 0x2f, 0x9d, \
                                       0x3a, 0x4c, 0x5e, 0x7b, n, 0x00, 0x1c, 0x7a)
static const ble_uuid128_t kSplitSvc = SPLIT_UUID(0x01);
static const ble_uuid128_t kSplitKey = SPLIT_UUID(0x02);    // notify: 2 bytes {pos 0-35, down 0/1}
static const ble_uuid128_t kSplitLayer = SPLIT_UUID(0x03);  // write: 1 byte layer id

static ble_split_state_cb_t g_split_state_cb;
static ble_split_key_cb_t g_split_key_cb;
static ble_split_layer_cb_t g_split_layer_cb;
static ble_split_state_t g_split_state = BLE_SPLIT_OFF;
static uint16_t g_split_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t h_split_key;        // right: our notify characteristic value handle
static bool g_split_sub;            // right: left half enabled notifications
static uint16_t h_remote_key;       // left: right half's key characteristic value handle
static uint16_t h_remote_layer;     // left: right half's layer characteristic value handle
static uint16_t g_svc_start, g_svc_end;
static bool g_connecting;           // left: a connect to the right half is in progress
static uint8_t g_layer_cache;
static uint8_t g_status_cache[6];   // layer, mods, first 3 keys, sound settings (left -> right)
static ble_split_sound_cb_t g_split_sound_cb;
void ble_split_set_sound_cb(ble_split_sound_cb_t cb) { g_split_sound_cb = cb; }
static ble_split_report_cb_t g_split_report_cb;
void ble_split_set_report_cb(ble_split_report_cb_t cb) { g_split_report_cb = cb; }
static bool g_layer_known;

void ble_split_set_callbacks(ble_split_state_cb_t state_cb, ble_split_key_cb_t key_cb,
                             ble_split_layer_cb_t layer_cb) {
  g_split_state_cb = state_cb;
  g_split_key_cb = key_cb;
  g_split_layer_cb = layer_cb;
}

static void set_split_state(ble_split_state_t s) {
  if (s == g_split_state) return;
  g_split_state = s;
  ESP_LOGI(TAG, "split: %s", ble_split_state_str(s));
  if (g_split_state_cb) g_split_state_cb(s);
}

const char *ble_split_state_str(ble_split_state_t s) {
  switch (s) {
    case BLE_SPLIT_SEARCHING: return "searching";
    case BLE_SPLIT_LINKED: return "linked";
    default: return "off";
  }
}

static void set_state(ble_kb_state_t s) {
  if (s == g_state) return;
  g_state = s;
  ESP_LOGI(TAG, "state: %s", ble_kb_state_str(s));
  if (g_cb) g_cb(s);
}

static void update_state(void) {
  if (g_conn == BLE_HS_CONN_HANDLE_NONE) return;
  set_state((g_encrypted && g_sub_kbd) ? BLE_KB_READY : BLE_KB_CONNECTED);
}

const char *ble_kb_state_str(ble_kb_state_t s) {
  switch (s) {
    case BLE_KB_ADVERTISING: return "advertising";
    case BLE_KB_CONNECTED: return "connected";
    case BLE_KB_READY: return "ready";
    default: return "off";
  }
}

/* -------------------------------------------------------------------- GATT */

static int ro_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn; (void)attr;
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
    const ro_val_t *v = (const ro_val_t *)arg;
    return os_mbuf_append(ctxt->om, v->data, v->len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

static int rw_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn;
  const rw_val_t *v = (const rw_val_t *)arg;
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    return os_mbuf_append(ctxt->om, v->buf, v->len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (n > v->len) n = v->len;
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, v->buf, n, &got) != 0) return BLE_ATT_ERR_UNLIKELY;
    ESP_LOGI(TAG, "write attr %d: %d byte(s), first=0x%02X", attr, got, got ? v->buf[0] : 0);
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def kSvcsLeft[] = {
    {   // HID
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = ro_access, .arg = (void *)&v_hid_info,
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = ro_access, .arg = (void *)&v_report_map,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN},
            {.uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = rw_access, .arg = (void *)&v_ctrl,
             .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
            {.uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = rw_access, .arg = (void *)&v_protocol,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP},
            {   // keyboard input report (id 1)
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = ro_access, .arg = (void *)&v_kbd_in,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_kbd_in,
                .descriptors = (struct ble_gatt_dsc_def[]){
                    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                     .access_cb = ro_access, .arg = (void *)&v_ref_kbd_in},
                    {0}},
            },
            {   // keyboard LED output report (id 1)
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = rw_access, .arg = (void *)&v_led,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_AUTHEN | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .descriptors = (struct ble_gatt_dsc_def[]){
                    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                     .access_cb = ro_access, .arg = (void *)&v_ref_kbd_out},
                    {0}},
            },
            {   // consumer input report (id 2)
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = ro_access, .arg = (void *)&v_cons_in,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_cons_in,
                .descriptors = (struct ble_gatt_dsc_def[]){
                    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                     .access_cb = ro_access, .arg = (void *)&v_ref_cons_in},
                    {0}},
            },
            {   // mouse input report (id 3)
                .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = ro_access, .arg = (void *)&v_mouse_in,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_mouse_in,
                .descriptors = (struct ble_gatt_dsc_def[]){
                    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                     .access_cb = ro_access, .arg = (void *)&v_ref_mouse_in},
                    {0}},
            },
            {0}},
    },
    {   // Battery
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = ro_access, .arg = (void *)&v_battery,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_battery},
            {0}},
    },
    {   // Device information
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = ro_access, .arg = (void *)&v_mfr,
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2A24), .access_cb = ro_access, .arg = (void *)&v_model,
             .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = ro_access, .arg = (void *)&v_pnp,
             .flags = BLE_GATT_CHR_F_READ},
            {0}},
    },
    {0},
};

// Right half: only the custom split service.
static int split_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    if (!g_split_secure) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t st[6] = {0, 0, 0, 0, 0, 0};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1) return BLE_ATT_ERR_UNLIKELY;
    if (len > sizeof st) len = sizeof st;
    if (os_mbuf_copydata(ctxt->om, 0, len, st) != 0) return BLE_ATT_ERR_UNLIKELY;
    if (g_split_layer_cb) g_split_layer_cb(st[0]);
    if (len >= 5 && g_split_report_cb) g_split_report_cb(st[1], &st[2]);
    if (len >= 6 && (st[5] & 0x80) && g_split_sound_cb) g_split_sound_cb(st[5]);
    return 0;
  }
  return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def kSvcsRight[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSplitSvc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &kSplitKey.u, .access_cb = split_access, .flags = BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &h_split_key},
            {.uuid = &kSplitLayer.u, .access_cb = split_access,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_AUTHEN},
            {0}},
    },
    {0},
};

/* --------------------------------------------------------------------- GAP */

static int gap_event(struct ble_gap_event *event, void *arg);

/* ---- Bluetooth profiles (like ZMK's BT_SEL): profile 0 uses the normal public address (existing pairing
 * keeps working); profiles 1-3 each advertise from their own random address and name, so every PC pairs
 * with a separate "keyboard". */
static volatile uint8_t g_profile;        // wanted profile 0..3
static uint8_t g_adv_profile = 0xFF;      // profile the current advertising address belongs to
static uint8_t g_pc_addr_type;
static char g_base_name[24] = "Tab5 KB";

static void profile_load(void) {
  nvs_handle_t h;
  uint8_t p = 0;
  if (nvs_open("tab5kb", NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_u8(h, "profile", &p) != ESP_OK) p = 0;
    nvs_close(h);
  }
  g_profile = p < 4 ? p : 0;
}

// Small settings (key-sound options) kept in the same flash namespace.
// Flash storage is needed before Bluetooth starts (the half choice is read at boot), so it is set up here too.
void ble_kb_nvs_init(void) {
  static bool done;
  if (done) return;
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  done = true;
}
bool ble_kb_setting_get(const char *key, uint8_t *out) {
  ble_kb_nvs_init();
  nvs_handle_t h;
  if (nvs_open("tab5kb", NVS_READONLY, &h) != ESP_OK) return false;
  const bool ok = nvs_get_u8(h, key, out) == ESP_OK;
  nvs_close(h);
  return ok;
}
void ble_kb_setting_set(const char *key, uint8_t v) {
  ble_kb_nvs_init();
  nvs_handle_t h;
  if (nvs_open("tab5kb", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, key, v);
  nvs_commit(h);
  nvs_close(h);
}

static void profile_save(uint8_t p) {
  nvs_handle_t h;
  if (nvs_open("tab5kb", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "profile", p);
  nvs_commit(h);
  nvs_close(h);
}

static void split_scan(void);

// Switch the advertising identity to the wanted profile. Only while not advertising and not connected.
static void profile_apply(void) {
  const uint8_t p = g_profile;
  if (p == 0) {
    g_pc_addr_type = g_own_addr_type;
    ble_svc_gap_device_name_set(g_base_name);
  } else {
    uint8_t a[6];
    if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, a, NULL) != 0) memset(a, 0x5A, sizeof a);
    a[0] = (uint8_t)(a[0] + p);          // differs per profile
    a[5] |= 0xC0;                        // static random address: two top bits set
    // the controller refuses a new random address while it is scanning (the left half scans for the right half
    // all the time), so pause the scan around the change
    const bool wasScanning = ble_gap_disc_active();
    if (wasScanning) ble_gap_disc_cancel();
    int rc = ble_hs_id_set_rnd(a);
    if (wasScanning) split_scan();
    if (rc != 0) {
      ESP_LOGW(TAG, "profile %u: set random address failed, rc=%d (staying on profile 1)", p + 1, rc);
      g_profile = 0;
      g_pc_addr_type = g_own_addr_type;
      ble_svc_gap_device_name_set(g_base_name);
      g_adv_profile = 0;
      return;
    }
    g_pc_addr_type = BLE_OWN_ADDR_RANDOM;
    char nm[32];
    snprintf(nm, sizeof nm, "%s %u", g_base_name, p + 1);
    ble_svc_gap_device_name_set(nm);
  }
  g_adv_profile = p;
  ESP_LOGI(TAG, "bluetooth profile %u (%s)", p + 1, ble_svc_gap_device_name());
}

uint8_t ble_kb_profile(void) { return g_profile; }

static void advertise_pc(void);

void ble_kb_select_profile(uint8_t p) {
  if (g_role != BLE_KB_ROLE_LEFT || p > 3 || p == g_profile) return;
  g_profile = p;
  profile_save(p);
  ESP_LOGI(TAG, "switching to bluetooth profile %u", p + 1);
  if (g_conn != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(g_conn, BLE_ERR_REM_USER_CONN_TERM);   // the disconnect event re-advertises as the new profile
  } else {
    ble_gap_adv_stop();
    advertise_pc();
  }
}

static void advertise_pc(void) {
  if (g_adv_profile != g_profile) {
    ble_gap_adv_stop();
    profile_apply();
  }
  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof fields);
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.appearance = 0x03C1;  // keyboard
  fields.appearance_is_present = 1;
  fields.uuids16 = (ble_uuid16_t[]){BLE_UUID16_INIT(0x1812)};
  fields.num_uuids16 = 1;
  fields.uuids16_is_complete = 1;
  const char *name = ble_svc_gap_device_name();
  fields.name = (uint8_t *)name;
  fields.name_len = strlen(name);
  fields.name_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

  // name again in the scan response, in case the scanner only shows that
  struct ble_hs_adv_fields rsp;
  memset(&rsp, 0, sizeof rsp);
  rsp.name = (uint8_t *)name;
  rsp.name_len = strlen(name);
  rsp.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&rsp);
  if (rc != 0) ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);

  struct ble_gap_adv_params p;
  memset(&p, 0, sizeof p);
  p.conn_mode = BLE_GAP_CONN_MODE_UND;
  p.disc_mode = BLE_GAP_DISC_MODE_GEN;
  rc = ble_gap_adv_start(g_pc_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
  if (rc != 0) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
  set_state(BLE_KB_ADVERTISING);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  struct ble_gap_conn_desc desc;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      ESP_LOGI(TAG, "connect status=%d", event->connect.status);
      if (event->connect.status == 0) {
        g_conn = event->connect.conn_handle;
        g_encrypted = false;
        g_sub_kbd = false;
        g_sub_mouse = false;
        update_state();
        // ask for a fast link: 7.5-15 ms interval, no latency, 4 s timeout (the PC may refuse)
        struct ble_gap_upd_params up = {.itvl_min = 6, .itvl_max = 12, .latency = 0,
                                        .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0};
        int rc = ble_gap_update_params(g_conn, &up);
        if (rc != 0) ESP_LOGW(TAG, "update_params rc=%d", rc);
      } else {
        advertise_pc();
      }
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
      pair_dismiss(event->disconnect.conn.conn_handle);
      g_conn = BLE_HS_CONN_HANDLE_NONE;
      g_encrypted = false;
      g_sub_kbd = false;
      g_sub_mouse = false;
      advertise_pc();
      return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise_pc();
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, "encryption change status=%d", event->enc_change.status);
      pair_dismiss(event->enc_change.conn_handle);
      g_encrypted = false;
      if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
        ESP_LOGI(TAG, "  encrypted=%d authenticated=%d bonded=%d", desc.sec_state.encrypted,
                 desc.sec_state.authenticated, desc.sec_state.bonded);
        if (desc.sec_state.encrypted && desc.sec_state.authenticated) {
          g_encrypted = true;
        } else {
          // e.g. an old "Just Works" bond: refuse. Remove the keyboard in Windows and pair again.
          ESP_LOGW(TAG, "link is not authenticated: dropping");
          ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
      }
      update_state();
      return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      on_passkey(event, "PC");
      return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
      ESP_LOGI(TAG, "subscribe attr=%d notify=%d", event->subscribe.attr_handle, event->subscribe.cur_notify);
      if (event->subscribe.attr_handle == h_kbd_in) {
        g_sub_kbd = event->subscribe.cur_notify;
        update_state();
      }
      if (event->subscribe.attr_handle == h_mouse_in) g_sub_mouse = event->subscribe.cur_notify;
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
      // The PC lost its bond (or we did): drop ours and pair again.
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
      return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
      if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0)
        ESP_LOGI(TAG, "conn update: interval=%d (x1.25 ms) latency=%d timeout=%d", desc.conn_itvl,
                 desc.conn_latency, desc.supervision_timeout);
      return 0;

    default:
      return 0;
  }
}

/* ---------------------------------------------------- split link: right (peripheral) */

static int right_gap_event(struct ble_gap_event *event, void *arg);

static void advertise_right(void) {
  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof fields);
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = (ble_uuid128_t[]){kSplitSvc};
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

  const char *name = ble_svc_gap_device_name();
  struct ble_hs_adv_fields rsp;
  memset(&rsp, 0, sizeof rsp);
  rsp.name = (uint8_t *)name;
  rsp.name_len = strlen(name);
  rsp.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&rsp);
  if (rc != 0) ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);

  struct ble_gap_adv_params p;
  memset(&p, 0, sizeof p);
  p.conn_mode = BLE_GAP_CONN_MODE_UND;
  p.disc_mode = BLE_GAP_DISC_MODE_GEN;
  p.itvl_min = 32;   // 20 ms: the left half finds us quickly
  p.itvl_max = 48;   // 30 ms
  rc = ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &p, right_gap_event, NULL);
  if (rc != 0) { ESP_LOGE(TAG, "adv_start rc=%d", rc); return; }
  set_split_state(BLE_SPLIT_SEARCHING);
}

static int right_gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  struct ble_gap_conn_desc desc;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      ESP_LOGI(TAG, "left half connect status=%d", event->connect.status);
      if (event->connect.status == 0) {
        g_split_conn = event->connect.conn_handle;
        g_split_sub = false;
        g_split_secure = false;
      } else {
        advertise_right();
      }
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, "left half encryption change status=%d", event->enc_change.status);
      pair_dismiss(event->enc_change.conn_handle);
      if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 &&
          desc.sec_state.encrypted && desc.sec_state.authenticated && peer_check("left", desc.peer_id_addr.val)) {
        g_split_secure = true;
      } else {
        ESP_LOGW(TAG, "left half is not authenticated or not the known one: dropping");
        ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      }
      return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      on_passkey(event, "left half");
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "left half disconnect reason=%d", event->disconnect.reason);
      pair_dismiss(event->disconnect.conn.conn_handle);
      g_split_conn = BLE_HS_CONN_HANDLE_NONE;
      g_split_sub = false;
      g_split_secure = false;
      set_split_state(BLE_SPLIT_SEARCHING);
      advertise_right();
      return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise_right();
      return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
      ESP_LOGI(TAG, "subscribe attr=%d notify=%d", event->subscribe.attr_handle, event->subscribe.cur_notify);
      if (event->subscribe.attr_handle == h_split_key) {
        if (event->subscribe.cur_notify && !g_split_secure) {   // never talk to an unauthenticated peer
          ble_gap_terminate(event->subscribe.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
          return 0;
        }
        g_split_sub = event->subscribe.cur_notify;
        set_split_state(g_split_sub ? BLE_SPLIT_LINKED : BLE_SPLIT_SEARCHING);
      }
      return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
      if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0)
        ESP_LOGI(TAG, "conn update: interval=%d (x1.25 ms) latency=%d timeout=%d", desc.conn_itvl,
                 desc.conn_latency, desc.supervision_timeout);
      return 0;

    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(TAG, "mtu=%d", event->mtu.value);
      return 0;

    default:
      return 0;
  }
}

void ble_split_send_key(uint8_t pos, bool down) {
  if (g_role != BLE_KB_ROLE_RIGHT || g_split_conn == BLE_HS_CONN_HANDLE_NONE || !g_split_sub || !g_split_secure) return;
  const uint8_t d[2] = {pos, down ? 1 : 0};
  struct os_mbuf *om = ble_hs_mbuf_from_flat(d, sizeof d);
  if (!om) { ESP_LOGW(TAG, "out of mbufs"); return; }
  int rc = ble_gatts_notify_custom(g_split_conn, h_split_key, om);
  if (rc != 0) ESP_LOGW(TAG, "split notify rc=%d", rc);
}

/* ----------------------------------------------------- split link: left (central) */

static int split_gap_event(struct ble_gap_event *event, void *arg);

static void split_scan(void) {
  struct ble_gap_disc_params dp;
  memset(&dp, 0, sizeof dp);
  dp.itvl = 160;    // 100 ms
  dp.window = 48;   // 30 ms
  dp.passive = 1;
  int rc = ble_gap_disc(g_own_addr_type, BLE_HS_FOREVER, &dp, split_gap_event, NULL);
  if (rc != 0 && rc != BLE_HS_EALREADY) { ESP_LOGE(TAG, "scan start rc=%d", rc); return; }
  set_split_state(BLE_SPLIT_SEARCHING);
}

static void split_drop(uint16_t conn) { ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM); }

static int on_cccd(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
  (void)attr; (void)arg;
  if (err->status != 0) {
    ESP_LOGE(TAG, "enabling right half notifications failed: %d", err->status);
    split_drop(conn);
    return 0;
  }
  set_split_state(BLE_SPLIT_LINKED);
  if (g_layer_known && h_remote_layer) {
    ble_gattc_write_no_rsp_flat(conn, h_remote_layer, g_status_cache, sizeof g_status_cache);
  }
  return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg) {
  (void)arg;
  if (err->status == 0) {
    if (ble_uuid_cmp(&chr->uuid.u, &kSplitKey.u) == 0) h_remote_key = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &kSplitLayer.u) == 0) h_remote_layer = chr->val_handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE && h_remote_key != 0) {
    // The client configuration descriptor sits right after the characteristic value.
    const uint8_t on[2] = {1, 0};
    int rc = ble_gattc_write_flat(conn, h_remote_key + 1, on, sizeof on, on_cccd, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "cccd write rc=%d", rc); split_drop(conn); }
    return 0;
  }
  ESP_LOGE(TAG, "right half characteristics not found (status %d)", err->status);
  split_drop(conn);
  return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg) {
  (void)arg;
  if (err->status == 0) {
    g_svc_start = svc->start_handle;
    g_svc_end = svc->end_handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE && g_svc_end != 0) {
    int rc = ble_gattc_disc_all_chrs(conn, g_svc_start, g_svc_end, on_chr, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "disc chrs rc=%d", rc); split_drop(conn); }
    return 0;
  }
  ESP_LOGE(TAG, "right half service not found (status %d)", err->status);
  split_drop(conn);
  return 0;
}

static bool is_right_half(const struct ble_gap_event *event) {
  struct ble_hs_adv_fields f;
  if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0) return false;
  for (int i = 0; i < f.num_uuids128; i++)
    if (ble_uuid_cmp(&f.uuids128[i].u, &kSplitSvc.u) == 0) return true;
  return false;
}

static int split_gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  struct ble_gap_conn_desc desc;
  switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
      if (g_connecting || g_split_conn != BLE_HS_CONN_HANDLE_NONE) return 0;
      if (event->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND) return 0;
      if (!is_right_half(event)) return 0;
      ble_addr_t addr = event->disc.addr;
      {
        uint8_t known[6];
        if (peer_load("right", known) && memcmp(known, addr.val, 6) != 0) return 0;   // only our own right half
      }
      ESP_LOGI(TAG, "found right half %02x:%02x:%02x:%02x:%02x:%02x, connecting", addr.val[5], addr.val[4],
               addr.val[3], addr.val[2], addr.val[1], addr.val[0]);
      ble_gap_disc_cancel();
      struct ble_gap_conn_params cp;
      memset(&cp, 0, sizeof cp);
      cp.scan_itvl = 160;
      cp.scan_window = 48;
      cp.itvl_min = 9;    // 11.25 ms
      cp.itvl_max = 12;   // 15 ms
      cp.latency = 0;
      cp.supervision_timeout = 400;   // 4 s
      g_connecting = true;
      int rc = ble_gap_connect(g_own_addr_type, &addr, 10000, &cp, split_gap_event, NULL);
      if (rc != 0) {
        ESP_LOGE(TAG, "connect rc=%d", rc);
        g_connecting = false;
        split_scan();
      }
      return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
      if (!g_connecting && g_split_conn == BLE_HS_CONN_HANDLE_NONE) split_scan();
      return 0;

    case BLE_GAP_EVENT_CONNECT:
      g_connecting = false;
      ESP_LOGI(TAG, "right half connect status=%d", event->connect.status);
      if (event->connect.status == 0) {
        g_split_conn = event->connect.conn_handle;
        g_split_secure = false;
        h_remote_key = h_remote_layer = 0;
        g_svc_start = g_svc_end = 0;
        // authenticate and encrypt first; the service is only used after that (see ENC_CHANGE)
        int rc = ble_gap_security_initiate(g_split_conn);
        if (rc != 0) { ESP_LOGE(TAG, "security initiate rc=%d", rc); split_drop(g_split_conn); }
      } else {
        split_scan();
      }
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, "right half encryption change status=%d", event->enc_change.status);
      pair_dismiss(event->enc_change.conn_handle);
      if (event->enc_change.conn_handle != g_split_conn) return 0;
      if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 &&
          desc.sec_state.encrypted && desc.sec_state.authenticated && peer_check("right", desc.peer_id_addr.val)) {
        if (!g_split_secure) {
          g_split_secure = true;
          int rc = ble_gattc_disc_svc_by_uuid(g_split_conn, &kSplitSvc.u, on_svc, NULL);
          if (rc != 0) { ESP_LOGE(TAG, "disc svc rc=%d", rc); split_drop(g_split_conn); }
        }
      } else {
        ESP_LOGW(TAG, "right half is not authenticated or not the known one: dropping");
        split_drop(g_split_conn);
      }
      return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      on_passkey(event, "right half");
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "right half disconnect reason=%d", event->disconnect.reason);
      pair_dismiss(event->disconnect.conn.conn_handle);
      g_split_conn = BLE_HS_CONN_HANDLE_NONE;
      g_split_secure = false;
      h_remote_key = h_remote_layer = 0;
      set_split_state(BLE_SPLIT_SEARCHING);
      split_scan();
      return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (!g_split_secure || event->notify_rx.conn_handle != g_split_conn ||
          event->notify_rx.attr_handle != h_remote_key) return 0;
      uint8_t d[2];
      if (OS_MBUF_PKTLEN(event->notify_rx.om) < 2 || os_mbuf_copydata(event->notify_rx.om, 0, 2, d) != 0) return 0;
      if (g_split_key_cb) g_split_key_cb(d[0], d[1] != 0);
      return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
      if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0)
        ESP_LOGI(TAG, "right half conn update: interval=%d (x1.25 ms) latency=%d", desc.conn_itvl, desc.conn_latency);
      return 0;

    default:
      return 0;
  }
}

static void split_push_status(void) {
  g_layer_known = true;
  if (g_role != BLE_KB_ROLE_LEFT || g_split_state != BLE_SPLIT_LINKED || g_split_conn == BLE_HS_CONN_HANDLE_NONE ||
      h_remote_layer == 0)
    return;
  int rc = ble_gattc_write_no_rsp_flat(g_split_conn, h_remote_layer, g_status_cache, sizeof g_status_cache);
  if (rc != 0) ESP_LOGW(TAG, "status write rc=%d", rc);
}

void ble_split_send_layer(uint8_t layer) {
  g_layer_cache = layer;
  g_status_cache[0] = layer;
  split_push_status();
}

void ble_split_send_report(uint8_t mods, const uint8_t keys[3]) {
  g_status_cache[1] = mods;
  g_status_cache[2] = keys[0];
  g_status_cache[3] = keys[1];
  g_status_cache[4] = keys[2];
  split_push_status();
}

void ble_split_send_sound(uint8_t packed) {
  g_status_cache[5] = packed | 0x80;
  split_push_status();
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset, reason=%d", reason); }

static void on_sync(void) {
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return; }
  rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
  if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
  uint8_t a[6] = {0};
  ble_hs_id_copy_addr(g_own_addr_type, a, NULL);
  ESP_LOGI(TAG, "address %02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
  g_pc_addr_type = g_own_addr_type;
  if (g_role == BLE_KB_ROLE_LEFT) {
    profile_load();
    advertise_pc();   // HID keyboard for the PC
    split_scan();     // and look for the right half
  } else {
    advertise_right();
  }
}

static void host_task(void *param) {
  (void)param;
  ESP_LOGI(TAG, "host task started");
  nimble_port_run();
  nimble_port_freertos_deinit();
}

int ble_kb_start(const char *device_name, ble_kb_role_t role, ble_kb_state_cb_t cb) {
  g_role = role;
  g_cb = cb;

  ble_kb_nvs_init();

  // Same sequence as the bleprph example. On this Tab5's old C6 firmware the two
  // controller calls time out (about 5 s each) but Bluetooth works anyway.
  esp_hosted_connect_to_slave();
  esp_hosted_coprocessor_fwver_t fwver;
  if (esp_hosted_get_coprocessor_fwversion(&fwver) == ESP_OK)
    ESP_LOGI(TAG, "C6 firmware %" PRIu32 ".%" PRIu32 ".%" PRIu32, fwver.major1, fwver.minor1, fwver.patch1);
  else {
    // No answer from the C6: starting NimBLE now would abort and reboot-loop, so stay touch-only.
    ESP_LOGE(TAG, "no answer from the C6 over SDIO; Bluetooth disabled");
    return -2;
  }
  // The M5Stack C6 firmware (1.x) never answers the controller init/enable requests, so each one just
  // waits out a 5 s RPC timeout. Bluetooth works without them on that firmware, so skip them there.
  // Newer ESP-Hosted slave firmware (2.x+) does need the calls. Set to 0 if Bluetooth ever fails to come up.
#ifndef BLE_KB_SKIP_RPC_ON_OLD_C6
#define BLE_KB_SKIP_RPC_ON_OLD_C6 1
#endif
  if (BLE_KB_SKIP_RPC_ON_OLD_C6 && fwver.major1 < 2) {
    ESP_LOGI(TAG, "old C6 firmware: skipping bt controller init/enable (saves ~10 s)");
  } else {
    if (esp_hosted_bt_controller_init() != ESP_OK) ESP_LOGW(TAG, "bt controller init failed");
    if (esp_hosted_bt_controller_enable() != ESP_OK) ESP_LOGW(TAG, "bt controller enable failed");
  }

  ret = nimble_port_init();
  if (ret != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed: %d", ret); return -1; }

  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;    // numeric comparison: code on screen + YES/NO tap
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  const struct ble_gatt_svc_def *svcs = (role == BLE_KB_ROLE_LEFT) ? kSvcsLeft : kSvcsRight;
  int rc = ble_gatts_count_cfg(svcs);
  if (rc == 0) rc = ble_gatts_add_svcs(svcs);
  if (rc != 0) { ESP_LOGE(TAG, "gatt setup failed rc=%d", rc); return rc; }

  snprintf(g_base_name, sizeof g_base_name, "%s", device_name);
  ble_svc_gap_device_name_set(device_name);
  if (role == BLE_KB_ROLE_LEFT) ble_svc_gap_device_appearance_set(0x03C1);
  ble_store_config_init();
  nimble_port_freertos_init(host_task);
  return 0;
}

/* ------------------------------------------------------------------ output */

static void notify(uint16_t handle, const uint8_t *d, uint16_t n) {
  if (g_conn == BLE_HS_CONN_HANDLE_NONE) return;
  struct os_mbuf *om = ble_hs_mbuf_from_flat(d, n);
  if (!om) { ESP_LOGW(TAG, "out of mbufs"); return; }
  int rc = ble_gatts_notify_custom(g_conn, handle, om);
  if (rc != 0) ESP_LOGW(TAG, "notify rc=%d", rc);
}

void ble_kb_send_keyboard(const uint8_t report[8]) {
  memcpy(g_kbd_in, report, 8);
  if (g_state == BLE_KB_READY) notify(h_kbd_in, g_kbd_in, 8);
}

void ble_kb_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan) {
  g_mouse_in[0] = buttons & 0x07;
  g_mouse_in[1] = (uint8_t)dx;
  g_mouse_in[2] = (uint8_t)dy;
  g_mouse_in[3] = (uint8_t)wheel;
  g_mouse_in[4] = (uint8_t)pan;
  if (g_state == BLE_KB_READY && g_sub_mouse) notify(h_mouse_in, g_mouse_in, sizeof g_mouse_in);
}

void ble_kb_send_consumer(uint16_t usage) {
  g_cons_in[0] = usage & 0xFF;
  g_cons_in[1] = usage >> 8;
  if (g_state == BLE_KB_READY) notify(h_cons_in, g_cons_in, 2);
}
