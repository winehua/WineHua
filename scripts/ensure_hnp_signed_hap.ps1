[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$UnsignedHapPath,
    [Parameter(Mandatory = $true)][string]$SignedHapPath,
    [string]$EntryDir = '',
    [string[]]$PayloadPaths = @(),
    [string]$OhosSdkPath = '',
    [string]$NodePath = '',
    [string]$JavaPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$InjectScript = Join-Path $PSScriptRoot 'inject_hnp_into_hap.ps1'
$SignScript = Join-Path $RepoRoot 'sign.py'

function Write-Info {
    param([string]$Message)
    Write-Host "[ensure-hnp] $Message"
}

function Resolve-ExistingPath {
    param([string[]]$Candidates)

    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }

        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    return ''
}

function Resolve-CommandPath {
    param(
        [string]$RequestedPath,
        [string[]]$Candidates,
        [string[]]$Commands
    )

    $resolved = Resolve-ExistingPath -Candidates (@($RequestedPath) + $Candidates)
    if ($resolved) {
        return $resolved
    }

    foreach ($commandName in $Commands) {
        if ([string]::IsNullOrWhiteSpace($commandName)) {
            continue
        }

        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    return ''
}

function Get-InvokeParams {
    param(
        [string]$HapPath,
        [switch]$VerifyOnly
    )

    $invokeParams = @{
        HapPath = $HapPath
        EntryDir = $ResolvedEntryDir
    }

    if ($PayloadPaths.Count -gt 0) {
        $invokeParams['PayloadPaths'] = $PayloadPaths
    }
    if ($VerifyOnly) {
        $invokeParams['VerifyOnly'] = $true
    }

    return $invokeParams
}

function Test-HnpPayloadInHap {
    param([string]$HapPath)

    try {
        $invokeParams = Get-InvokeParams -HapPath $HapPath -VerifyOnly
        & $InjectScript @invokeParams
        return $true
    } catch {
        Write-Info $_.Exception.Message
        return $false
    }
}

$ResolvedUnsignedHapPath = (Resolve-Path $UnsignedHapPath).Path
$ResolvedSignedHapPath = (Resolve-Path $SignedHapPath).Path
$ResolvedEntryDir = if ($EntryDir) {
    (Resolve-Path $EntryDir).Path
} else {
    (Resolve-Path (Join-Path $RepoRoot 'entry')).Path
}

if (Test-HnpPayloadInHap $ResolvedSignedHapPath) {
    Write-Info 'Signed HAP already contains HNP payloads'
    exit 0
}

Write-Info 'Signed HAP is missing HNP payloads; repairing via unsigned HAP and re-signing'

$invokeParams = Get-InvokeParams -HapPath $ResolvedUnsignedHapPath
& $InjectScript @invokeParams

$ResolvedOhosSdkPath = Resolve-ExistingPath -Candidates @(
    @(
        $OhosSdkPath,
        $env:OHOS_SDK,
        $(if ($env:DEVECO_HOME) { Join-Path $env:DEVECO_HOME 'sdk\default\openharmony' }),
        'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony',
        'D:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony'
    )
)
if (-not $ResolvedOhosSdkPath) {
    throw 'Unable to locate OHOS SDK for sign.py. Set OHOS_SDK or pass -OhosSdkPath.'
}

$DevEcoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $ResolvedOhosSdkPath))
$ResolvedNodePath = Resolve-CommandPath -RequestedPath $NodePath -Candidates @(
    $(if ($env:NODE_BIN) { $env:NODE_BIN }),
    $(if ($DevEcoRoot) { Join-Path $DevEcoRoot 'tools\node\node.exe' })
) -Commands @('node.exe', 'node')
if (-not $ResolvedNodePath) {
    throw 'Unable to locate Node.js for sign.py. Set NODE_BIN or pass -NodePath.'
}

$ResolvedJavaPath = Resolve-CommandPath -RequestedPath $JavaPath -Candidates @(
    $(if ($env:JAVA_BIN) { $env:JAVA_BIN }),
    $(if ($DevEcoRoot) { Join-Path $DevEcoRoot 'jbr\bin\java.exe' })
) -Commands @('java.exe', 'java')
if (-not $ResolvedJavaPath) {
    throw 'Unable to locate Java for sign.py. Set JAVA_BIN or pass -JavaPath.'
}

$PythonCommand = Resolve-CommandPath -RequestedPath '' -Candidates @() -Commands @('py.exe', 'python.exe', 'python3.exe')
if (-not $PythonCommand) {
    throw 'Unable to locate Python for sign.py. Install py.exe/python.exe or add one to PATH.'
}

$savedOhosSdk = $env:OHOS_SDK
$savedNodeBin = $env:NODE_BIN
$savedJavaBin = $env:JAVA_BIN

try {
    $env:OHOS_SDK = $ResolvedOhosSdkPath
    $env:NODE_BIN = $ResolvedNodePath
    $env:JAVA_BIN = $ResolvedJavaPath

    Push-Location $RepoRoot
    try {
        if ([System.IO.Path]::GetFileName($PythonCommand).Equals('py.exe', [System.StringComparison]::OrdinalIgnoreCase)) {
            & $PythonCommand -3 $SignScript $ResolvedUnsignedHapPath $ResolvedSignedHapPath
        } else {
            & $PythonCommand $SignScript $ResolvedUnsignedHapPath $ResolvedSignedHapPath
        }

        if ($LASTEXITCODE -ne 0) {
            throw "sign.py failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
} finally {
    if ($null -eq $savedOhosSdk) {
        Remove-Item Env:OHOS_SDK -ErrorAction SilentlyContinue
    } else {
        $env:OHOS_SDK = $savedOhosSdk
    }

    if ($null -eq $savedNodeBin) {
        Remove-Item Env:NODE_BIN -ErrorAction SilentlyContinue
    } else {
        $env:NODE_BIN = $savedNodeBin
    }

    if ($null -eq $savedJavaBin) {
        Remove-Item Env:JAVA_BIN -ErrorAction SilentlyContinue
    } else {
        $env:JAVA_BIN = $savedJavaBin
    }
}

if (-not (Test-HnpPayloadInHap $ResolvedSignedHapPath)) {
    throw "Signed HAP still lacks HNP payloads after repair: $ResolvedSignedHapPath"
}

Write-Info 'Re-signed HAP with HNP payloads preserved'
