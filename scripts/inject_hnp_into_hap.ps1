[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$HapPath,
    [string]$EntryDir = '',
    [string[]]$PayloadPaths = @(),
    [switch]$VerifyOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Write-Info {
    param([string]$Message)
    Write-Host "[inject-hnp] $Message"
}

function Read-TextFile {
    param([string]$Path)
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Test-HnpDeclared {
    param([string]$ModuleJsonPath)

    if (-not (Test-Path $ModuleJsonPath)) {
        return $false
    }

    return (Read-TextFile $ModuleJsonPath) -match '"hnpPackages"\s*:'
}

function Get-ConfiguredAbiFilters {
    param([string]$BuildProfilePath)

    if (-not (Test-Path $BuildProfilePath)) {
        return @()
    }

    $content = Read-TextFile $BuildProfilePath
    $match = [regex]::Match($content, '"abiFilters"\s*:\s*\[(?<items>[^\]]*)\]')
    if (-not $match.Success) {
        return @()
    }

    $filters = New-Object System.Collections.Generic.List[string]
    foreach ($item in [regex]::Matches($match.Groups['items'].Value, '"([^"]+)"')) {
        $value = $item.Groups[1].Value.Trim()
        if ($value) {
            [void]$filters.Add($value)
        }
    }
    return $filters.ToArray()
}

function Normalize-ArchName {
    param([string]$Abi)

    switch ($Abi) {
        'arm64' { return 'arm64-v8a' }
        'arm64-v8a' { return 'arm64-v8a' }
        'x86_64' { return 'x86_64' }
        default { return $Abi }
    }
}

function Get-RelativePayloads {
    param([string]$ResolvedEntryDir)

    if ($PayloadPaths.Count -gt 0) {
        return $PayloadPaths | ForEach-Object { $_ -replace '\\', '/' }
    }

    $resolved = New-Object System.Collections.Generic.List[string]
    $seen = New-Object 'System.Collections.Generic.HashSet[string]'
    $configuredAbis = Get-ConfiguredAbiFilters (Join-Path $ResolvedEntryDir 'build-profile.json5')

    foreach ($abi in $configuredAbis) {
        $arch = Normalize-ArchName $abi
        $candidate = "hnp/$arch/winehua.hnp"
        if ((Test-Path (Join-Path $ResolvedEntryDir $candidate)) -and $seen.Add($candidate)) {
            [void]$resolved.Add($candidate)
        }
    }

    if ($resolved.Count -eq 0) {
        $hnpRoot = Join-Path $ResolvedEntryDir 'hnp'
        if (Test-Path $hnpRoot) {
            foreach ($file in Get-ChildItem -Path $hnpRoot -Filter 'winehua.hnp' -Recurse -File | Sort-Object FullName) {
                $relative = $file.FullName.Substring($ResolvedEntryDir.Length).TrimStart('\').Replace('\', '/')
                if ($seen.Add($relative)) {
                    [void]$resolved.Add($relative)
                }
            }
        }
    }

    return $resolved.ToArray()
}

function Get-MissingEntries {
    param(
        [System.IO.Compression.ZipArchive]$Archive,
        [string[]]$Entries
    )

    $missing = New-Object System.Collections.Generic.List[string]
    foreach ($entryName in $Entries) {
        if (-not $Archive.GetEntry($entryName)) {
            [void]$missing.Add($entryName)
        }
    }
    return $missing.ToArray()
}

if (-not $EntryDir) {
    $EntryDir = Join-Path $PSScriptRoot '..\entry'
}

$resolvedEntryDir = (Resolve-Path $EntryDir).Path
$resolvedHapPath = (Resolve-Path $HapPath).Path
$moduleJsonPath = Join-Path $resolvedEntryDir 'src\main\module.json5'

if (-not (Test-HnpDeclared $moduleJsonPath)) {
    Write-Info 'module.json5 has no hnpPackages declaration; nothing to do'
    exit 0
}

$relativePayloads = @(Get-RelativePayloads $resolvedEntryDir)
if ($relativePayloads.Count -eq 0) {
    throw "No HNP payloads were resolved under $resolvedEntryDir\hnp"
}

foreach ($relativePayload in $relativePayloads) {
    $sourcePath = Join-Path $resolvedEntryDir $relativePayload
    if (-not (Test-Path $sourcePath)) {
        throw "HNP payload is missing: $sourcePath"
    }
}

if ($VerifyOnly) {
    $archive = [System.IO.Compression.ZipFile]::OpenRead($resolvedHapPath)
    try {
        $missing = @(Get-MissingEntries -Archive $archive -Entries $relativePayloads)
        if ($missing.Count -gt 0) {
            throw "Missing HNP payload entries: $($missing -join ', ')"
        }
    } finally {
        $archive.Dispose()
    }

    Write-Info "Verified HNP payloads in $resolvedHapPath"
    exit 0
}

$updateArchive = [System.IO.Compression.ZipFile]::Open($resolvedHapPath, [System.IO.Compression.ZipArchiveMode]::Update)
try {
    foreach ($relativePayload in $relativePayloads) {
        $entryName = $relativePayload -replace '\\', '/'
        $sourcePath = Join-Path $resolvedEntryDir $relativePayload
        $existingEntry = $updateArchive.GetEntry($entryName)
        if ($existingEntry) {
            $existingEntry.Delete()
        }

        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $updateArchive,
            $sourcePath,
            $entryName,
            [System.IO.Compression.CompressionLevel]::NoCompression
        ) | Out-Null
    }

    $missing = @(Get-MissingEntries -Archive $updateArchive -Entries $relativePayloads)
    if ($missing.Count -gt 0) {
        throw "Missing HNP payload entries after update: $($missing -join ', ')"
    }
} finally {
    $updateArchive.Dispose()
}

Write-Info "Injected HNP payloads into $resolvedHapPath"
