// BLE for the Tab5 split keyboard, via ESP-Hosted + NimBLE host over VHCI.
//  LEFT  role: HID keyboard to the PC (BLE peripheral) + BLE central link to the right half.
//  RIGHT role: BLE peripheral with a small custom service that sends key events to the left half.
// Startup order copies Espressif's host_nimble_bleprph_host_only_vhci example.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  BLE_KB_ROLE_LEFT = 0,
  BLE_KB_ROLE_RIGHT = 1,
} ble_kb_role_t;

// State of the HID link to the PC (left half only).
typedef enum {
  BLE_KB_IDLE = 0,
  BLE_KB_ADVERTISING,  // waiting for the PC
  BLE_KB_CONNECTED,    // link up, not yet encrypted/subscribed
  BLE_KB_READY,        // encrypted and the PC has enabled input reports
} ble_kb_state_t;

// State of the left <-> right link (both halves).
typedef enum {
  BLE_SPLIT_OFF = 0,
  BLE_SPLIT_SEARCHING,  // left: scanning for the right half; right: advertising
  BLE_SPLIT_LINKED,     // connected and notifications enabled
} ble_split_state_t;

typedef void (*ble_kb_state_cb_t)(ble_kb_state_t state);          // BLE host task
typedef void (*ble_split_state_cb_t)(ble_split_state_t state);    // BLE host task
typedef void (*ble_split_key_cb_t)(uint8_t pos, bool down);       // left: key event from the right half (host task)
typedef void (*ble_split_report_cb_t)(uint8_t mods, const uint8_t keys[3]);   // right: left half's HID report (host task)
typedef void (*ble_split_sound_cb_t)(uint8_t packed);            // right: key-sound settings from the left half (host task)
typedef void (*ble_split_layer_cb_t)(uint8_t layer);              // right: layer id from the left half (host task)

// Optional callbacks; call before ble_kb_start. Any may be NULL.
void ble_split_set_callbacks(ble_split_state_cb_t state_cb, ble_split_key_cb_t key_cb,
                             ble_split_layer_cb_t layer_cb);

void ble_split_set_report_cb(ble_split_report_cb_t cb);   // right half only; call before ble_kb_start
void ble_split_set_sound_cb(ble_split_sound_cb_t cb);     // right half only; call before ble_kb_start

// Blocks for ~2-15 s while the C6 comes up. Returns 0 on success.
// pc_cb is only used by the left role.
int ble_kb_start(const char *device_name, ble_kb_role_t role, ble_kb_state_cb_t pc_cb);

// ---- left role: HID to the PC. Safe to call from any task.
// 8 bytes: modifiers, reserved(0), 6 key usages.
void ble_kb_send_keyboard(const uint8_t report[8]);
// Mouse report: buttons (bit0 left, bit1 right, bit2 middle), movement, wheel and horizontal wheel.
void ble_kb_send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan);
// Bluetooth profile 0..3 (BT_SEL_n). Profile 0 keeps the original identity; the others have their own.
// Switching disconnects from the PC and advertises again as the new profile.
void ble_kb_select_profile(uint8_t profile);
uint8_t ble_kb_profile(void);
// Persistent one-byte settings (flash). get returns false when the key was never saved.
bool ble_kb_setting_get(const char *key, uint8_t *out);
void ble_kb_setting_set(const char *key, uint8_t v);
// Consumer usage (e.g. 0xE9 volume up); 0 = release.
void ble_kb_send_consumer(uint16_t usage);
// Tell the right half which layer is active (remembered and re-sent when the link comes up).
void ble_split_send_layer(uint8_t layer);
// Tell the right half the key-sound settings: bit6 on, bits4-5 sound, bits0-3 volume (bit7 is set for you).
void ble_split_send_sound(uint8_t packed);
// Tell the right half the current HID modifiers and first 3 keys (shown in its status text).
void ble_split_send_report(uint8_t mods, const uint8_t keys[3]);

// ---- right role: send a key event to the left half. Safe to call from any task.
void ble_split_send_key(uint8_t pos, bool down);

// ---- pairing (both roles). Numeric comparison: both devices show the same 6-digit code and the
// user confirms on each. The callback runs on the BLE host task. active=false dismisses the prompt.
typedef void (*ble_pair_cb_t)(const char *who, uint32_t code, bool active);
void ble_pair_set_callback(ble_pair_cb_t cb);   // before ble_kb_start
void ble_pair_reply(bool accept);               // answer the prompt (any task)

const char *ble_kb_state_str(ble_kb_state_t s);
const char *ble_split_state_str(ble_split_state_t s);

#ifdef __cplusplus
}
#endif
