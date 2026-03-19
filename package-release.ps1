# package-release.ps1 — Build both architectures and create release zips.
# Usage: powershell -ExecutionPolicy Bypass -File package-release.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# Build both architectures
Write-Host "=== Building 64-bit ===" -ForegroundColor Cyan
& cmd.exe /c "$root\build-install-64.cmd"
if ($LASTEXITCODE -ne 0) { throw "64-bit build failed" }

Write-Host "=== Building 32-bit ===" -ForegroundColor Cyan
& cmd.exe /c "$root\build-install-32.cmd"
if ($LASTEXITCODE -ne 0) { throw "32-bit build failed" }

# Create release directory
$releaseDir = Join-Path $root "release"
if (Test-Path $releaseDir) { Remove-Item -Recurse -Force $releaseDir }
New-Item -ItemType Directory -Path $releaseDir | Out-Null

# Stage x64
$x64Dir = Join-Path $releaseDir "x64"
New-Item -ItemType Directory -Path $x64Dir | Out-Null
$x64Files = @(
    "build64\Release\x64dbg-rippy.dp64",
    "build64\Release\libcrypto-3-x64.dll",
    "build64\Release\libssl-3-x64.dll",
    "build64\Release\brotlicommon.dll",
    "build64\Release\brotlidec.dll",
    "build64\Release\WebView2Loader.dll"
)
foreach ($f in $x64Files) {
    Copy-Item (Join-Path $root $f) $x64Dir
}

# Stage x32
$x32Dir = Join-Path $releaseDir "x32"
New-Item -ItemType Directory -Path $x32Dir | Out-Null
$x32Files = @(
    "build32\Release\x64dbg-rippy.dp32",
    "build32\Release\libcrypto-3.dll",
    "build32\Release\libssl-3.dll",
    "build32\Release\brotlicommon.dll",
    "build32\Release\brotlidec.dll",
    "build32\Release\WebView2Loader.dll"
)
foreach ($f in $x32Files) {
    Copy-Item (Join-Path $root $f) $x32Dir
}

# Create zips
$x64Zip = Join-Path $releaseDir "x64dbg-rippy-x64.zip"
$x32Zip = Join-Path $releaseDir "x64dbg-rippy-x32.zip"

Compress-Archive -Path "$x64Dir\*" -DestinationPath $x64Zip
Compress-Archive -Path "$x32Dir\*" -DestinationPath $x32Zip

Write-Host ""
Write-Host "=== Release packages ===" -ForegroundColor Green
Write-Host "  $x64Zip"
Write-Host "  $x32Zip"
Write-Host ""
Write-Host "Upload these to GitHub Releases." -ForegroundColor Yellow
