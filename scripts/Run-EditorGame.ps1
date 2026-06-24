param(
    [string]$EngineDir = $env:UNREAL_ENGINE_DIR,
    [int]$ResX = 688,
    [int]$ResY = 288,
    [string]$VisibilityMode = "night",
    [double]$VisibilityHorizonScale = 0.9823,
    [string]$Map = "",
    [string]$CaptureSpec = "",
    [string]$CaptureSpecDir = "",
    [string]$CaptureOutputDir = "",
    [switch]$CaptureNoExit,
    [double]$CaptureDelaySeconds = -1,
    [switch]$CapturePack,
    [string]$CapturePackOutput = "",
    [string]$CapturePythonExe = "",
    [switch]$CapturePackDeleteInput,
    [int]$CapturePackSamplesPerShard = 0,
    [switch]$CapturePackNoOverwrite,
    [string]$RenderMode = "",
    [string]$ModelServerUrl = "",
    [double]$ModelRequestFps = 0,
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ProjectFile = Join-Path $ProjectRoot "MiniWorldSim.uproject"

. (Join-Path $PSScriptRoot "Find-Unreal.ps1")

$ResolvedEngineDir = Resolve-UnrealEngineDir -EngineDir $EngineDir
$EditorExe = Join-Path $ResolvedEngineDir "Engine\Binaries\Win64\UnrealEditor.exe"

$Args = @(
    "`"$ProjectFile`"",
    "-game",
    "-windowed",
    "-ResX=$ResX",
    "-ResY=$ResY",
    "-log"
)

if (-not [string]::IsNullOrWhiteSpace($Map)) {
    $Args += "-MiniWorldMap=$Map"
}

if (-not [string]::IsNullOrWhiteSpace($CaptureSpec)) {
    $Args += "-MiniWorldCaptureSpec=$CaptureSpec"
}

if (-not [string]::IsNullOrWhiteSpace($CaptureSpecDir)) {
    $Args += "-MiniWorldCaptureSpecDir=$CaptureSpecDir"
}

if (-not [string]::IsNullOrWhiteSpace($CaptureOutputDir)) {
    $Args += "-MiniWorldCaptureOutputDir=$CaptureOutputDir"
}

if ($CaptureNoExit) {
    $Args += "-MiniWorldCaptureNoExit"
}

if ($CaptureDelaySeconds -ge 0) {
    $Args += "-MiniWorldCaptureDelaySeconds=$CaptureDelaySeconds"
}

if ($CapturePack) {
    $Args += "-MiniWorldCapturePack"
}

if (-not [string]::IsNullOrWhiteSpace($CapturePackOutput)) {
    $Args += "-MiniWorldCapturePackOutput=$CapturePackOutput"
}

if (-not [string]::IsNullOrWhiteSpace($CapturePythonExe)) {
    $Args += "-MiniWorldCapturePythonExe=$CapturePythonExe"
}

if ($CapturePackDeleteInput) {
    $Args += "-MiniWorldCapturePackDeleteInput"
}

if ($CapturePackSamplesPerShard -gt 0) {
    $Args += "-MiniWorldCapturePackSamplesPerShard=$CapturePackSamplesPerShard"
}

if ($CapturePackNoOverwrite) {
    $Args += "-MiniWorldCapturePackNoOverwrite"
}

if (-not [string]::IsNullOrWhiteSpace($RenderMode)) {
    $Args += "-MiniWorldRenderMode=$RenderMode"
}

if (-not [string]::IsNullOrWhiteSpace($ModelServerUrl)) {
    $Args += "-MiniWorldModelServerUrl=$ModelServerUrl"
}

if ($ModelRequestFps -gt 0) {
    $Args += "-MiniWorldModelRequestFps=$ModelRequestFps"
}

if (-not [string]::IsNullOrWhiteSpace($VisibilityMode)) {
    $Args += "-MiniWorldVisibilityMode=$VisibilityMode"
}

$Args += "-MiniWorldVisibilityHorizonScale=$VisibilityHorizonScale"

$Args += $ExtraArgs

Start-Process -FilePath $EditorExe -ArgumentList $Args -WorkingDirectory $ProjectRoot
