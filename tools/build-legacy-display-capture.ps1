[CmdletBinding()]
param(
    [string]$OutputRoot = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $projectRoot "dist\legacy-display-capture"
}
$toolchain = Join-Path $projectRoot "references\tools\llvm-mingw-20260407-ucrt-x86_64"
$compiler = Join-Path $toolchain "bin\x86_64-w64-mingw32-clang++.exe"
$objdump = Join-Path $toolchain "bin\llvm-objdump.exe"
$source = Join-Path $projectRoot "tools\native\ce_legacy_display_capture.cpp"
$output = Join-Path $OutputRoot "ce_legacy_display_capture.exe"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "Missing compiler: $compiler"
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

& $compiler --target=x86_64-w64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Werror `
    -municode $source -o $output -ld3d11 -ldxgi -lgdi32
if ($LASTEXITCODE -ne 0) { throw "Legacy display capture compilation failed" }

$machineLine = & $objdump -f $output | Select-String "architecture: x86_64"
if (-not $machineLine) { throw "Capture tool is not PE32+ x86-64" }

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $output
Write-Output "OK: $output"
Write-Output "SHA256: $($hash.Hash)"
