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
    $OutputRoot = Join-Path $projectRoot "dist\ChildrenOfEltan\DATA"
}
$compiler = Join-Path $ToolchainRoot "bin\i686-w64-mingw32-clang.exe"
$objdump = Join-Path $ToolchainRoot "bin\llvm-objdump.exe"
$source = Join-Path $projectRoot "src\engine_adapter\ce_second_map_adapter.c"
$include = Join-Path $projectRoot "src\engine_adapter"
$testSource = Join-Path $projectRoot "tools\native\test_ce_second_map_adapter.c"
$dll = Join-Path $OutputRoot "CESecondMapAdapter.dll"
$testDir = Join-Path ([IO.Path]::GetTempPath()) ("ce-adapter-test-" + [guid]::NewGuid().ToString("N"))
$testExe = Join-Path $testDir "test_ce_second_map_adapter.exe"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "Missing x86 compiler: $compiler"
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
New-Item -ItemType Directory -Path $testDir -Force | Out-Null

& $compiler --target=i686-w64-windows-gnu -std=c11 -O2 -Wall -Wextra -Werror `
    -shared -I $include $source -o $dll
if ($LASTEXITCODE -ne 0) { throw "Adapter DLL compilation failed" }

& $compiler --target=i686-w64-windows-gnu -std=c11 -O2 -Wall -Wextra -Werror `
    $testSource -o $testExe
if ($LASTEXITCODE -ne 0) { throw "Adapter test host compilation failed" }

$machineLine = & $objdump -f $dll | Select-String "architecture: i386"
if (-not $machineLine) { throw "Adapter DLL is not PE32/i386" }

& $testExe $dll
if ($LASTEXITCODE -ne 0) { throw "Adapter ABI smoke test failed" }

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $dll
Write-Output "OK: $dll"
Write-Output "SHA256: $($hash.Hash)"
