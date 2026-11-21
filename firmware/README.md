# Firmware skeleton (ESP-IDF 5.4.x, ESP32-P4 v1.3 Tab5)

NOT compiled or run by me: I had no ESP-IDF here. Everything it calls that is not
hardware (touch tracking, layout, engine) is unit-tested on the desktop
(`cmake -S .. -B ../build && cmake --build ../build && ctest --test-dir ../build`).

Build (ESP-IDF 5.4 PowerShell):

    cd firmware
    idf.py set-target esp32p4
    idf.py menuconfig          # Tab5 keyboard -> which half (left/right)
    idf.py -p COM12 flash monitor

Keep the whole `tab5kb` folder together: `components/kb` pulls `../engine` and `../core`.
