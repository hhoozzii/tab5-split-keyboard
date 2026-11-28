# Builds the firmware and merges bootloader + partition table + app into ONE image that is flashed at address 0x0,
# the format M5Burner (and esptool) take. Run in the ESP-IDF 5.4 PowerShell:  powershell -File tools\make_burner_image.ps1
$root = Split-Path $PSScriptRoot -Parent
Set-Location "$root\firmware"
idf.py set-target esp32p4
idf.py build
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Set-Location "$root\firmware\build"
$out = "$root\tab5kb-merged.bin"
python -m esptool --chip esp32p4 merge_bin -o $out "@flash_args"
if ($LASTEXITCODE -ne 0) { throw "merge failed" }
Write-Host "Wrote $out  (flash at 0x0:  python -m esptool --chip esp32p4 -p COMx write_flash 0x0 tab5kb-merged.bin)"
