[CmdletBinding()]
param(
    [string]$OutputRoot = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $projectRoot "dist\ChildrenOfEltanSmoke"
}
$dataRoot = Join-Path $OutputRoot "DATA"
$scriptRoot = Join-Path $dataRoot "Script"
$assetOutputRoot = Join-Path $dataRoot "ChildrenOfEltan"
$cfgRoot = Join-Path $OutputRoot "CFG"
$langRoot = Join-Path $cfgRoot "Rus"
$rscript = Join-Path $projectRoot "references\tools\RScript_4.10f\RScript.exe"
$blockPar = Join-Path $projectRoot "references\tools\BlockParEditor_1.9\BlockParEditor.exe"
$sourceRson = Join-Path $projectRoot "src\scripts\CE_MapSmoke.rson"
$sourceMain = Join-Path $projectRoot "src\config\CE_MapSmoke.Main.txt"
$sourceLang = Join-Path $projectRoot "src\config\CE_MapSmoke.Lang.txt"
$sourcePortalLang = Join-Path $projectRoot "src\config\CE_MapSmoke.Portal.Lang.txt"
$sourceTransitLang = Join-Path $projectRoot "src\config\CE_InterarmTransit.Lang.txt"
$sourceCache = Join-Path $projectRoot "smoke_module\CFG\CacheData.txt"
$sourceMapBackground = Join-Path $projectRoot "src\assets\second_home_map_bg.gi"
$sourceAnchorIcon = Join-Path $projectRoot "src\assets\twin_home_anchor.gi"
$outputScr = Join-Path $scriptRoot "CE_MapSmoke.scr"
$outputText = Join-Path $langRoot "CE_MapSmoke.txt"
$outputMain = Join-Path $cfgRoot "Main.dat"
$outputLang = Join-Path $langRoot "Lang.dat"
$outputCache = Join-Path $cfgRoot "CacheData.dat"
$outputPackage = Join-Path $OutputRoot "ChildrenOfEltanSmoke.pkg"

foreach ($required in @($rscript, $blockPar, $sourceRson, $sourceMain, $sourceLang, $sourcePortalLang, $sourceTransitLang, $sourceCache, $sourceMapBackground, $sourceAnchorIcon)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing required file: $required" }
}
New-Item -ItemType Directory -Path $scriptRoot, $langRoot, $assetOutputRoot -Force | Out-Null
Copy-Item -LiteralPath $sourceMapBackground `
    -Destination (Join-Path $assetOutputRoot "SecondHomeMap.gi") -Force
Copy-Item -LiteralPath $sourceAnchorIcon `
    -Destination (Join-Path $assetOutputRoot "TwinHomeAnchor.gi") -Force

& (Join-Path $PSScriptRoot "build-engine-adapter.ps1") -OutputRoot $dataRoot
if ($LASTEXITCODE -ne 0) { throw "Engine adapter build failed" }
& (Join-Path $PSScriptRoot "build-early-launcher.ps1") -OutputRoot $OutputRoot
if ($LASTEXITCODE -ne 0) { throw "Early launcher build failed" }

$rscriptBefore = @(Get-Process RScript -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
$buildStarted = Get-Date
& $rscript --cli --build --full $sourceRson $outputScr $outputText
Start-Sleep -Milliseconds 750
$newRscript = @(Get-Process RScript -ErrorAction SilentlyContinue | Where-Object { $_.Id -notin $rscriptBefore })
if ($newRscript) { $newRscript | Stop-Process -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path -LiteralPath $outputScr) -or
    (Get-Item -LiteralPath $outputScr).Length -lt 64 -or
    (Get-Item -LiteralPath $outputScr).LastWriteTimeUtc -lt $buildStarted.ToUniversalTime().AddSeconds(-1)) {
    throw "RScript did not produce CE_MapSmoke.scr"
}

& $blockPar --cli --convert $sourceMain $outputMain
Start-Sleep -Milliseconds 500
if (-not (Test-Path -LiteralPath $outputMain) -or (Get-Item -LiteralPath $outputMain).Length -lt 64) {
    throw "BlockParEditor did not produce CFG\Main.dat"
}

$combinedLang = Join-Path ([IO.Path]::GetTempPath()) ("ce-map-smoke-lang-" + [guid]::NewGuid().ToString("N") + ".txt")
$windows1251 = [Text.Encoding]::GetEncoding(1251)
# Read as UTF-8 and re-encode, exactly like $sourceTransitLang below. This
# file used to be copied through as raw bytes, which shipped its UTF-8 text
# straight into BlockParEditor -- the engine reads Lang as CP1251, so every
# Cyrillic name arrived mojibake ("РРССРРЁ" in place of a star name). It only
# became visible once this file started contributing names the map actually
# draws; the sector names above had the same defect but were never rendered.
$langText = [IO.File]::ReadAllText($sourceLang, (New-Object Text.UTF8Encoding($false, $true)))
$langBytes = $windows1251.GetBytes($langText)
$portalBytes = [Text.Encoding]::ASCII.GetBytes("`r`n" + [IO.File]::ReadAllText($sourcePortalLang, [Text.Encoding]::ASCII))
$transitText = [IO.File]::ReadAllText($sourceTransitLang, (New-Object Text.UTF8Encoding($false, $true)))
$transitBytes = $windows1251.GetBytes("`r`n" + $transitText)
$combinedBytes = New-Object byte[] ($langBytes.Length + $portalBytes.Length + $transitBytes.Length)
[Array]::Copy($langBytes, 0, $combinedBytes, 0, $langBytes.Length)
[Array]::Copy($portalBytes, 0, $combinedBytes, $langBytes.Length, $portalBytes.Length)
[Array]::Copy($transitBytes, 0, $combinedBytes, $langBytes.Length + $portalBytes.Length, $transitBytes.Length)
[IO.File]::WriteAllBytes($combinedLang, $combinedBytes)
& $blockPar --cli --convert $combinedLang $outputLang
Start-Sleep -Milliseconds 500
Remove-Item -LiteralPath $combinedLang -Force -ErrorAction SilentlyContinue
if (-not (Test-Path -LiteralPath $outputLang) -or (Get-Item -LiteralPath $outputLang).Length -lt 32) {
    throw "BlockParEditor did not produce CFG\Rus\Lang.dat"
}

& $blockPar --cli --convert $sourceCache $outputCache
Start-Sleep -Milliseconds 500
if (-not (Test-Path -LiteralPath $outputCache) -or (Get-Item -LiteralPath $outputCache).Length -lt 32) {
    throw "BlockParEditor did not produce CFG\CacheData.dat"
}

& python (Join-Path $PSScriptRoot "build_smoke_pkg.py") $outputScr $outputPackage
if ($LASTEXITCODE -ne 0) { throw "PKG build failed" }

Copy-Item -LiteralPath (Join-Path $projectRoot "smoke_module\ModuleInfo.txt") -Destination $OutputRoot -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "smoke_module\INSTALL.TXT") -Destination $OutputRoot -Force

& python (Join-Path $PSScriptRoot "game_smoke_check.py") preflight --module $OutputRoot
if ($LASTEXITCODE -ne 0) { throw "Smoke module preflight failed" }
Write-Output "OK: smoke module built at $OutputRoot"
