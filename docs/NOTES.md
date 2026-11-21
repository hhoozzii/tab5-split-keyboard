# Tab5 split keyboard: how it works, how to build it (as of 2026-11-20)

36-key Miryoku split keyboard on two M5Stack Tab5 units. Left half = Bluetooth HID keyboard to the PC and BLE central to the right half; right half = BLE peripheral that forwards touches. Behaviour follows https://github.com/hhoozzii/miryoku_zmk. Status: complete and tested on hardware (typing, layers, split link, secure pairing, tilted thumbs, IMU flip, mouse keys, caps word, BT profiles, key sounds, C6 2.12.13 on both halves).

## Hardware facts
- Tab5 = ESP32-P4 (no radio) + ESP32-C6 co-processor over SDIO (CLK 12, CMD 13, D0-D3 11/10/9/8, RST 15). All Bluetooth goes through the C6 via ESP-Hosted; the P4 runs the NimBLE host over VHCI.
- **P4 silicon is revision v1.3.** ESP-IDF 5.5.x builds only for v3.0+ chips; **use ESP-IDF 5.4.x (v5.4.4 confirmed)**. Never flash with --force.
- Panel: BSP reports "board version 2 (LCD ST7123, Touch ST7123, FW 3)", portrait 720x1280, up to 10 touches. Verified: 5 fingers tracked.
- Both Tab5s identical. Left on COM12, right on COM13 (ports can change after replugging). C6 BLE addresses: left b4:3a:45:9f:ed:c6, right b4:3a:45:91:1f:aa.
- **The C6 must be powered by the BSP before ESP-Hosted starts:** `bsp_feature_enable(BSP_FEATURE_WIFI, true)`, then ~500 ms, then Bluetooth. Otherwise SDIO times out (`send_op_cond 0x107`).
- **esp_hosted mempool crash:** pin esp_hosted 2.13.0 in components/ble_kb/idf_component.yml and set `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` in sdkconfig.defaults (delete sdkconfig afterwards; defaults apply only when sdkconfig doesn't exist). `CONFIG_BT_NIMBLE_MAX_BONDS=8` also lives there; patch an existing sdkconfig: `(Get-Content sdkconfig) -replace 'CONFIG_BT_NIMBLE_MAX_BONDS=3','CONFIG_BT_NIMBLE_MAX_BONDS=8' | Set-Content sdkconfig`.
- IMU: BMI270 at I2C 0x68 on the BSP's bus (SDA 31, SCL 32). Upright landscape reads x about -990 mg.
- Speakers: ES8388 codec, stereo; small speakers cannot play low notes (lowest sound note is about 350 Hz).

## C6 firmware (ESP-Hosted 2.12.13, both halves)
- The C6 originally shipped with Wi-Fi SDIO firmware 1.4.1: BLE worked but boot cost about 15 s of RPC timeouts (the firmware skips the BT RPC when the C6 is < 2.0, `BLE_KB_SKIP_RPC_ON_OLD_C6`).
- Updated with `c6tool/` (separate ESP-IDF project): embeds Espressif's prebuilt `network_adapter_esp32c6.bin` v2.12.13 (SDIO build, chip id 13, from esphome.github.io/esp-hosted-firmware/v2.12.13/; `c6tool\get_c6_fw.ps1` downloads it). Steps per half: `cd c6tool; idf.py -p COMx flash monitor`, wait for `DONE`, unplug and replug USB, run the monitor again until it says "the C6 already runs this version", then flash the keyboard firmware back.
- After flashing c6tool the P4 can stay in download mode: unplug/replug USB, no buttons held.
- Host 2.13.0 with C6 2.12.x logs a harmless "Version mismatch" warning and "did not report transport size" lines. If RPC timeouts or link drops ever appear, pin the host to ~2.12.
- Right half boot: split linked about 7.7 s after power-on.

## Build and flash (Windows)
- Use the ESP-IDF 5.4 PowerShell; project at C:\vccode\tab5kb, firmware in `firmware`.
- First run after cloning: `powershell -ExecutionPolicy Bypass -File tools\get_bmi270.ps1` (downloads the Bosch driver into firmware\components\imu\bosch) and, for the C6 tool, `c6tool\get_c6_fw.ps1`.
- Half is a menuconfig setting (`Tab5 keyboard` -> Left/Right): `idf.py menuconfig`, then `idf.py -p COM12 flash` (left) and again for `COM13` (right). `flash` builds first.
- If you copy changed source files in, set `LastWriteTime = Get-Date` on them so ninja rebuilds (copied files keep their old date).
- "opening dependency file ... .obj.d" or "loading build.ninja" errors = corrupted build folder: `idf.py fullclean`, rebuild. Don't paste log text or `<placeholder>` into PowerShell.
- Confirm a new flash by the ELF SHA / compile time in the boot log. If pairing misbehaves: remove the device in Windows and `idf.py -p COM12 erase-flash flash monitor`.
- Desktop tests (no ESP-IDF needed): `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`; keysound: `g++ -std=c++17 tests/test_keysound.cpp -o tks && tks`.

## Architecture
- **HID over BLE (left):** own NimBLE HID-over-GATT (components/ble_kb): keyboard report ID 1, consumer ID 2, mouse ID 3 (changing the descriptor needs removing and re-pairing in Windows), Battery, Device Info. Advertises as "Tab5 KB L".
- **Split link:** left is central for the right half and HID peripheral for the PC at once. Right = peripheral "Tab5 KB R", custom GATT service 7a1c0001-7b5e-4c3a-9d2f-5a0e11b3f001: key characteristic ...0002 (notify, {pos, down}), status characteristic ...0003 (write, 6 bytes: layer, mods, 3 keys, sound settings). The right half runs no engine; it forwards touches and shows what the left reports. Reason: a home-row mod on one half must combine with a key on the other half. If the link drops, the left releases every key the right was holding.
- **Secure pairing:** DisplayYesNo with MITM, Secure Connections, bonding (NVS). Numeric comparison: a 6-digit code with YES/NO on screen and on the PC. The split link requires encrypted and authenticated, and each half pins the other's address.
- **BT profiles:** profile 0 = public address ("Tab5 KB L"); 1-3 = static random addresses ("Tab5 KB L 2/3/4"). Two profiles on the same PC can clash during pairing (accepted).
- **Keymap engine** (engine/, tools/gen_keymap.py generates the layer table and labels from the miryoku_zmk fork): hold-taps are tap-preferred with 200 ms tapping term; layer jumps need a double tap; a key keeps the binding it had when pressed. Specials (engine/specials.h): mouse keys (300 to 1200 px/s ramp over 500 ms, 8 wheel notches/s), buttons, caps word (ends on other keys, ctrl/alt/gui or 5 s idle), BT profiles, Boot. Regenerate the table: `python tools/gen_keymap.py <miryoku_zmk dir> engine/miryoku_keymap.h QWERTY`.
- **Touch:** core/touch_tracker (up to 5 fingers), core/touch_keys (a finger latches to the key it landed on), core/key_layout (hit-testing for rotated shapes).
- **IMU auto-flip:** components/imu (Bosch driver downloaded by tools/get_bmi270.ps1), core/autorotate.h turns the picture 180 degrees when gravity along x passes 600 mg for 600 ms; the touch mapping follows. The BSP's espressif/bmi270 clashes on `bmi270_config_file`, so ours is renamed via a compile definition.
- **Display rotation:** landscape 1280x720 with `bsp_display_rotate(ROTATION_270)`; touch read raw and mapped by core/orientation.h `panelToLogical` (measured: Deg270 lx=py, ly=719-px; Deg90 lx=1279-py, ly=px).

## UI
- Miryoku-style staggered keys, 19 mm wide, rounded corners (cap 206x140 px, pitch 220 wide); thumb keys tilted 13 and 25 degrees, mirrored on the right. Status text sits in the outer bottom corner: left shows layer, PC, right half and BT profile plus sound; right shows layer, left link, fingers, mods, keys and sound.
- Each key shows the layer's label (coloured by layer) plus small legends for the other layers (left half Num blue / Sym green / Fun red, right half Nav yellow / Mouse orange / Media purple). Thumb text takes the colour of the layer it holds: Esc purple (Media), Space yellow (Nav), Tab orange (Mouse), Ent green (Sym), Bksp blue (Num), Del red (Fun). Colours: Nav 0xE8C547, Mouse 0xF08A3C, Media 0xB57BEE, Num 0x5B9BFF, Sym 0x4CC76B, Fun 0xF0605A; unused key 0x161616, pressed 0x2E7DFF, normal 0x2B2B2B.
- **Tilted keys:** LVGL transforms crash (`lv_draw_sw_transform`), so tilted thumbs use pre-rendered ARGB canvases (face recoloured for pressed/normal) with text drawn upright to a scratch picture and rotated by own float code (`blit_label`); tilted text is cached per layer.
- **Responsiveness:** redundant LVGL style/text sets used to redraw large canvases on every touch frame (cached now). Redraws run in their own task (`ui_task`, pinned to core 0, prio 4), LVGL refresh period 10 ms, touch task prio 5.

## Key sounds
- engine/keysound.h: hardware-independent synth (22050 Hz, 8 voices, 5 partials, soft limiter) with desktop test tests/test_keysound.cpp. Eight sounds, cycled by the Tone key: Click, Piano, Organ, Marimba, Bell, Tick, Clack (typewriter), Pluck. One-shots (Click, Tick, Clack) play on press only; Piano and Organ hold while pressed; Marimba, Bell and Pluck ring and die away. Pitch follows key position on a C major scale from C5.
- Each half plays its own touches on its own speaker. Settings (on/off, sound, volume 0-10, default 7) live on the left, are saved to flash (`ble_kb_setting_get/set`, namespace "tab5kb", keys snd_on / snd_vol / snd_kind, loaded about 4 s after boot) and are sent to the right in the sixth byte of the status write (bit7 valid, bits4-6 sound, bits0-3 volume, 15 = muted; resent whenever the link comes up). The right half has a speaker trim of 0.6 (`kSpeakerTrim`).
- Controls: the Media layer's top-row RGB keys on the right half are relabelled Snd (on/off), Tone (next), Vol-, Vol+, Test.
- Audio output: `bsp_audio_codec_speaker_init`, 16-bit stereo, codec volume fixed at 80, `BSP_FEATURE_SPEAKER` enabled. The audio task runs at priority 10 pinned to core 1, in 256-frame writes, only while a voice is active. (A lower priority made held tones skip during layer-change redraws.)
- Saving through nvs.h from main did nothing (header not visible); settings go through ble_kb instead.

## Possible follow-ups
- Per-half volume offset beyond the fixed trim, key-release click, more sounds (the split link allows 8; a 9th needs a wider field).
- Skipped by choice: exact hold timing (holds land 2-18 ms late), palm rejection, a fix for two BT profiles on one PC, an IMU tilt-target on screen.

## Sources
- https://docs.m5stack.com/en/core/Tab5
- https://docs.m5stack.com/en/guide/restore_factory/m5tab5_c6_wifi
- https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/chip_revision.html
- https://components.espressif.com/components/espressif/m5stack_tab5/versions/1.3.3/api
- https://components.espressif.com/components/espressif/esp_hosted/versions/2.11.5/examples/host_nimble_bleprph_host_only_vhci
- https://community.bosch-sensortec.com/mems-sensors-forum-jrmujtaw/post/bmi270-initialization-without-config-file-Aq7R8THPDvEHlmT
- https://github.com/hhoozzii/miryoku_zmk
