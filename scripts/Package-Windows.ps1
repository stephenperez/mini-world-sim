param(
    [string]$EngineDir = $env:UNREAL_ENGINE_DIR,
    [string]$Configuration = "Development",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ProjectFile = Join-Path $ProjectRoot "MiniWorldSim.uproject"
$ArchiveDir = if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    Join-Path $ProjectRoot "Build\Windows"
} else {
    $OutputDir
}

. (Join-Path $PSScriptRoot "Find-Unreal.ps1")

$ResolvedEngineDir = Resolve-UnrealEngineDir -EngineDir $EngineDir
$RunUAT = Join-Path $ResolvedEngineDir "Engine\Build\BatchFiles\RunUAT.bat"

Write-Host "Packaging $ProjectFile"
Write-Host "Engine:  $ResolvedEngineDir"
Write-Host "Output:  $ArchiveDir"

& $RunUAT `
    BuildCookRun `
    "-project=$ProjectFile" `
    -noP4 `
    -platform=Win64 `
    "-clientconfig=$Configuration" `
    "-serverconfig=$Configuration" `
    -cook `
    -build `
    -stage `
    -pak `
    -archive `
    "-archivedirectory=$ArchiveDir"

exit $LASTEXITCODE
