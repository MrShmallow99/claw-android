<#
.SYNOPSIS
    Builds the Android APK on Windows, inside Docker (Docker Desktop with the WSL 2 backend).

.DESCRIPTION
    Runs tools/build.sh in the build container (see Dockerfile). The first run builds the
    container image and downloads the Android SDK/NDK and the third-party sources, which
    takes a while; later runs reuse them.

.PARAMETER Mode
    picker  - Claw-folder-picker.apk (no game files inside; the app imports them on the device)
    bundled - Claw-with-game-files.apk (your game files packed into the APK; for your own use only)

.PARAMETER Game
    The CLAW-1.4.5.4-Complete-Pack.zip file, or the extracted game folder (the one with CLAW.exe).

.PARAMETER Abis
    Android ABIs, space-separated (default "arm64-v8a"; add "x86_64" for emulators).

.PARAMETER SkipHashCheck
    Accept a game pack that is not the exact supported release.

.PARAMETER RebuildImage
    Rebuild the Docker image even if it already exists.

.EXAMPLE
    .\tools\build.ps1 picker -Game "$HOME\Downloads\CLAW-1.4.5.4-Complete-Pack.zip"

.EXAMPLE
    .\tools\build.ps1 bundled -Game "D:\Games\CLAW-1.4.5.4"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('picker', 'bundled')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Abis = 'arm64-v8a',
    [switch]$SkipHashCheck,
    [switch]$RebuildImage
)
$ErrorActionPreference = 'Stop'
$Image = 'claw-android-build'

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'Docker was not found. Install Docker Desktop (https://www.docker.com/products/docker-desktop/) and start it.'
}
docker info *> $null
if ($LASTEXITCODE -ne 0) { throw 'Docker is installed but not running. Start Docker Desktop and try again.' }

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$GamePath = (Resolve-Path $Game).Path

docker image inspect $Image *> $null
if ($RebuildImage -or $LASTEXITCODE -ne 0) {
    Write-Host '==> Building the build image (first run only; downloads the Android SDK and NDK)'
    docker build -t $Image $Root
    if ($LASTEXITCODE -ne 0) { throw 'docker build failed' }
}

if (Test-Path $GamePath -PathType Leaf) {
    $Mount = "${GamePath}:/game/pack.zip:ro"
    $GameArg = '/game/pack.zip'
} else {
    $Mount = "${GamePath}:/game/folder:ro"
    $GameArg = '/game/folder'
}

$BuildArgs = @('tools/build.sh', $Mode, '--game', $GameArg, '--abis', $Abis)
if ($SkipHashCheck) { $BuildArgs += '--skip-hash-check' }

docker run --rm `
    -v "${Root}:/src" `
    -v $Mount `
    -e GRADLE_USER_HOME=/src/build/gradle-home `
    $Image bash @BuildArgs
if ($LASTEXITCODE -ne 0) { throw "the build failed (exit code $LASTEXITCODE)" }

Write-Host ''
Write-Host "The APK is in $Root\out"
