# tab5kb: 36-key Miryoku split keyboard on two M5Stack Tab5

See docs/NOTES.md for how it works and how to build and flash it. Original engine notes below.

# tab5kb engine

Hardware-independent keymap engine for the 36-key split keyboard on two M5Stack Tab5s.
Behaves like Miryoku ZMK (tap-preferred hold-taps, 200 ms, double-tap layer jumps).

    engine/keymap_types.h        Binding/Keycode types, key position order
    engine/keymap_engine.{h,cpp} keyEvent()/tick() in -> HID reports, layer changes, special actions out
    engine/miryoku_keymap.h      GENERATED from your fork (QWERTY base, 10 layers)
    tools/gen_keymap.py          regenerate:  python tools/gen_keymap.py <miryoku_zmk dir> engine/miryoku_keymap.h QWERTY
    tests/test_engine.cpp        desktop tests

Run tests (Windows, any C++17 compiler + CMake; in a VS Code terminal):

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build --output-on-failure

Key positions: 0-9 top row, 10-19 home row, 20-29 bottom row, 30-35 thumbs;
left half = columns 0-4 of each row + thumbs 30-32, right half = the rest (isLeftHalf()).
The right Tab5 only needs to send {pos, down/up, timestamp}; the left Tab5 runs the engine.

Not implemented yet (emitted as Special actions for the firmware): mouse keys/wheel,
caps word, BT profile select, output toggle, RGB, bootloader.
