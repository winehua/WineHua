#requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$wrapper = Join-Path $PSScriptRoot 'Start-SteamHost.ps1'
$fixture = Join-Path $PSScriptRoot 'testdata\host-wrapper-config.json'
$stateDirectory = Join-Path $PSScriptRoot 'testdata\state'
$hostExecutable = Join-Path $PSHOME 'pwsh.exe'
if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf)) {
  $hostExecutable = Join-Path $PSHOME 'powershell.exe'
}
if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf)) {
  throw 'Unable to locate the current PowerShell executable'
}
if (Test-Path -LiteralPath $stateDirectory) {
  throw 'The wrapper test fixture must not contain generated Host state'
}

$output = & $hostExecutable -NoProfile -File $wrapper -Action Preflight -Config $fixture -ValidateOnly 2>&1
if ($LASTEXITCODE -ne 0) { throw "Wrapper validation failed: $output" }
$report = ($output | Out-String | ConvertFrom-Json)
if (-not $report.configurationValid -or -not $report.steamcmdConfigured -or -not $report.tlsConfigured) {
  throw 'Wrapper did not validate the relative-path fixture'
}

$previousErrorActionPreference = $ErrorActionPreference
try {
  # Probe without an AppID is the intentional negative case. In PowerShell 7,
  # a native command's stderr becomes a terminating NativeCommandError under Stop.
  $ErrorActionPreference = 'Continue'
  $probeOutput = & $hostExecutable -NoProfile -File $wrapper -Action Probe -Config $fixture -ValidateOnly 2>&1
  $probeExitCode = $LASTEXITCODE
} finally {
  $ErrorActionPreference = $previousErrorActionPreference
}
if ($probeExitCode -eq 0) { throw 'Wrapper accepted Probe without an AppID' }
if (($probeOutput | Out-String) -notmatch 'Probe requires -AppId') {
  throw "Wrapper rejected Probe for an unexpected reason: $probeOutput"
}
if (Test-Path -LiteralPath $stateDirectory) {
  throw 'ValidateOnly unexpectedly created Host state'
}

Write-Output 'Start-SteamHost wrapper tests PASS'
