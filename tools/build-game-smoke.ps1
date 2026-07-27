[CmdletBinding()]
param(
    [string]$OutputRoot = "",
    [string]$GameRoot = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $projectRoot "dist\ChildrenOfEltan"
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
$launcherIconExe = ""
if (-not [string]::IsNullOrWhiteSpace($GameRoot)) {
    $launcherIconExe = Join-Path $GameRoot "Rangers.exe"
}
& (Join-Path $PSScriptRoot "build-early-launcher.ps1") -OutputRoot $OutputRoot -IconExe $launcherIconExe
if ($LASTEXITCODE -ne 0) { throw "Early launcher build failed" }

# RScript treats ' as a string delimiter inside // comments too, so a comment
# holding an odd number of them opens a literal that swallows the code after it.
# The compiler then reports a syntax error hundreds of characters away, at some
# innocent declaration, with no hint of the real cause -- an afternoon lost the
# first time. Refuse to build instead.
$rsonLines = [IO.File]::ReadAllLines($sourceRson, (New-Object Text.UTF8Encoding($false, $true)))
$badComments = @()
for ($i = 0; $i -lt $rsonLines.Count; $i++) {
    $text = $rsonLines[$i].Trim()
    if (-not $text.StartsWith('"//')) { continue }
    if ((($text.ToCharArray() | Where-Object { $_ -eq "'" }).Count % 2) -ne 0) {
        $badComments += ("  line {0}: {1}" -f ($i + 1), $text)
    }
}
if ($badComments.Count -gt 0) {
    throw ("Comment with an unpaired apostrophe in $sourceRson -- RScript reads it as an unterminated string:`n" + ($badComments -join "`n"))
}

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

# Every BlockPar source is staged under one tree with the name it will carry in
# the module, and the whole tree is converted in a single call to
# tools\srblockpar.py -- which is still BlockParEditor --cli --convert, one
# invocation per file, but planned and reported in one place. Staged outside
# CFG\ on purpose: RScript writes CE_MapSmoke.txt into CFG\Rus, and a batch
# rooted at CFG would sweep that up as a BlockPar source too.
$blockParStage = Join-Path ([IO.Path]::GetTempPath()) ("ce-blockpar-" + [guid]::NewGuid().ToString("N"))
$stageLangRoot = Join-Path $blockParStage "Rus"
New-Item -ItemType Directory -Path $stageLangRoot -Force | Out-Null
$stagedMain = Join-Path $blockParStage "Main.txt"
$stagedCache = Join-Path $blockParStage "CacheData.txt"
$stagedLang = Join-Path $stageLangRoot "Lang.txt"
Copy-Item -LiteralPath $sourceMain -Destination $stagedMain -Force
Copy-Item -LiteralPath $sourceCache -Destination $stagedCache -Force

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
[IO.File]::WriteAllBytes($stagedLang, $combinedBytes)

& python (Join-Path $PSScriptRoot "srblockpar.py") $blockParStage --to-dat --blockpar $blockPar
$blockParExit = $LASTEXITCODE
if ($blockParExit -ne 0) {
    Remove-Item -LiteralPath $blockParStage -Recurse -Force -ErrorAction SilentlyContinue
    throw "BlockPar batch conversion failed (exit $blockParExit)"
}
Move-Item -LiteralPath (Join-Path $blockParStage "Main.dat") -Destination $outputMain -Force
Move-Item -LiteralPath (Join-Path $stageLangRoot "Lang.dat") -Destination $outputLang -Force
Move-Item -LiteralPath (Join-Path $blockParStage "CacheData.dat") -Destination $outputCache -Force
Remove-Item -LiteralPath $blockParStage -Recurse -Force -ErrorAction SilentlyContinue

if (-not (Test-Path -LiteralPath $outputMain) -or (Get-Item -LiteralPath $outputMain).Length -lt 64) {
    throw "BlockParEditor did not produce CFG\Main.dat"
}
if (-not (Test-Path -LiteralPath $outputLang) -or (Get-Item -LiteralPath $outputLang).Length -lt 32) {
    throw "BlockParEditor did not produce CFG\Rus\Lang.dat"
}
if (-not (Test-Path -LiteralPath $outputCache) -or (Get-Item -LiteralPath $outputCache).Length -lt 32) {
    throw "BlockParEditor did not produce CFG\CacheData.dat"
}

# No package and no INSTALL.TXT: every module in the Universe pack is just
# CFG, DATA and ModuleInfo, and the mod manager refused ours while it carried
# more than that. The script is addressed by its real path instead, which is
# what a loose file needs -- data\... only resolves inside a package.

# ModuleInfo is read as UTF-16LE: a working example from another mod starts
# with a byte-order mark and CRLF, and ours went out as UTF-8, which is why
# every Russian line showed up as mojibake in the in-game mod list. Kept as
# plain UTF-8 in the repository so it stays readable and diffable, converted
# on the way out.
$moduleInfoText = [IO.File]::ReadAllText(
    (Join-Path $projectRoot "smoke_module\ModuleInfo.txt"),
    (New-Object Text.UTF8Encoding($false, $true)))
$moduleInfoText = $moduleInfoText -replace "`r`n", "`n" -replace "`n", "`r`n"
[IO.File]::WriteAllBytes(
    (Join-Path $OutputRoot "ModuleInfo.txt"),
    ([Text.Encoding]::Unicode.GetPreamble() + [Text.Encoding]::Unicode.GetBytes($moduleInfoText)))

& python (Join-Path $PSScriptRoot "game_smoke_check.py") preflight --module $OutputRoot
if ($LASTEXITCODE -ne 0) { throw "Smoke module preflight failed" }
Write-Output "OK: smoke module built at $OutputRoot"
