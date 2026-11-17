#requires -version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameDirectory,

    [Parameter(Mandatory = $true)]
    [string]$GameExecutable,

    [switch]$SkipBuild,

    [switch]$ReplaceExistingD3D9
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bridgeRoot = Join-Path $repoRoot 'bridge'
$gameRoot = (Resolve-Path $GameDirectory).Path

if ([IO.Path]::GetFileName($GameExecutable) -ne $GameExecutable) {
    throw "GameExecutable must be a file name only, not a path: '$GameExecutable'."
}

$gameExePath = Join-Path $gameRoot $GameExecutable
if (-not (Test-Path -LiteralPath $gameExePath -PathType Leaf)) {
    throw "Game executable not found: '$gameExePath'."
}

if (-not $SkipBuild) {
    Write-Host '=== Building RTX Remix Bridge (x64 server + x86 client) ===' -ForegroundColor Cyan
    Push-Location $bridgeRoot
    try {
        & .\build_bridge_release.bat
        $bridgeBuildExit = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    if ($bridgeBuildExit -ne 0) {
        throw "RTX Remix Bridge build failed with exit code $bridgeBuildExit."
    }
}

$bridgeOutput = Join-Path $bridgeRoot '_output'
$runtimeOutput = Join-Path $repoRoot '_output'
$clientDll = Join-Path $bridgeOutput 'd3d9.dll'
$serverExe = Join-Path $bridgeOutput '.trex\NvRemixBridge.exe'
$runtimeDll = Join-Path $runtimeOutput 'd3d9.dll'

foreach ($artifact in @($clientDll, $serverExe, $runtimeDll)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Required build artifact is missing: '$artifact'. Build the x64 runtime and x86/x64 Bridge first."
    }
}

$destinationClientDll = Join-Path $gameRoot 'd3d9.dll'
if (Test-Path -LiteralPath $destinationClientDll -PathType Leaf) {
    $existingHash = (Get-FileHash -LiteralPath $destinationClientDll -Algorithm SHA256).Hash
    $bridgeHash = (Get-FileHash -LiteralPath $clientDll -Algorithm SHA256).Hash
    if ($existingHash -ne $bridgeHash -and -not $ReplaceExistingD3D9) {
        throw "A different d3d9.dll already exists in the game directory. Back it up/remove it, or rerun with -ReplaceExistingD3D9 after confirming it can be replaced."
    }
}

$trexDirectory = Join-Path $gameRoot '.trex'
New-Item -ItemType Directory -Path $trexDirectory -Force | Out-Null

Write-Host '=== Deploying 64-bit Remix Runtime into .trex ===' -ForegroundColor Cyan
Get-ChildItem -LiteralPath $runtimeOutput -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $trexDirectory -Recurse -Force
}

Write-Host '=== Deploying Bridge server ===' -ForegroundColor Cyan
Copy-Item -LiteralPath $serverExe -Destination (Join-Path $trexDirectory 'NvRemixBridge.exe') -Force

Write-Host '=== Deploying 32-bit D3D9 Bridge client ===' -ForegroundColor Cyan
Copy-Item -LiteralPath $clientDll -Destination $destinationClientDll -Force

$bridgeConfig = Join-Path $trexDirectory 'bridge.conf'
if (-not (Test-Path -LiteralPath $bridgeConfig -PathType Leaf)) {
    Copy-Item -LiteralPath (Join-Path $bridgeRoot 'bridge.conf') -Destination $bridgeConfig
}

$configLines = @(Get-Content -LiteralPath $bridgeConfig)
$targetLine = "client.targetProcess = $GameExecutable"
$targetPattern = '^\s*#?\s*client\.targetProcess\s*=.*$'
$targetFound = $false
for ($i = 0; $i -lt $configLines.Count; $i++) {
    if ($configLines[$i] -match $targetPattern) {
        $configLines[$i] = $targetLine
        $targetFound = $true
        break
    }
}
if (-not $targetFound) {
    $configLines += $targetLine
}
[IO.File]::WriteAllLines(
    $bridgeConfig,
    [string[]]$configLines,
    (New-Object System.Text.UTF8Encoding($false))
)

Write-Host ''
Write-Host 'RTX Remix Bridge deployment completed.' -ForegroundColor Green
Write-Host "Game:       $gameExePath"
Write-Host "D3D9 proxy: $destinationClientDll"
Write-Host "Runtime:    $trexDirectory"
Write-Host "Bridge cfg: $bridgeConfig"
Write-Host ''
Write-Host 'Start SteamVR/OpenRBRVR, then launch the game with DXVK_ENABLE_VR=1 in its environment.' -ForegroundColor Yellow
