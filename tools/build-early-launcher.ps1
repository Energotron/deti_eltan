[CmdletBinding()]
param(
    [string]$ToolchainRoot = "",
    [string]$OutputRoot = "",
    [string]$IconExe = ""
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

# The launcher is its own process, so Windows draws it with the generic
# application icon while the game it starts carries its own. Borrow the game's,
# taken from the installed executable at build time rather than committed here:
# it is the game's artwork, and this repository has no business shipping a copy.
# Without a game to read it from the launcher simply builds without one.
$iconObject = ""
# The mod's own artwork wins when it is there; the game icon is the fallback,
# which is better than the generic application one but is still the game's.
$ownIcon = Join-Path $projectRoot "srcssets\launcher_icon.png"
$useOwnIcon = Test-Path -LiteralPath $ownIcon
if ($useOwnIcon -or (-not [string]::IsNullOrWhiteSpace($IconExe) -and (Test-Path -LiteralPath $IconExe))) {
    $stage = Join-Path ([IO.Path]::GetTempPath()) ("ce-launcher-icon-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    $icoPath = Join-Path $stage "launcher.ico"
    if ($useOwnIcon) {
        & (Join-Path $PSScriptRoot "make_launcher_icon.ps1") -Source $ownIcon -Destination $icoPath
        if ($LASTEXITCODE -ne 0) { throw "Building the launcher icon failed" }
    } else {
        & python (Join-Path $PSScriptRoot "extract_exe_icon.py") $IconExe $icoPath
        if ($LASTEXITCODE -ne 0) { throw "Icon extraction failed for $IconExe" }
    }
    $rcPath = Join-Path $stage "launcher.rc"
    [IO.File]::WriteAllText($rcPath, "1 ICON `"launcher.ico`"`r`n", [Text.Encoding]::ASCII)
    $iconObject = Join-Path $stage "launcher_icon.o"
    $windres = Join-Path $ToolchainRoot "bin\i686-w64-mingw32-windres.exe"
    if (-not (Test-Path -LiteralPath $windres)) { throw "Missing resource compiler: $windres" }
    & $windres --input-format=rc --output-format=coff --target=pe-i386 `
        -I $stage $rcPath $iconObject
    if ($LASTEXITCODE -ne 0) { throw "Resource compilation failed" }
}

$compileArgs = @(
    "--target=i686-w64-windows-gnu", "-std=c11", "-O2",
    "-Wall", "-Wextra", "-Werror", "-municode", "-mwindows", $source
)
if ($iconObject -ne "") { $compileArgs += $iconObject }
$compileArgs += @("-o", $launcher)
& $compiler @compileArgs
if ($LASTEXITCODE -ne 0) { throw "Early launcher compilation failed" }
if ($iconObject -ne "") {
    Remove-Item -LiteralPath (Split-Path -Parent $iconObject) -Recurse -Force -ErrorAction SilentlyContinue
    $resources = (& $objdump -h $launcher) -join "`n"
    if ($resources -notmatch "\.rsrc") { throw "Launcher was built without its icon resource" }
}

$machineLine = & $objdump -f $launcher | Select-String "architecture: i386"
if (-not $machineLine) { throw "Early launcher is not PE32/i386" }
$imports = (& $objdump -p $launcher) -join "`n"
if ($imports -notmatch "CreateRemoteThread" -or $imports -notmatch "WriteProcessMemory") {
    throw "Early launcher is missing required injection imports"
}

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $launcher
Write-Output "OK: $launcher"
Write-Output "SHA256: $($hash.Hash)"
