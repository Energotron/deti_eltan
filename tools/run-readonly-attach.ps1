[CmdletBinding()]
param(
    [int]$TargetProcessId = 0,
    [string]$ZeroMask = "0818404000000400",
    [string]$PointerMask = "000300003000F801",
    [switch]$Exact,
    [string]$ToolPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ToolPath)) {
    $ToolPath = Join-Path $projectRoot "dist\readonly-attach\ce_galaxy_attach.exe"
}
if (-not (Test-Path -LiteralPath $ToolPath)) {
    throw "Attach tool is missing. Run tools\build-readonly-attach.ps1 first."
}
if ($TargetProcessId -eq 0) {
    $games = @(Get-Process -Name "Rangers" -ErrorAction SilentlyContinue)
    if ($games.Count -ne 1) {
        throw "Expected exactly one Rangers.exe process; found $($games.Count)."
    }
    $TargetProcessId = $games[0].Id
}

$arguments = @(
    "--pid", $TargetProcessId,
    "--zero-mask", $ZeroMask,
    "--pointer-mask", $PointerMask
)
if ($Exact) { $arguments += "--exact" }

& $ToolPath @arguments
exit $LASTEXITCODE
