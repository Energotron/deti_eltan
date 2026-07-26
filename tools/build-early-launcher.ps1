[CmdletBinding()]
param(
    [string]$ToolchainRoot = "",
    [string]$OutputRoot = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    $ToolchainRoot = Join-Path $projectRoot "references\tools\llvm-mingw-20260407-ucrt-x86_64"
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $projectRoot "dist\ChildrenOfEltan"
}
$compiler = Join-Path $ToolchainRoot "bin\i686-w64-mingw32-clang.exe"
$objdump = Join-Path $ToolchainRoot "bin\llvm-objdump.exe"
$source = Join-Path $projectRoot "tools\native\ce_early_launcher.c"
$launcher = Join-Path $OutputRoot "ChildrenOfEltanLauncher.exe"

foreach ($required in @($compiler, $objdump, $source)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing required file: $required"
    }
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
& $compiler --target=i686-w64-windows-gnu -std=c11 -O2 -Wall -Wextra -Werror `
    -municode -mwindows $source -o $launcher
if ($LASTEXITCODE -ne 0) { throw "Early launcher compilation failed" }

$machineLine = & $objdump -f $launcher | Select-String "architecture: i386"
if (-not $machineLine) { throw "Early launcher is not PE32/i386" }
$imports = (& $objdump -p $launcher) -join "`n"
if ($imports -notmatch "CreateRemoteThread" -or $imports -notmatch "WriteProcessMemory") {
    throw "Early launcher is missing required injection imports"
}

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $launcher
Write-Output "OK: $launcher"
Write-Output "SHA256: $($hash.Hash)"
