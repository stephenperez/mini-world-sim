function Test-UnrealEngineDir {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $false
    }

    $editor = Join-Path $Path "Engine\Binaries\Win64\UnrealEditor.exe"
    $uat = Join-Path $Path "Engine\Build\BatchFiles\RunUAT.bat"
    return (Test-Path $editor) -and (Test-Path $uat)
}

function Resolve-UnrealEngineDir {
    param([string]$EngineDir)

    $candidates = New-Object System.Collections.Generic.List[string]

    foreach ($candidate in @($EngineDir, $env:UNREAL_ENGINE_DIR, $env:UE_ENGINE_DIR)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $candidates.Add($candidate)
        }
    }

    foreach ($root in @(
        "C:\Program Files\Epic Games",
        "D:\Epic Games",
        "D:\Program Files\Epic Games",
        "D:\unreal\Epic Games"
    )) {
        if (Test-Path $root) {
            Get-ChildItem -Path $root -Directory -Filter "UE_*" -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object { $candidates.Add($_.FullName) }
        }
    }

    foreach ($registryPath in @(
        "HKCU:\Software\Epic Games\Unreal Engine\Builds",
        "HKLM:\SOFTWARE\EpicGames\Unreal Engine"
    )) {
        if (Test-Path $registryPath) {
            $item = Get-ItemProperty -Path $registryPath -ErrorAction SilentlyContinue
            foreach ($property in $item.PSObject.Properties) {
                if ($property.Value -is [string]) {
                    $candidates.Add($property.Value)
                }
            }
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-UnrealEngineDir $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Unreal Engine was not found. Install Unreal Engine 5.x or pass -EngineDir / set UNREAL_ENGINE_DIR to the folder containing Engine\Binaries\Win64\UnrealEditor.exe."
}
