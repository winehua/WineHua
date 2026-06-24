<#
.SYNOPSIS
Bootstrap MSYS2 for WineHua on a Windows host.

.DESCRIPTION
Installs MSYS2 when missing, runs the required double-update flow,
installs the packages used by the repository, and prints the recommended
doctor command for follow-up validation.

.PARAMETER MsysRoot
MSYS2 installation root. The default is C:\msys64.

.PARAMETER InstallerPath
Optional path to a local MSYS2 self-extracting installer. When omitted,
the script downloads the latest official installer.
#>
[CmdletBinding()]
param(
    [string]$MsysRoot = 'C:\msys64',
    [string]$InstallerPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$DefaultInstallerUrl = 'https://repo.msys2.org/distrib/x86_64/msys2-base-x86_64-latest.sfx.exe'
$RequiredPackages = @(
    'git',
    'make',
    'pkgconf',
    'python',
    'perl',
    'zip',
    'unzip',
    'libtool',
    'autoconf-wrapper',
    'automake-wrapper',
    'patch',
    'diffutils',
    'cmake',
    'meson',
    'ninja'
)

function Write-Info {
    param([string]$Message)
    Write-Host "[bootstrap] $Message"
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter()][string[]]$ArgumentList = @()
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($ArgumentList -join ' ')"
    }
}

function Resolve-BashPath {
    param([string]$RootPath)

    $bashPath = Join-Path $RootPath 'usr\bin\bash.exe'
    if (-not (Test-Path $bashPath)) {
        throw "MSYS2 bash.exe not found under $RootPath"
    }
    return (Resolve-Path $bashPath).Path
}

function Invoke-MsysCommand {
    param(
        [Parameter(Mandatory = $true)][string]$BashPath,
        [Parameter(Mandatory = $true)][string]$Command
    )

    $savedEnv = @{
        'MSYSTEM' = $env:MSYSTEM
        'CHERE_INVOKING' = $env:CHERE_INVOKING
        'MSYS' = $env:MSYS
    }

    try {
        $env:MSYSTEM = 'MSYS'
        $env:CHERE_INVOKING = '1'
        $env:MSYS = 'winsymlinks:nativestrict'
        Invoke-Checked -FilePath $BashPath -ArgumentList @('-lc', $Command)
    } finally {
        foreach ($name in $savedEnv.Keys) {
            if ($null -eq $savedEnv[$name]) {
                Remove-Item "Env:$name" -ErrorAction SilentlyContinue
            } else {
                Set-Item "Env:$name" $savedEnv[$name]
            }
        }
    }
}

function Invoke-WithRetry {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Description,
        [int]$MaxAttempts = 3,
        [int]$DelaySeconds = 5
    )

    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            & $Action
            return
        } catch {
            if ($attempt -ge $MaxAttempts) {
                throw
            }

            Write-Warning "[bootstrap] $Description failed on attempt ${attempt}/${MaxAttempts}: $($_.Exception.Message)"
            Start-Sleep -Seconds ($DelaySeconds * $attempt)
        }
    }
}

function Ensure-MsysInstalled {
    param(
        [string]$RootPath,
        [string]$LocalInstallerPath
    )

    if (Test-Path (Join-Path $RootPath 'usr\bin\bash.exe')) {
        Write-Info "Existing MSYS2 installation detected at $RootPath"
        return
    }

    $leaf = Split-Path $RootPath -Leaf
    if ($leaf -ne 'msys64') {
        throw "This bootstrap currently expects the installation directory name to be 'msys64' (received: $RootPath)."
    }

    $parent = Split-Path $RootPath -Parent
    if (-not (Test-Path $parent)) {
        New-Item -ItemType Directory -Path $parent | Out-Null
    }

    $installerToUse = $LocalInstallerPath
    if (-not $installerToUse) {
        $tempDir = Join-Path $env:TEMP ("msys2-bootstrap-" + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $tempDir | Out-Null
        $installerToUse = Join-Path $tempDir 'msys2-base-x86_64-latest.sfx.exe'
        Write-Info "Downloading MSYS2 base installer from $DefaultInstallerUrl"
        Invoke-WebRequest -Uri $DefaultInstallerUrl -OutFile $installerToUse
    } elseif (-not (Test-Path $installerToUse)) {
        throw "InstallerPath does not exist: $installerToUse"
    }

    Write-Info "Extracting MSYS2 into $parent"
    Invoke-Checked -FilePath $installerToUse -ArgumentList @('-y', "-o$parent")
}

Ensure-MsysInstalled -RootPath $MsysRoot -LocalInstallerPath $InstallerPath

$bashPath = Resolve-BashPath -RootPath $MsysRoot

Write-Info 'Initializing MSYS2 shell'
Invoke-MsysCommand -BashPath $bashPath -Command ' '

Write-Info 'Updating MSYS2 core packages (pass 1/2)'
Invoke-WithRetry -Description 'MSYS2 update pass 1/2' -Action {
    Invoke-MsysCommand -BashPath $bashPath -Command 'pacman --noconfirm -Syuu'
}

Write-Info 'Updating MSYS2 remaining packages (pass 2/2)'
Invoke-WithRetry -Description 'MSYS2 update pass 2/2' -Action {
    Invoke-MsysCommand -BashPath $bashPath -Command 'pacman --noconfirm -Syuu'
}

$packageArgs = $RequiredPackages -join ' '
Write-Info "Installing required packages: $packageArgs"
Invoke-WithRetry -Description 'MSYS2 package install' -Action {
    Invoke-MsysCommand -BashPath $bashPath -Command "pacman --noconfirm --needed -S $packageArgs"
}

Write-Info 'Verifying bash and pacman'
Invoke-MsysCommand -BashPath $bashPath -Command 'bash --version | head -n 1 && pacman -Q git make pkgconf python perl zip unzip libtool autoconf-wrapper automake-wrapper patch diffutils cmake meson ninja'

Write-Host ''
Write-Host 'Recommended verification command:'
Write-Host "powershell -NoProfile -ExecutionPolicy Bypass -File `"$RepoRoot\scripts\rebuild_harmony.ps1`" -Backend msys2 -Mode doctor -Arch x86_64"
