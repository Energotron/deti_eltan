param(
    [string]$OutputRoot = (Join-Path $PSScriptRoot "..\dist\readonly-attach")
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$source = Join-Path $repoRoot "tools\native\ce_galaxy_attach.c"
$toolchain = Join-Path $repoRoot "references\tools\llvm-mingw-20260407-ucrt-x86_64"
$compiler = Join-Path $toolchain "bin\i686-w64-mingw32-clang.exe"
$objdump = Join-Path $toolchain "bin\llvm-objdump.exe"
$output = Join-Path $OutputRoot "ce_galaxy_attach.exe"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "Missing compiler: $compiler"
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

& $compiler --target=i686-w64-windows-gnu -std=c11 -O2 -Wall -Wextra -Werror `
    $source -o $output
if ($LASTEXITCODE -ne 0) { throw "Read-only attach tool compilation failed" }

$machineLine = & $objdump -f $output | Select-String "architecture: i386"
if (-not $machineLine) { throw "Attach tool is not PE32/i386" }

& $output --self-test
if ($LASTEXITCODE -ne 0) { throw "Read-only matcher self-test failed" }

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $output
Write-Output "OK: $output"
Write-Output "SHA256: $($hash.Hash)"
