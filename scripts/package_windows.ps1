param([string]$StudioHome)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $StudioHome) {
    $property = Get-Content -LiteralPath (Join-Path $projectRoot 'local.properties') |
        Where-Object { $_ -match '^sdk\.dir=' } | Select-Object -First 1
    if (-not $property) { throw 'Set sdk.dir in local.properties or pass -StudioHome.' }
    $StudioHome = Split-Path -Parent ($property.Substring('sdk.dir='.Length).Trim())
}
$required = @(
    'entry/src/main/resources/rawfile/wine-data.zip',
    'entry/src/main/resources/rawfile/wine-runtime-manifest.json',
    'entry/libs/arm64-v8a/box64.so'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $relative))) {
        throw "Missing Linux build artifact: $relative. Run make assemble first."
    }
}
$env:NODE_HOME = Join-Path $StudioHome 'tools/node'
$env:JAVA_HOME = Join-Path $StudioHome 'jbr'
$env:DEVECO_SDK_HOME = Join-Path $StudioHome 'sdk'
$env:Path = "$env:NODE_HOME;$env:JAVA_HOME/bin;$(Join-Path $StudioHome 'tools/ohpm/bin');$env:Path"
$wrapper = Join-Path $StudioHome 'tools/hvigor/bin/hvigorw.js'
Push-Location $projectRoot
try {
    & (Join-Path $env:NODE_HOME 'node.exe') $wrapper assembleHap --no-daemon
    if ($LASTEXITCODE -ne 0) { throw "HAP build failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
