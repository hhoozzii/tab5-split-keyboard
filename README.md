# Tab5 split keyboard

A 36-key [Miryoku](https://github.com/manna-harbour/miryoku_zmk) split keyboard built from two M5Stack Tab5 units
(ESP32-P4 + ESP32-C6). Multi-touch, home-row mods and thumb layers like Miryoku ZMK, Bluetooth HID to the PC,
a Bluetooth split link between the halves, secure pairing, auto-flip with the IMU, mouse keys, caps word,
four Bluetooth profiles and eight key-press sounds. Layer table generated from the
[hhoozzii/miryoku_zmk](https://github.com/hhoozzii/miryoku_zmk) fork.

Status: complete and tested on hardware (2026-11-20).

## Quick start (Windows, ESP-IDF 5.4.x PowerShell)

    git clone https://github.com/hhoozzii/tab5-split-keyboard C:\vccode\tab5kb
    cd C:\vccode\tab5kb\firmware
    idf.py set-target esp32p4
    idf.py -p COM12 flash monitor      # first Tab5
    idf.py -p COM13 flash monitor      # second Tab5 (same firmware)

The same firmware runs on both. On first boot each Tab5 asks which half it is (tap LEFT or RIGHT); the answer is
remembered. Hold a finger on the screen while powering on to be asked again.

Use ESP-IDF **5.4.x**: the Tab5's P4 is silicon v1.3, which 5.5 does not support. The C6 must run ESP-Hosted 2.12.x
(see `c6tool/`). Everything else (hardware facts, gotchas, architecture) is in [docs/NOTES.md](docs/NOTES.md).

## Where to change things

| To change | Edit | Then |
|---|---|---|
| Keymap, layers, labels | `tools/gen_keymap.py` output `engine/miryoku_keymap.h` (`python tools/gen_keymap.py <miryoku_zmk dir> engine/miryoku_keymap.h QWERTY`) | rerun desktop tests, flash both halves |
| Hold timing, hold-tap rules | `engine/keymap_engine.cpp` | tests, flash left half |
| Mouse keys, caps word, BT profiles | `engine/specials.h` | tests, flash left half |
| Key sounds (sounds, pitch, volume curve) | `engine/keysound.h`, controls in `firmware/main/main.cpp` (`sound_special`, `label_at`) | `tests/test_keysound.cpp`, flash both halves |
| Key size, layout, tilt | `core/key_layout.h/.cpp` | tests, flash both halves |
| Colours, text, status text, UI speed | `firmware/main/main.cpp` | flash both halves |
| Bluetooth, pairing, split link, settings storage | `firmware/components/ble_kb/` | flash both halves (the link format is shared) |
| IMU / auto-flip | `firmware/components/imu/`, `core/autorotate.h` | flash both halves |
| C6 radio firmware | `c6tool/` (flash once per half) | see docs/NOTES.md |

Split link note: both halves must run the same `ble_kb` and `main.cpp` version. The status write is 6 bytes
(layer, mods, 3 keys, sound settings).

## Tests (no hardware needed)

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build --output-on-failure

Covers the keymap engine, touch tracking and layout, and the key-sound synth. The firmware itself can only be
built with ESP-IDF on a PC with the Tab5s attached.

## Layout of the repository

    engine/      keymap engine, generated Miryoku table, special actions, key-sound synth (no hardware code)
    core/        touch tracker, key layout and hit-testing, rotation and auto-flip logic (no hardware code)
    firmware/    ESP-IDF project: main/main.cpp (UI, touch, audio), components/ble_kb, components/imu
    c6tool/      separate ESP-IDF project that updates the Tab5's ESP32-C6 to ESP-Hosted 2.12.13
    tools/       keymap generator, Bosch driver download script
    tests/       desktop tests
    docs/        NOTES.md: hardware facts, gotchas, architecture, build and flash

## Third-party code

`firmware/components/imu/bosch/` holds Bosch Sensortec's BMI270 driver (BSD-3-Clause, see the file headers).
The C6 image is Espressif's prebuilt ESP-Hosted release, downloaded by `c6tool/get_c6_fw.ps1` and not stored here.
No licence has been chosen for the rest of the code yet.
