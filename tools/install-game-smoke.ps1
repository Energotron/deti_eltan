[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot,
    [switch]$Install
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $projectRoot "dist\ChildrenOfEltanSmoke"
$modsRoot = Join-Path $GameRoot "Mods"
$target = Join-Path $modsRoot "ChildrenOfEltanSmoke"

if (-not (Test-Path -LiteralPath (Join-Path $GameRoot "Rangers.exe"))) {
    throw "Rangers.exe not found under: $GameRoot"
}
& (Join-Path $PSScriptRoot "build-game-smoke.ps1") -OutputRoot $source -GameRoot $GameRoot
if ($LASTEXITCODE -ne 0) { throw "Smoke build failed" }
Write-Output "Dry run: source=$source"
Write-Output "Dry run: target=$target"
if (-not $Install) {
    Write-Output "No game files changed. Re-run with -Install to copy the isolated module."
    return
}
if (Test-Path -LiteralPath $target) {
    throw "Refusing to overwrite existing module: $target"
}
New-Item -ItemType Directory -Path $target | Out-Null
Copy-Item -Path (Join-Path $source "*") -Destination $target -Recurse -Force
Write-Output "OK: installed isolated module at $target"
Write-Output "The module is not enabled; activate ChildrenOfEltanSmoke in the game menu."
