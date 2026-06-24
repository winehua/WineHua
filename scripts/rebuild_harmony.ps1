<#
.SYNOPSIS
Unified Windows-host entrypoint for WineHua Harmony builds.

.DESCRIPTION
Chooses a host backend (MSYS2 by default, WSL as fallback), runs the
single-shell inner build orchestration, and optionally installs, launches,
and collects logs from a target device or emulator.

.PARAMETER Mode
Build or runtime action to execute. Supported values:
doctor, full, incremental, wine, package, deploy, logs.

.PARAMETER Backend
Host backend selection: auto, msys2, or wsl.

.PARAMETER Arch
Target architecture: x86_64, arm64, or all.

.PARAMETER Target
HDC target key or ip:port. Use auto only when exactly one target is visible.
#>
[CmdletBinding()]
param(
    [ValidateSet('doctor', 'full', 'incremental', 'wine', 'package', 'deploy', 'logs')]
    [string]$Mode = 'incremental',

    [ValidateSet('x86_64', 'arm64', 'all')]
    [string]$Arch = 'x86_64',

    [ValidateSet('auto', 'msys2', 'wsl')]
    [string]$Backend = 'auto',

    [string]$Target = 'auto',
    [int]$WaitSeconds = 20,
    [int]$TailLines = 260,
    [string]$HdcPath = '',
    [string]$WslExe = 'wsl.exe',
    [string]$WslDistro = '',
    [string]$MsysRoot = 'C:\msys64',
    [switch]$NoAutoHeal,
    [switch]$SkipInstall,
    [switch]$SkipLaunch,
    [switch]$SkipLogs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildScript = Join-Path $RepoRoot 'scripts\rebuild_harmony.sh'
$SignedHap = Join-Path $RepoRoot 'entry\build\default\outputs\default\entry-default-signed.hap'
$BundleName = 'app.hackeris.winehua'
$AbilityName = 'EntryAbility'
$LogPattern = 'wine|winehua_audio_smoke|MediaReg|AudioBroker|quartz|mci|mp3dmod|devenum|Alarm01|testTag|MW-NAPI|WL-|WineWM|cmd\.exe|notepad\.exe|c0000135|wineboot|wineserver|Mono|Gecko|prefix'

function Write-Info {
    param([string]$Message)
    Write-Host "[rebuild] $Message"
}

function Convert-ToPosixPath {
    param(
        [Parameter(Mandatory = $true)][string]$WindowsPath,
        [Parameter(Mandatory = $true)][ValidateSet('wsl', 'msys2')][string]$Style
    )

    $full = [System.IO.Path]::GetFullPath($WindowsPath)
    if ($full -notmatch '^(?<drive>[A-Za-z]):\\(?<rest>.*)$') {
        throw "Unsupported Windows path for $Style conversion: $WindowsPath"
    }

    $drive = $Matches.drive.ToLowerInvariant()
    $rest = $Matches.rest -replace '\\', '/'
    switch ($Style) {
        'wsl' {
            if ([string]::IsNullOrEmpty($rest)) {
                return "/mnt/$drive"
            }
            return "/mnt/$drive/$rest"
        }
        'msys2' {
            if ([string]::IsNullOrEmpty($rest)) {
                return "/$drive"
            }
            return "/$drive/$rest"
        }
    }
}

function Resolve-HdcPath {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        if (-not (Test-Path $RequestedPath)) {
            throw "HDC path does not exist: $RequestedPath"
        }
        return (Resolve-Path $RequestedPath).Path
    }

    $candidates = @(
        $env:HDC_PATH,
        'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe',
        'D:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
    ) | Where-Object { $_ }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $cmd = Get-Command hdc.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    throw 'Unable to locate hdc.exe. Pass -HdcPath or install DevEco Studio toolchains.'
}

function Resolve-MsysBashPath {
    param([string]$RequestedRoot)

    $candidates = @(
        $env:MSYS2_BASH,
        (Join-Path $RequestedRoot 'usr\bin\bash.exe'),
        'C:\msys64\usr\bin\bash.exe'
    ) | Where-Object { $_ }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Unable to locate MSYS2 bash.exe under $RequestedRoot. Run scripts\bootstrap_msys2.ps1 first or pass -MsysRoot."
}

function Test-WslAvailable {
    param([string]$ExePath)

    $cmd = Get-Command $ExePath -ErrorAction SilentlyContinue
    return $null -ne $cmd
}

function Test-MsysAvailable {
    param([string]$RootPath)

    try {
        Resolve-MsysBashPath -RequestedRoot $RootPath | Out-Null
        return $true
    } catch {
        return $false
    }
}

