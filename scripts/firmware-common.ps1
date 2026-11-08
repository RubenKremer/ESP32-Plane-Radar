# Shared helpers for merge-firmware.ps1 and flash-firmware.ps1 (dot-sourced).

# A command is an array: the executable followed by any fixed leading
# arguments (e.g. python -m platformio).

function Find-Python {
  foreach ($name in @('python', 'python3', 'py')) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
  }
  return $null
}

function Test-Native([string[]]$Command, [string[]]$Arguments) {
  try {
    $exe = $Command[0]
    $rest = @()
    if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }
    & $exe @rest @Arguments *> $null
    return ($LASTEXITCODE -eq 0)
  } catch {
    return $false
  }
}

function Find-Pio {
  $candidates = @()
  $onPath = Get-Command pio -ErrorAction SilentlyContinue
  if ($onPath) { $candidates += , @($onPath.Source) }
  $penv = Join-Path $HOME '.platformio\penv\Scripts\pio.exe'
  if (Test-Path -LiteralPath $penv) { $candidates += , @($penv) }
  $python = Find-Python
  if ($python) { $candidates += , @($python, '-m', 'platformio') }

  foreach ($c in $candidates) {
    if (Test-Native $c @('--version')) { return , $c }
  }
  return $null
}

function Find-Esptool {
  $python = Find-Python
  $bundled = Join-Path $HOME '.platformio\packages\tool-esptoolpy\esptool.py'
  if ($python -and (Test-Path -LiteralPath $bundled)) { return , @($python, $bundled) }

  $onPath = Get-Command esptool -ErrorAction SilentlyContinue
  if ($onPath) { return , @($onPath.Source) }

  if ($python -and (Test-Native @($python, '-m', 'esptool') @('version'))) {
    return , @($python, '-m', 'esptool')
  }
  return $null
}

# Runs a command array plus arguments; throws when it exits non-zero.
function Invoke-Native([string[]]$Command, [string[]]$Arguments) {
  $exe = $Command[0]
  $rest = @()
  if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }
  & $exe @rest @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed (exit $LASTEXITCODE): $($Command -join ' ') $($Arguments -join ' ')"
  }
}
