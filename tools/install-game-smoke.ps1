[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot,
    [switch]$Install
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
# One folder for the player to install. Inside it sit this mod and the modules
# the Klissans need, each still the build its own author shipped -- only the
# paths in their CFG were rewritten, because the engine addresses a mod's files
# from the game root and moving a folder otherwise orphans every one of them.
$pack = Join-Path $projectRoot "dist\pack\ChildrenOfEltan"
$source = Join-Path $pack "ChildrenOfEltan"
$modsRoot = Join-Path $GameRoot "Mods"
$target = Join-Path $modsRoot "ChildrenOfEltan"

if (-not (Test-Path -LiteralPath (Join-Path $GameRoot "Rangers.exe"))) {
    throw "Rangers.exe not found under: $GameRoot"
}
& (Join-Path $PSScriptRoot "build-game-smoke.ps1") -OutputRoot $source -GameRoot $GameRoot
foreach ($embedded in @("ShuKlissan", "UtilityFunctionsPack")) {
    if (-not (Test-Path -LiteralPath (Join-Path $pack $embedded))) {
        throw "$embedded is missing from $pack. Run tools/embed_module.py for it first."
    }
}
if ($LASTEXITCODE -ne 0) { throw "Smoke build failed" }
Write-Output "Dry run: source=$pack"
Write-Output "Dry run: target=$target"
if (-not $Install) {
    Write-Output "No game files changed. Re-run with -Install to copy the isolated module."
    return
}
if (Test-Path -LiteralPath $target) {
    throw "Refusing to overwrite existing module: $target"
}
New-Item -ItemType Directory -Path $target | Out-Null
Copy-Item -Path (Join-Path $pack "*") -Destination $target -Recurse -Force
Write-Output "OK: installed isolated module at $target"
Write-Output "The module is not enabled; activate Children of Eltan in the game menu."