function Resolve-BuildBackend {
    if ($Backend -ne 'auto') {
        return $Backend
    }

    if (Test-MsysAvailable -RootPath $MsysRoot) {
        return 'msys2'
    }
    if (Test-WslAvailable -ExePath $WslExe) {
        return 'wsl'
    }

    throw 'No build backend is available. Install MSYS2 via scripts\bootstrap_msys2.ps1 or make sure WSL is installed.'
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

function Get-HdcTargets {
    param([string]$ToolPath)

    $lines = & $ToolPath list targets 2>$null
    if ($LASTEXITCODE -ne 0) {
        return @()
    }

    return ,@($lines | Where-Object { $_ -and $_ -ne '[Empty]' } | ForEach-Object {
        ($_ -split '\s+')[0]
    })
}

function Resolve-Target {
    param(
        [string]$ToolPath,
        [string]$RequestedTarget
    )

    if ($RequestedTarget -and $RequestedTarget -ne 'auto') {
        if ($RequestedTarget -match '^\d+\.\d+\.\d+\.\d+(?::\d+)?$') {
            $knownTargets = @(Get-HdcTargets -ToolPath $ToolPath)
            if ($knownTargets -notcontains $RequestedTarget) {
                Write-Info "hdc tconn $RequestedTarget"
                & $ToolPath tconn $RequestedTarget | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    throw "hdc tconn failed: $RequestedTarget"
                }
            }
        }
        return $RequestedTarget
    }

    $targets = @(Get-HdcTargets -ToolPath $ToolPath)
    if ($targets.Count -eq 0) {
        throw 'No HDC targets found. Start the emulator or connect a device, or pass -Target.'
    }
    if ($targets.Count -gt 1) {
        throw "Multiple HDC targets found: $($targets -join ', '). Pass -Target explicitly."
    }
    return $targets[0]
}

function Invoke-WslBuild {
    param(
        [string]$ModeName,
        [string]$ArchName,
        [bool]$DisableAutoHeal = $false
    )

    if (-not (Test-Path $BuildScript)) {
        throw "Build script does not exist: $BuildScript"
    }

    $repoWsl = Convert-ToPosixPath -WindowsPath $RepoRoot -Style 'wsl'
    $modeArg = $ModeName.Replace("'", "'""'""'")
    $archArg = $ArchName.Replace("'", "'""'""'")
    $command = "cd '$repoWsl' && bash './scripts/rebuild_harmony.sh' '$modeArg' '$archArg'"
    if ($DisableAutoHeal) {
        $command += " --no-auto-heal"
    }

    $args = @()
    if ($WslDistro) {
        $args += @('-d', $WslDistro)
    }
    $args += @('bash', '-lc', $command)

    Write-Info "WSL build: mode=$ModeName arch=$ArchName"
    Invoke-Checked -FilePath $WslExe -ArgumentList $args
}

