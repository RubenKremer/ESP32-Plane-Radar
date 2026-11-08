<#
.SYNOPSIS
  Flash Plane Radar to an ESP32-C3 Super Mini via esptool.
  PowerShell port of flash-firmware.sh for Windows.

.DESCRIPTION
  By default flashes the merged image (bootloader + partitions + app) at
  offset 0x0. That image pads the gaps between its parts with blank bytes,
  so it also erases NVS: saved Wi-Fi credentials and radar settings.

  With -KeepSettings it instead writes the bootloader, partition table,
  boot_app0 and app from the PlatformIO build folder at their own offsets,
  leaving NVS untouched.

  If flashing fails, put the board in download mode: hold BOOT, tap RESET,
  release BOOT.

.PARAMETER Port
  Serial port, e.g. COM5 (default: auto-detect the Espressif USB device).

.PARAMETER Firmware
  Merged .bin to flash (default: release\plane-radar-merged.bin).
  Ignored with -KeepSettings.

.PARAMETER Baud
  Upload baud rate (default: 921600).

.PARAMETER Build
  Build and merge firmware before flashing.

.PARAMETER KeepSettings
  Keep saved Wi-Fi and radar settings (write the parts, not the merged image).

.PARAMETER PioEnv
  PlatformIO environment (default: supermini, or $env:PIOENV).

.EXAMPLE
  .\scripts\flash-firmware.ps1 -Build -KeepSettings
.EXAMPLE
  .\scripts\flash-firmware.ps1 -Port COM5
#>
[CmdletBinding()]
param(
  [string]$Port,
  [string]$Firmware,
  [int]$Baud = 921600,
  [switch]$Build,
  [switch]$KeepSettings,
  [Alias('Env')]
  [string]$PioEnv = $(if ($env:PIOENV) { $env:PIOENV } else { 'supermini' })
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'firmware-common.ps1')

function Find-SerialPort {
  $pio = Find-Pio
  if ($pio) {
    try {
      $exe = $pio[0]
      $rest = @()
      if ($pio.Count -gt 1) { $rest = $pio[1..($pio.Count - 1)] }
      $json = & $exe @rest device list --serial --json-output 2>$null
      if ($LASTEXITCODE -eq 0 -and $json) {
        # Windows PowerShell 5.1 emits a parsed JSON array as one object;
        # piping it through ForEach-Object unrolls it into its elements.
        $parsed = ($json -join "`n") | ConvertFrom-Json
        $devices = @($parsed | ForEach-Object { $_ })
        # Prefer the Espressif USB-Serial/JTAG (VID 303A).
        $esp = $devices | Where-Object {
          $_.hwid -match '303A:' -or $_.description -match 'Espressif|JTAG|serial debug'
        } | Select-Object -First 1
        if ($esp) { return $esp.port }
        # Otherwise accept a single non-Bluetooth port (BTHENUM = Bluetooth
        # serial links, which Windows lists alongside USB ones).
        $usb = @($devices | Where-Object { $_.hwid -notmatch 'BTHENUM' })
        if ($usb.Count -eq 1) { return $usb[0].port }
        return $null
      }
    } catch {
      # Fall through to the plain port list.
    }
  }
  $ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique)
  if ($ports.Count -eq 1) { return $ports[0] }
  return $null
}

$esptool = Find-Esptool
if (-not $esptool) {
  Write-Error @'
esptool not found.

Install PlatformIO and build once (pulls in esptool):
  python -m pip install --user platformio
  .\scripts\merge-firmware.ps1

Or install esptool directly:
  python -m pip install --user esptool
'@
}

Push-Location $Root
try {
  if ($Build) {
    & (Join-Path $PSScriptRoot 'merge-firmware.ps1') -PioEnv $PioEnv
  }

  # address -> file to write
  $images = [ordered]@{}
  if ($KeepSettings) {
    $buildDir = Join-Path $Root ".pio\build\$PioEnv"
    $bootApp0 = Join-Path $HOME '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
    $images['0x0'] = Join-Path $buildDir 'bootloader.bin'
    $images['0x8000'] = Join-Path $buildDir 'partitions.bin'
    $images['0xe000'] = $bootApp0
    $images['0x10000'] = Join-Path $buildDir 'firmware.bin'
  } else {
    if (-not $Firmware) {
      $release = Join-Path $Root 'release\plane-radar-merged.bin'
      $built = Join-Path $Root ".pio\build\$PioEnv\firmware-merged.bin"
      if (Test-Path -LiteralPath $release) { $Firmware = $release }
      elseif (Test-Path -LiteralPath $built) { $Firmware = $built }
    }
    if (-not $Firmware) {
      Write-Error 'Firmware image not found. Run .\scripts\merge-firmware.ps1 first, or pass -Firmware PATH.'
    }
    $images['0x0'] = $Firmware
  }

  foreach ($file in $images.Values) {
    if (-not (Test-Path -LiteralPath $file)) {
      Write-Error "Firmware file not found: $file (build first, or use -Build)"
    }
  }

  if (-not $Port) {
    $Port = Find-SerialPort
    if (-not $Port) {
      Write-Error @'
No serial port detected (or more than one and none is Espressif).

Connect the ESP32-C3 over USB and pass the port explicitly:
  .\scripts\flash-firmware.ps1 -Port COM5
'@
    }
  }
  if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $Port) {
    Write-Error "Serial port not found: $Port"
  }

  Write-Host 'Flashing Plane Radar'
  Write-Host '  chip:     esp32c3'
  Write-Host "  port:     $Port"
  Write-Host "  baud:     $Baud"
  Write-Host '  flash:    4MB @ 80MHz'
  if ($KeepSettings) {
    Write-Host '  settings: kept (NVS not written)'
  } else {
    Write-Host '  settings: ERASED (merged image covers NVS; use -KeepSettings to keep them)'
  }
  foreach ($addr in $images.Keys) {
    Write-Host ("  {0,-8}  {1}" -f $addr, $images[$addr])
  }

  $flashArgs = @('--chip', 'esp32c3', '--port', $Port, '--baud', "$Baud",
            'write_flash', '--flash_mode', 'keep', '--flash_freq', '80m',
            '--flash_size', '4MB')
  foreach ($addr in $images.Keys) {
    $flashArgs += $addr
    $flashArgs += $images[$addr]
  }
  Invoke-Native $esptool $flashArgs

  Write-Host 'Done. Reset the board or unplug/replug USB to run the new firmware.'
} finally {
  Pop-Location
}
