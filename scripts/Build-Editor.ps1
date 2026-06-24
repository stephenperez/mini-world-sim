param(
    [string]$EngineDir = $env:UNREAL_ENGINE_DIR,
    [string]$Configuration = "Development"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ProjectFile = Join-Path $ProjectRoot "MiniWorldSim.uproject"

. (Join-Path $PSScriptRoot "Find-Unreal.ps1")

$ResolvedEngineDir = Resolve-UnrealEngineDir -EngineDir $EngineDir
$Build = Join-Path $ResolvedEngineDir "Engine\Build\BatchFiles\Build.bat"

& $Build MiniWorldSimEditor Win64 $Configuration "-Project=$ProjectFile" -WaitMutex -NoHotReloadFromIDE
exit $LASTEXITCODE
