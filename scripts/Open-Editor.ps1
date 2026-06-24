param(
    [string]$EngineDir = $env:UNREAL_ENGINE_DIR
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ProjectFile = Join-Path $ProjectRoot "MiniWorldSim.uproject"

. (Join-Path $PSScriptRoot "Find-Unreal.ps1")

$ResolvedEngineDir = Resolve-UnrealEngineDir -EngineDir $EngineDir
$Editor = Join-Path $ResolvedEngineDir "Engine\Binaries\Win64\UnrealEditor.exe"

Write-Host "Opening $ProjectFile with $Editor"
& $Editor $ProjectFile -log -windowed -ResX=688 -ResY=288
