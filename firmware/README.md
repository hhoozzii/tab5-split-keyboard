# Firmware (ESP-IDF 5.4.x, ESP32-P4 rev v1.3, M5Stack Tab5)

Build and flash steps, hardware gotchas and architecture are in [../docs/NOTES.md](../docs/NOTES.md).
Short version (ESP-IDF 5.4 PowerShell):

    cd firmware
    idf.py set-target esp32p4        # first time only
    idf.py -p COM12 flash monitor    # one firmware for both halves; each Tab5 asks LEFT or RIGHT on its first boot

`components/kb` compiles `../engine` and `../core`, so keep the repository together.
Delete `sdkconfig` after editing `sdkconfig.defaults` (the defaults only apply when no sdkconfig exists).
