# Downloads the ESP-Hosted slave firmware for the ESP32-C6 (SDIO) into fw\ so the updater can embed it.
# Run in PowerShell:  powershell -ExecutionPolicy Bypass -File C:\vccode\tab5kb\c6tool\get_c6_fw.ps1
$ver = "v2.12.13"
$dest = Join-Path $PSScriptRoot "fw\network_adapter_esp32c6.bin"
New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
$url = "https://esphome.github.io/esp-hosted-firmware/$ver/network_adapter_esp32c6.bin"
Write-Host "downloading $url"
Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
(Get-Item $dest).LastWriteTime = Get-Date
$f = Get-Item $dest
Write-Host ("saved {0} bytes; SHA256 {1}" -f $f.Length, (Get-FileHash $dest -Algorithm SHA256).Hash)
$b = [System.IO.File]::ReadAllBytes($dest)
if ($b[0] -ne 0xE9) { throw "not an ESP32 app image (first byte is not 0xE9)" }
Write-Host ("chip id in image header: {0} (13 = ESP32-C6)" -f ($b[12] + 256 * $b[13]))