function Invoke-MsysBuild {
    param(
        [string]$ModeName,
        [string]$ArchName,
        [bool]$DisableAutoHeal = $false
    )

    if (-not (Test-Path $BuildScript)) {
        throw "Build script does not exist: $BuildScript"
    }

    $bashPath = Resolve-MsysBashPath -RequestedRoot $MsysRoot
    $repoMsys = Convert-ToPosixPath -WindowsPath $RepoRoot -Style 'msys2'
    $modeArg = $ModeName.Replace("'", "'""'""'")
    $archArg = $ArchName.Replace("'", "'""'""'")
    $command = "cd '$repoMsys' && bash './scripts/rebuild_harmony.sh' '$modeArg' '$archArg'"
    if ($DisableAutoHeal) {
        $command += " --no-auto-heal"
    }

    $savedEnv = @{
        'MSYSTEM' = $env:MSYSTEM
        'CHERE_INVOKING' = $env:CHERE_INVOKING
        'MSYS' = $env:MSYS
    }

    try {
        $env:MSYSTEM = 'MSYS'
        $env:CHERE_INVOKING = '1'
        $env:MSYS = 'winsymlinks:nativestrict'
        Write-Info "MSYS2 build: mode=$ModeName arch=$ArchName root=$MsysRoot"
        Invoke-Checked -FilePath $bashPath -ArgumentList @('-lc', $command)
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

function Invoke-Build {
    param(
        [string]$ModeName,
        [string]$ArchName,
        [bool]$DisableAutoHeal = $false
    )

    $selectedBackend = Resolve-BuildBackend
    Write-Info "Selected backend: $selectedBackend"
    switch ($selectedBackend) {
        'msys2' { Invoke-MsysBuild -ModeName $ModeName -ArchName $ArchName -DisableAutoHeal:$DisableAutoHeal }
        'wsl' { Invoke-WslBuild -ModeName $ModeName -ArchName $ArchName -DisableAutoHeal:$DisableAutoHeal }
        default { throw "Unsupported backend: $selectedBackend" }
    }
}

function Install-Hap {
    param(
        [string]$ToolPath,
        [string]$ResolvedTarget
    )

    if (-not (Test-Path $SignedHap)) {
        throw "Signed HAP not found: $SignedHap"
    }

    Write-Info "Installing $SignedHap to $ResolvedTarget"
    & $ToolPath -t $ResolvedTarget uninstall -n $BundleName 2>$null
    Invoke-Checked -FilePath $ToolPath -ArgumentList @('-t', $ResolvedTarget, 'install', '-r', $SignedHap)
}

function Start-App {
    param(
        [string]$ToolPath,
        [string]$ResolvedTarget
    )

    Write-Info "Starting $BundleName/$AbilityName on $ResolvedTarget"
    Invoke-Checked -FilePath $ToolPath -ArgumentList @('-t', $ResolvedTarget, 'shell', 'aa', 'start', '-b', $BundleName, '-a', $AbilityName)
}

function Show-Logs {
    param(
        [string]$ToolPath,
        [string]$ResolvedTarget
    )

    Write-Info "Collecting filtered hilog lines from $ResolvedTarget"
    $raw = & $ToolPath -t $ResolvedTarget shell hilog -z ([string]($TailLines * 6))
    if ($LASTEXITCODE -ne 0) {
        throw "hilog read failed from $ResolvedTarget"
    }

    $matched = @($raw | Select-String $LogPattern | Select-Object -Last $TailLines | ForEach-Object { $_.Line })
    if ($matched.Count -eq 0) {
        Write-Info 'No filtered Wine logs matched; showing the raw tail instead.'
        $raw | Select-Object -Last ([Math]::Min($TailLines, 80))
        return
    }

    $matched
}

$ResolvedHdc = Resolve-HdcPath -RequestedPath $HdcPath

switch ($Mode) {
    'doctor' {
        Invoke-Build -ModeName 'doctor' -ArchName $Arch
        Write-Info "HDC path: $ResolvedHdc"
        Write-Info 'Visible HDC targets:'
        $targets = @(Get-HdcTargets -ToolPath $ResolvedHdc)
        if ($targets.Count -eq 0) {
            Write-Host '[rebuild]   (none)'
        } else {
            $targets | ForEach-Object { Write-Host "[rebuild]   $_" }
        }
    }

    'full' {
        Invoke-Build -ModeName 'full' -ArchName $Arch -DisableAutoHeal:$NoAutoHeal
        if (-not $SkipInstall) {
            $resolvedTarget = Resolve-Target -ToolPath $ResolvedHdc -RequestedTarget $Target
            Install-Hap -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            if (-not $SkipLaunch) {
                Start-App -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            }
            if (-not $SkipLogs) {
                Start-Sleep -Seconds $WaitSeconds
                Show-Logs -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            }
        }
    }

    'incremental' {
        Invoke-Build -ModeName 'incremental' -ArchName $Arch -DisableAutoHeal:$NoAutoHeal
        if (-not $SkipInstall) {
            $resolvedTarget = Resolve-Target -ToolPath $ResolvedHdc -RequestedTarget $Target
            Install-Hap -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            if (-not $SkipLaunch) {
                Start-App -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            }
            if (-not $SkipLogs) {
                Start-Sleep -Seconds $WaitSeconds
                Show-Logs -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            }
        }
    }

    'wine' {
        Invoke-Build -ModeName 'wine' -ArchName $Arch -DisableAutoHeal:$NoAutoHeal
        if (-not $SkipInstall) {
            $resolvedTarget = Resolve-Target -ToolPath $ResolvedHdc -RequestedTarget $Target
            Install-Hap -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            if (-not $SkipLaunch) {
                Start-App -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            }
            if (-not $SkipLogs) {
                Start-Sleep -Seconds $WaitSeconds
                Show-Logs -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            }
        }
    }

    'package' {
        Invoke-Build -ModeName 'package' -ArchName $Arch -DisableAutoHeal:$NoAutoHeal
        if (-not $SkipInstall) {
            $resolvedTarget = Resolve-Target -ToolPath $ResolvedHdc -RequestedTarget $Target
            Install-Hap -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            if (-not $SkipLaunch) {
                Start-App -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            }
            if (-not $SkipLogs) {
                Start-Sleep -Seconds $WaitSeconds
                Show-Logs -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
            }
        }
    }

    'deploy' {
        $resolvedTarget = Resolve-Target -ToolPath $ResolvedHdc -RequestedTarget $Target
        Install-Hap -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
        if (-not $SkipLaunch) {
            Start-App -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
        }
        if (-not $SkipLogs) {
            Start-Sleep -Seconds $WaitSeconds
            Show-Logs -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
        }
    }

    'logs' {
        $resolvedTarget = Resolve-Target -ToolPath $ResolvedHdc -RequestedTarget $Target
        Show-Logs -ToolPath $ResolvedHdc -ResolvedTarget $resolvedTarget
    }
}
