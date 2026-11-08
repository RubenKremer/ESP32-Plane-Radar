<#
.SYNOPSIS
  Build Plane Radar and produce a single .bin for browser-based flashers
  (esptool-js, etc.). PowerShell port of merge-firmware.sh for Windows.

.PARAMETER NoBuild
  Skip "pio run" (merge only; firmware must already be built).

.PARAMETER PioEnv
  PlatformIO environment (default: supermini, or $env:PIOENV).

.PARAMETER Out
  Output file (default: release\plane-radar-merged.bin).

.EXAMPLE
  .\scripts\merge-firmware.ps1
.EXAMPLE
  .\scripts\merge-firmware.ps1 -NoBuild
#>
[CmdletBinding()]
param(
  [switch]$NoBuild,
  [Alias('Env')]
  [string]$PioEnv = $(if ($env:PIOENV) { $env:PIOENV } else { 'supermini' }),
  [string]$Out
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $Root 'release\plane-radar-merged.bin' }

. (Join-Path $PSScriptRoot 'firmware-common.ps1')

$pio = Find-Pio
if (-not $pio) {
  Write-Error @'
PlatformIO (pio) not found.

Install it with:
  python -m pip install --user platformio
'@
}

Push-Location $Root
try {
  if (-not $NoBuild) {
    Invoke-Native $pio @('run', '-e', $PioEnv)
  }
  Invoke-Native $pio @('run', '-t', 'merge', '-e', $PioEnv)

  $merged = Join-Path $Root ".pio\build\$PioEnv\firmware-merged.bin"
  if (-not (Test-Path -LiteralPath $merged)) {
    Write-Error "Expected merged image not found: $merged"
  }

  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
  Copy-Item -LiteralPath $merged -Destination $Out -Force
  Write-Host "Wrote $Out"
  Write-Host 'Flash at offset 0x0 with chip ESP32-C3, 4MB flash (Web Serial flasher).'
} finally {
  Pop-Location
}
