#requires -Version 5.1
[CmdletBinding()]
param(
  [ValidateSet('Login', 'Status', 'Preflight', 'Probe', 'Serve', 'Pin')]
  [string]$Action = 'Preflight',

  [Parameter(Mandatory = $true)]
  [string]$Config,

  [string]$Python,

  [string]$AppId,

  [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-ConfigString {
  param(
    [Parameter(Mandatory = $true)] [object]$ConfigObject,
    [Parameter(Mandatory = $true)] [string]$Name,
    [bool]$Required = $false,
    [string]$DefaultValue = ''
  )

  $property = $ConfigObject.PSObject.Properties[$Name]
  if ($null -eq $property -or $null -eq $property.Value) {
    if ($Required) { throw "Missing required configuration property: $Name" }
    return $DefaultValue
  }
  if ($property.Value -isnot [string]) { throw "Configuration property $Name must be a string" }
  $value = $property.Value.Trim()
  if ($Required -and $value.Length -eq 0) { throw "Configuration property $Name must not be empty" }
  if ($value.Length -eq 0) { return $DefaultValue }
  return $value
}

function Get-ConfigPort {
  param([Parameter(Mandatory = $true)] [object]$ConfigObject)

  $property = $ConfigObject.PSObject.Properties['port']
  if ($null -eq $property -or $null -eq $property.Value) { return 8443 }
  try {
    $port = [Convert]::ToInt32($property.Value, [Globalization.CultureInfo]::InvariantCulture)
  } catch {
    throw 'Configuration property port must be an integer'
  }
  if ($port -lt 1 -or $port -gt 65535) { throw 'Configuration property port is out of range' }
  return $port
}

function Require-File {
  param([Parameter(Mandatory = $true)] [string]$Path, [Parameter(Mandatory = $true)] [string]$Label)

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label does not exist: $Path" }
  return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-ConfiguredPath {
  param(
    [Parameter(Mandatory = $true)] [string]$Path,
    [Parameter(Mandatory = $true)] [string]$ConfigDirectory
  )

  if ($Path.Length -eq 0 -or [IO.Path]::IsPathRooted($Path)) { return $Path }
  return Join-Path -Path $ConfigDirectory -ChildPath $Path
}

function Find-Python {
  param([string]$Requested)

  $candidates = if ($Requested.Length -gt 0) { @($Requested) } else { @('py', 'python') }
  foreach ($candidate in $candidates) {
    $command = Get-Command -Name $candidate -CommandType Application -ErrorAction SilentlyContinue |
      Select-Object -First 1
    if ($null -ne $command) { return $command.Source }
  }
  throw 'Python 3 was not found. Supply -Python with its executable path.'
}

$configPath = Require-File -Path $Config -Label 'Host configuration'
$configDirectory = Split-Path -Parent $configPath
try {
  $hostConfig = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
} catch {
  throw "Unable to parse Host configuration JSON: $($_.Exception.Message)"
}
if ($null -eq $hostConfig -or $hostConfig -isnot [pscustomobject]) {
  throw 'Host configuration must be a JSON object'
}

$hostScript = Require-File -Path (Join-Path $PSScriptRoot 'steam_host.py') -Label 'Steam Host program'
$origin = Get-ConfigString -ConfigObject $hostConfig -Name 'origin' -Required ($Action -in @('Preflight', 'Probe', 'Serve'))
$dataDir = Get-ConfigString -ConfigObject $hostConfig -Name 'dataDir' -Required ($Action -in @('Status', 'Preflight', 'Probe', 'Serve'))
$steamCmd = Get-ConfigString -ConfigObject $hostConfig -Name 'steamcmd' -Required ($Action -in @('Login', 'Status', 'Preflight', 'Probe', 'Serve'))
$steamUser = Get-ConfigString -ConfigObject $hostConfig -Name 'steamUser' -Required ($Action -in @('Login', 'Status', 'Preflight', 'Probe', 'Serve'))
$certificate = Get-ConfigString -ConfigObject $hostConfig -Name 'certificate' -Required ($Action -in @('Preflight', 'Serve', 'Pin'))
$privateKey = Get-ConfigString -ConfigObject $hostConfig -Name 'privateKey' -Required ($Action -in @('Preflight', 'Serve'))
$bindAddress = Get-ConfigString -ConfigObject $hostConfig -Name 'bind' -DefaultValue '0.0.0.0'
$port = Get-ConfigPort -ConfigObject $hostConfig

$dataDir = Resolve-ConfiguredPath -Path $dataDir -ConfigDirectory $configDirectory
$steamCmd = Resolve-ConfiguredPath -Path $steamCmd -ConfigDirectory $configDirectory
$certificate = Resolve-ConfiguredPath -Path $certificate -ConfigDirectory $configDirectory
$privateKey = Resolve-ConfiguredPath -Path $privateKey -ConfigDirectory $configDirectory

if ($steamCmd.Length -gt 0) { $steamCmd = Require-File -Path $steamCmd -Label 'SteamCMD executable' }
if ($certificate.Length -gt 0) { $certificate = Require-File -Path $certificate -Label 'TLS certificate' }
if ($privateKey.Length -gt 0) { $privateKey = Require-File -Path $privateKey -Label 'TLS private key' }

$arguments = @()
switch ($Action) {
  'Login' {
    $arguments += '--steamcmd-login', '--steamcmd', $steamCmd, '--steam-user', $steamUser
  }
  'Status' {
    $arguments += '--steamcmd-status', '--data-dir', $dataDir, '--steamcmd', $steamCmd, '--steam-user', $steamUser
  }
  'Preflight' {
    $arguments += '--preflight', '--data-dir', $dataDir, '--config', $configPath, '--origin', $origin,
      '--cert', $certificate, '--key', $privateKey, '--port', "$port", '--steamcmd', $steamCmd, '--steam-user', $steamUser
  }
  'Probe' {
    if ($AppId -notmatch '^\d{1,20}$') { throw 'Probe requires -AppId with a numeric Steam AppID' }
    $arguments += '--probe-app', $AppId, '--data-dir', $dataDir, '--config', $configPath, '--origin', $origin,
      '--steamcmd', $steamCmd, '--steam-user', $steamUser
  }
  'Serve' {
    $arguments += '--data-dir', $dataDir, '--config', $configPath, '--origin', $origin,
      '--cert', $certificate, '--key', $privateKey, '--bind', $bindAddress, '--port', "$port",
      '--steamcmd', $steamCmd, '--steam-user', $steamUser
  }
  'Pin' {
    $arguments += '--print-pin', '--cert', $certificate
  }
}

if ($ValidateOnly) {
  [PSCustomObject]@{
    schemaVersion = 1
    action = $Action
    configurationValid = $true
    steamcmdConfigured = $steamCmd.Length -gt 0
    tlsConfigured = $certificate.Length -gt 0 -and $privateKey.Length -gt 0
  } | ConvertTo-Json -Compress
  exit 0
}

$pythonPath = Find-Python -Requested $Python
& $pythonPath $hostScript @arguments
exit $LASTEXITCODE
