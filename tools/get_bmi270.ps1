# Downloads Bosch's open-source BMI270 driver (it contains the sensor firmware) into components\imu\bosch.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File C:\vccode\tab5kb\tools\get_bmi270.ps1
$dest = Join-Path $PSScriptRoot "..\firmware\components\imu\bosch"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$base = "https://raw.githubusercontent.com/boschsensortec/BMI270_SensorAPI/master"
foreach ($f in "bmi2.c", "bmi2.h", "bmi2_defs.h", "bmi270.c", "bmi270.h") {
  Write-Host "downloading $f"
  Invoke-WebRequest -Uri "$base/$f" -OutFile (Join-Path $dest $f) -UseBasicParsing
  (Get-Item (Join-Path $dest $f)).LastWriteTime = Get-Date
}
Get-ChildItem $dest | Select-Object Name, Length
