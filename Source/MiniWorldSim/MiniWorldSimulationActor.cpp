#include "MiniWorldSimulationActor.h"

#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/Scene.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpResponse.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "RenderingThread.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"

namespace
{
float ReadFloatArg(const TCHAR* Name, float DefaultValue)
{
    float Value = DefaultValue;
    FParse::Value(FCommandLine::Get(), Name, Value);
    return Value;
}

int32 ReadIntArg(const TCHAR* Name, int32 DefaultValue)
{
    int32 Value = DefaultValue;
    FParse::Value(FCommandLine::Get(), Name, Value);
    return Value;
}

EMiniWorldVisibilityMode ReadVisibilityModeArg(EMiniWorldVisibilityMode DefaultValue)
{
    FString ModeText;
    if (!FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldVisibilityMode="), ModeText))
    {
        return DefaultValue;
    }

    ModeText = ModeText.TrimStartAndEnd().ToLower();
    if (ModeText == TEXT("fog"))
    {
        return EMiniWorldVisibilityMode::Fog;
    }

    if (ModeText == TEXT("night"))
    {
        return EMiniWorldVisibilityMode::Night;
    }

    UE_LOG(LogTemp, Warning, TEXT("Unknown -MiniWorldVisibilityMode value '%s'; using default."), *ModeText);
    return DefaultValue;
}

bool ReadModelRenderModeArg()
{
    FString ModeText;
    if (FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldRenderMode="), ModeText))
    {
        ModeText = ModeText.TrimStartAndEnd().ToLower();
        return ModeText == TEXT("model") || ModeText == TEXT("neural") || ModeText == TEXT("ml");
    }

    return FParse::Param(FCommandLine::Get(), TEXT("MiniWorldModelRender")) ||
        FParse::Param(FCommandLine::Get(), TEXT("MiniWorldUseModel"));
}

void SetConsoleVariableInt(const TCHAR* Name, int32 Value)
{
    if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
    {
        Variable->Set(Value, ECVF_SetByCode);
    }
}

template <typename NumericType>
bool ReadJsonNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, NumericType& OutValue)
{
    if (!Object.IsValid())
    {
        return false;
    }

    double Number = 0.0;
    if (!Object->TryGetNumberField(Name, Number))
    {
        return false;
    }

    OutValue = static_cast<NumericType>(Number);
    return true;
}

bool ReadJsonBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, bool& OutValue)
{
    if (!Object.IsValid())
    {
        return false;
    }

    bool Value = false;
    if (!Object->TryGetBoolField(Name, Value))
    {
        return false;
    }

    OutValue = Value;
    return true;
}

bool ReadJsonString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FString& OutValue)
{
    if (!Object.IsValid())
    {
        return false;
    }

    FString Value;
    if (!Object->TryGetStringField(Name, Value))
    {
        return false;
    }

    OutValue = Value;
    return true;
}

bool TryParseBoolText(const FString& Text, bool& OutValue)
{
    const FString Normalized = Text.TrimStartAndEnd().ToLower();
    if (Normalized == TEXT("true") || Normalized == TEXT("1") || Normalized == TEXT("yes") || Normalized == TEXT("on"))
    {
        OutValue = true;
        return true;
    }

    if (Normalized == TEXT("false") || Normalized == TEXT("0") || Normalized == TEXT("no") || Normalized == TEXT("off"))
    {
        OutValue = false;
        return true;
    }

    return false;
}

FLinearColor ReadJsonColor(const TSharedPtr<FJsonObject>& Object, const FLinearColor& DefaultValue)
{
    if (!Object.IsValid())
    {
        return DefaultValue;
    }

    FString ColorText;
    if (Object->TryGetStringField(TEXT("color"), ColorText))
    {
        if (ColorText.StartsWith(TEXT("#")))
        {
            ColorText.RightChopInline(1);
        }

        if (ColorText.Len() == 6 || ColorText.Len() == 8)
        {
            return FLinearColor(FColor::FromHex(ColorText));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
    if (Object->TryGetArrayField(TEXT("color"), ColorArray) && ColorArray->Num() >= 3)
    {
        const float R = static_cast<float>((*ColorArray)[0]->AsNumber());
        const float G = static_cast<float>((*ColorArray)[1]->AsNumber());
        const float B = static_cast<float>((*ColorArray)[2]->AsNumber());
        return FLinearColor(R, G, B, 1.0f);
    }

    return DefaultValue;
}

FVector2D ForwardFromYaw(float YawDegrees)
{
    const float YawRadians = FMath::DegreesToRadians(YawDegrees);
    return FVector2D(FMath::Cos(YawRadians), FMath::Sin(YawRadians));
}

FVector2D RightFromYaw(float YawDegrees)
{
    const FVector2D Forward = ForwardFromYaw(YawDegrees);
    return FVector2D(-Forward.Y, Forward.X);
}

uint8 ColorComponentToByte(float Value)
{
    return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f));
}

TSharedRef<FJsonObject> PoseToJson(const FVector2D& Position, float YawDegrees)
{
    TSharedRef<FJsonObject> Pose = MakeShared<FJsonObject>();
    Pose->SetNumberField(TEXT("x"), Position.X);
    Pose->SetNumberField(TEXT("y"), Position.Y);
    Pose->SetNumberField(TEXT("yaw_degrees"), YawDegrees);
    return Pose;
}

FString QuoteProcessArgument(const FString& Argument)
{
    FString Escaped = Argument;
    Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
    return FString::Printf(TEXT("\"%s\""), *Escaped);
}

FMiniWorldCaptureAction ReadCaptureActionObject(const TSharedPtr<FJsonObject>& ActionObject)
{
    FMiniWorldCaptureAction Action;
    if (!ActionObject.IsValid())
    {
        return Action;
    }

    ReadJsonString(ActionObject, TEXT("id"), Action.Label);
    if (Action.Label.IsEmpty())
    {
        ReadJsonString(ActionObject, TEXT("label"), Action.Label);
    }
    ReadJsonNumber(ActionObject, TEXT("forward"), Action.Forward);
    ReadJsonNumber(ActionObject, TEXT("strafe"), Action.Strafe);
    ReadJsonNumber(ActionObject, TEXT("turn"), Action.Turn);
    ReadJsonNumber(ActionObject, TEXT("duration_seconds"), Action.DurationSeconds);

    bool bKeyDown = false;
    if (ReadJsonBool(ActionObject, TEXT("w"), bKeyDown) && bKeyDown)
    {
        Action.Forward += 1.0f;
    }
    if (ReadJsonBool(ActionObject, TEXT("s"), bKeyDown) && bKeyDown)
    {
        Action.Forward -= 1.0f;
    }
    if (ReadJsonBool(ActionObject, TEXT("a"), bKeyDown) && bKeyDown)
    {
        Action.Turn -= 1.0f;
    }
    if (ReadJsonBool(ActionObject, TEXT("d"), bKeyDown) && bKeyDown)
    {
        Action.Turn += 1.0f;
    }

    FString KeysText;
    if (ReadJsonString(ActionObject, TEXT("keys"), KeysText))
    {
        KeysText = KeysText.ToLower();
        if (KeysText.Contains(TEXT("w")))
        {
            Action.Forward += 1.0f;
        }
        if (KeysText.Contains(TEXT("s")))
        {
            Action.Forward -= 1.0f;
        }
        if (KeysText.Contains(TEXT("a")))
        {
            Action.Turn -= 1.0f;
        }
        if (KeysText.Contains(TEXT("d")))
        {
            Action.Turn += 1.0f;
        }
    }

    Action.Forward = FMath::Clamp(Action.Forward, -1.0f, 1.0f);
    Action.Strafe = FMath::Clamp(Action.Strafe, -1.0f, 1.0f);
    Action.Turn = FMath::Clamp(Action.Turn, -1.0f, 1.0f);
    Action.DurationSeconds = FMath::Clamp(Action.DurationSeconds, 0.0f, 1.0f);
    return Action;
}

float DistanceSquaredToSegment(const FVector2D& Point, const FVector2D& Start, const FVector2D& End)
{
    const FVector2D Segment = End - Start;
    const float LengthSquared = Segment.SizeSquared();
    if (LengthSquared <= KINDA_SMALL_NUMBER)
    {
        return FVector2D::DistSquared(Point, Start);
    }

    const float T = FMath::Clamp(FVector2D::DotProduct(Point - Start, Segment) / LengthSquared, 0.0f, 1.0f);
    const FVector2D Closest = Start + Segment * T;
    return FVector2D::DistSquared(Point, Closest);
}

float SmoothStep(float Value)
{
    const float T = FMath::Clamp(Value, 0.0f, 1.0f);
    return T * T * (3.0f - 2.0f * T);
}
}

AMiniWorldSimulationActor::AMiniWorldSimulationActor()
{
    PrimaryActorTick.bCanEverTick = true;
}

void AMiniWorldSimulationActor::BeginPlay()
{
    Super::BeginPlay();

    bCaptureModeActive = ReadCaptureSpecCommandLine(ActiveCaptureSpec);
    if (bCaptureModeActive && ActiveCaptureSpecs.IsEmpty())
    {
        ActiveCaptureSpecs.Add(ActiveCaptureSpec);
    }
    if (bCaptureModeActive && !ActiveCaptureSpecs.IsEmpty())
    {
        ActiveCaptureSpec = ActiveCaptureSpecs[0];
    }

    MoveSpeed = ReadFloatArg(TEXT("-MiniWorldMoveSpeed="), MoveSpeed);
    TurnRateDegrees = ReadFloatArg(TEXT("-MiniWorldTurnRate="), TurnRateDegrees);
    PlayerRadius = ReadFloatArg(TEXT("-MiniWorldPlayerRadius="), PlayerRadius);
    EyeHeight = ReadFloatArg(TEXT("-MiniWorldEyeHeight="), EyeHeight);
    CameraFov = ReadFloatArg(TEXT("-MiniWorldFOV="), CameraFov);
    FrameWidth = FMath::Clamp(ReadIntArg(TEXT("-MiniWorldFrameWidth="), FrameWidth), 32, 2048);
    FrameHeight = FMath::Clamp(ReadIntArg(TEXT("-MiniWorldFrameHeight="), FrameHeight), 32, 2048);
    VisibilityMode = ReadVisibilityModeArg(VisibilityMode);
    bModelRenderModeActive = ReadModelRenderModeArg();
    FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldModelServerUrl="), ModelServerUrl);
    ModelRequestFps = FMath::Clamp(ReadFloatArg(TEXT("-MiniWorldModelRequestFps="), ModelRequestFps), 1.0f, 120.0f);
    ModelMinimapSize = FMath::Clamp(ReadIntArg(TEXT("-MiniWorldModelMinimapSize="), ModelMinimapSize), 32, 1024);
    VisibilityHorizonScale = FMath::Clamp(
        ReadFloatArg(TEXT("-MiniWorldVisibilityHorizonScale="), VisibilityHorizonScale),
        0.5f,
        1.0f
    );
    SetConsoleVariableInt(TEXT("r.Fog"), 1);
    SetConsoleVariableInt(TEXT("r.VolumetricFog"), 0);
    SetConsoleVariableInt(TEXT("r.MotionBlurQuality"), 0);
    SetConsoleVariableInt(TEXT("r.DepthOfFieldQuality"), 0);
    SetConsoleVariableInt(TEXT("r.Tonemapper.GrainQuantization"), 0);

    LoadOrCreateWorldDefinition();
    PlayerPosition = WorldDefinition.SpawnPosition;
    PlayerYawDegrees = WorldDefinition.SpawnYawDegrees;

    if (bCaptureModeActive && !ActiveCaptureSpecs.IsEmpty())
    {
        ResetPlayerForCaptureSpec(ActiveCaptureSpecs[0]);
    }
    else
    {
        EnsureClearSpawn();
    }

    SpawnScene();
    UpdateCapture();
    if (bModelRenderModeActive)
    {
        UE_LOG(LogTemp, Display, TEXT("Mini-world model render mode enabled: %s"), *ModelServerUrl);
        MaybeRequestModelFrame();
    }

    if (bCaptureModeActive)
    {
        const float CaptureDelaySeconds = FMath::Max(ActiveCaptureSpec.StartDelaySeconds, 0.0f);
        if (CaptureDelaySeconds <= KINDA_SMALL_NUMBER)
        {
            GetWorldTimerManager().SetTimerForNextTick(this, &AMiniWorldSimulationActor::RunQueuedCaptureSpec);
        }
        else
        {
            FTimerHandle CaptureDelayTimerHandle;
            GetWorldTimerManager().SetTimer(
                CaptureDelayTimerHandle,
                this,
                &AMiniWorldSimulationActor::RunQueuedCaptureSpec,
                CaptureDelaySeconds,
                false
            );
            UE_LOG(LogTemp, Display, TEXT("Mini-world capture waiting %.2f second(s) before sampling."), CaptureDelaySeconds);
        }
    }
}

void AMiniWorldSimulationActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (bCaptureModeActive)
    {
        UpdateWallAppearance();
        UpdateFogCurtains();
        UpdateCapture();
        return;
    }

    UpdatePlayer(FMath::Clamp(DeltaSeconds, 0.0f, 0.1f));
    UpdateWallAppearance();
    UpdateFogCurtains();
    if (bModelRenderModeActive)
    {
        MaybeRequestModelFrame();
        if (!ModelFrameTexture)
        {
            UpdateCapture();
        }
        return;
    }

    UpdateCapture();
}

UTexture* AMiniWorldSimulationActor::GetDisplayViewTexture() const
{
    if (bModelRenderModeActive && ModelFrameTexture)
    {
        return ModelFrameTexture;
    }

    return ViewRenderTarget;
}

bool AMiniWorldSimulationActor::ReadCaptureSpecCommandLine(FMiniWorldCaptureSpec& OutSpec)
{
    FString SpecPath;
    FString SpecDirectory;
    const bool bHasSpec = FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCaptureSpec="), SpecPath);
    const bool bHasSpecDirectory = FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCaptureSpecDir="), SpecDirectory);

    if (!bHasSpec && !bHasSpecDirectory)
    {
        return false;
    }

    if (bHasSpec && bHasSpecDirectory)
    {
        UE_LOG(LogTemp, Error, TEXT("Use either -MiniWorldCaptureSpec or -MiniWorldCaptureSpecDir, not both."));
        FPlatformMisc::RequestExit(false);
        return false;
    }

    ActiveCaptureSpecs.Reset();

    if (bHasSpec)
    {
        if (SpecPath.IsEmpty())
        {
            UE_LOG(LogTemp, Error, TEXT("-MiniWorldCaptureSpec was provided with an empty path."));
            FPlatformMisc::RequestExit(false);
            return false;
        }

        if (FPaths::IsRelative(SpecPath))
        {
            const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
            SpecPath = FPaths::ConvertRelativePathToFull(ProjectDir, SpecPath);
        }

        if (!LoadCaptureSpec(SpecPath, OutSpec))
        {
            UE_LOG(LogTemp, Error, TEXT("Mini-world capture spec could not be loaded: %s"), *SpecPath);
            FPlatformMisc::RequestExit(false);
            return false;
        }

        ActiveCaptureSpecs.Add(OutSpec);
    }
    else
    {
        if (SpecDirectory.IsEmpty())
        {
            UE_LOG(LogTemp, Error, TEXT("-MiniWorldCaptureSpecDir was provided with an empty path."));
            FPlatformMisc::RequestExit(false);
            return false;
        }

        FString BatchOutputDir;
        if (!LoadCaptureSpecDirectory(SpecDirectory, ActiveCaptureSpecs, BatchOutputDir))
        {
            UE_LOG(LogTemp, Error, TEXT("Mini-world capture spec directory could not be loaded: %s"), *SpecDirectory);
            FPlatformMisc::RequestExit(false);
            return false;
        }

        ActiveCaptureOutputRoot = BatchOutputDir;
        OutSpec = ActiveCaptureSpecs[0];
    }

    FString ExitAfterCaptureText;
    if (FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCaptureExitAfterCapture="), ExitAfterCaptureText))
    {
        bool bParsedExitAfterCapture = true;
        if (TryParseBoolText(ExitAfterCaptureText, bParsedExitAfterCapture))
        {
            OutSpec.bExitAfterCapture = bParsedExitAfterCapture;
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Unknown -MiniWorldCaptureExitAfterCapture value '%s'; using capture spec value."), *ExitAfterCaptureText);
        }
    }

    if (FParse::Param(FCommandLine::Get(), TEXT("MiniWorldCaptureNoExit")) ||
        FParse::Param(FCommandLine::Get(), TEXT("MiniWorldCaptureStayOpen")))
    {
        OutSpec.bExitAfterCapture = false;
    }

    FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCaptureDelaySeconds="), OutSpec.StartDelaySeconds);
    OutSpec.StartDelaySeconds = FMath::Clamp(OutSpec.StartDelaySeconds, 0.0f, 30.0f);

    FString OutputDirOverride;
    if (FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCaptureOutputDir="), OutputDirOverride))
    {
        const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        FString ResolvedOutputDir = FPaths::IsRelative(OutputDirOverride)
            ? FPaths::ConvertRelativePathToFull(ProjectDir, OutputDirOverride)
            : FPaths::ConvertRelativePathToFull(OutputDirOverride);
        FPaths::NormalizeFilename(ResolvedOutputDir);
        ActiveCaptureOutputRoot = ResolvedOutputDir;

        for (FMiniWorldCaptureSpec& Spec : ActiveCaptureSpecs)
        {
            Spec.OutputDir = ResolvedOutputDir;
            Spec.bOutputDirIsBatchRoot = bHasSpecDirectory || Spec.Actions.Num() > 1;
        }
        OutSpec = ActiveCaptureSpecs[0];
    }

    for (FMiniWorldCaptureSpec& Spec : ActiveCaptureSpecs)
    {
        Spec.bExitAfterCapture = OutSpec.bExitAfterCapture;
        Spec.StartDelaySeconds = OutSpec.StartDelaySeconds;
    }

    FString PackText;
    bool bPackRequested = false;
    if (FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCapturePack="), PackText))
    {
        TryParseBoolText(PackText, bPackRequested);
    }
    bPackRequested = bPackRequested || FParse::Param(FCommandLine::Get(), TEXT("MiniWorldCapturePack"));
    bCapturePackAfterRun = bPackRequested;
    bCapturePackDeleteInput =
        FParse::Param(FCommandLine::Get(), TEXT("MiniWorldCapturePackDeleteInput")) ||
        FParse::Param(FCommandLine::Get(), TEXT("MiniWorldCaptureDeleteAfterPack"));
    bCapturePackOverwrite = !FParse::Param(FCommandLine::Get(), TEXT("MiniWorldCapturePackNoOverwrite"));
    FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCapturePackSamplesPerShard="), CapturePackSamplesPerShard);
    FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCapturePythonExe="), CapturePythonExe);
    FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCapturePackScript="), CapturePackScriptPath);
    FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldCapturePackOutput="), CapturePackOutputPath);

    return true;
}

bool AMiniWorldSimulationActor::LoadCaptureSpec(const FString& SpecPath, FMiniWorldCaptureSpec& OutSpec) const
{
    FString JsonText;
    if (!FFileHelper::LoadFileToString(JsonText, *SpecPath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Could not parse capture spec JSON: %s"), *SpecPath);
        return false;
    }

    FMiniWorldCaptureSpec Parsed;
    Parsed.SpecPath = FPaths::ConvertRelativePathToFull(SpecPath);
    FPaths::NormalizeFilename(Parsed.SpecPath);
    Parsed.SpecDirectory = FPaths::GetPath(Parsed.SpecPath);

    ReadJsonString(Root, TEXT("sample_id"), Parsed.SampleId);
    if (Parsed.SampleId.IsEmpty())
    {
        Parsed.SampleId = FPaths::GetBaseFilename(Parsed.SpecPath);
    }

    const TSharedPtr<FJsonObject>* WorldObject = nullptr;
    if (Root->TryGetObjectField(TEXT("world"), WorldObject) && WorldObject && WorldObject->IsValid())
    {
        FString MapPath;
        if (ReadJsonString(*WorldObject, TEXT("map_file"), MapPath) || ReadJsonString(*WorldObject, TEXT("map_path"), MapPath))
        {
            Parsed.MapPath = ResolveCapturePath(MapPath, Parsed.SpecDirectory, false);
        }
    }

    const TSharedPtr<FJsonObject>* PoseObject = nullptr;
    if (Root->TryGetObjectField(TEXT("initial_pose"), PoseObject) && PoseObject && PoseObject->IsValid())
    {
        Parsed.bHasInitialPose = true;
        ReadJsonNumber(*PoseObject, TEXT("x"), Parsed.InitialPose.Position.X);
        ReadJsonNumber(*PoseObject, TEXT("y"), Parsed.InitialPose.Position.Y);
        if (!ReadJsonNumber(*PoseObject, TEXT("yaw_degrees"), Parsed.InitialPose.YawDegrees))
        {
            ReadJsonNumber(*PoseObject, TEXT("yaw"), Parsed.InitialPose.YawDegrees);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* ActionArray = nullptr;
    if (Root->TryGetArrayField(TEXT("actions"), ActionArray))
    {
        for (const TSharedPtr<FJsonValue>& ActionValue : *ActionArray)
        {
            const TSharedPtr<FJsonObject> ActionObject = ActionValue.IsValid() ? ActionValue->AsObject() : nullptr;
            if (!ActionObject.IsValid())
            {
                continue;
            }

            int32 RepeatCount = 1;
            ReadJsonNumber(ActionObject, TEXT("repeat"), RepeatCount);
            RepeatCount = FMath::Clamp(RepeatCount, 1, 10000);

            const FMiniWorldCaptureAction Action = ReadCaptureActionObject(ActionObject);
            for (int32 RepeatIndex = 0; RepeatIndex < RepeatCount; ++RepeatIndex)
            {
                Parsed.Actions.Add(Action);
            }
        }
    }

    if (Parsed.Actions.IsEmpty())
    {
        const TSharedPtr<FJsonObject>* ActionObject = nullptr;
        if (Root->TryGetObjectField(TEXT("action"), ActionObject) && ActionObject && ActionObject->IsValid())
        {
            Parsed.Actions.Add(ReadCaptureActionObject(*ActionObject));
        }
    }

    if (Parsed.Actions.IsEmpty())
    {
        Parsed.Actions.Add(FMiniWorldCaptureAction());
    }

    const TSharedPtr<FJsonObject>* CaptureObject = nullptr;
    if (Root->TryGetObjectField(TEXT("capture"), CaptureObject) && CaptureObject && CaptureObject->IsValid())
    {
        FString OutputDir;
        if (ReadJsonString(*CaptureObject, TEXT("output_dir"), OutputDir))
        {
            Parsed.OutputDir = ResolveCapturePath(OutputDir, Parsed.SpecDirectory, true);
        }

        ReadJsonBool(*CaptureObject, TEXT("include_rgb"), Parsed.bIncludeRgb);
        ReadJsonBool(*CaptureObject, TEXT("include_minimap_rgb"), Parsed.bIncludeMinimapRgb);
        ReadJsonBool(*CaptureObject, TEXT("include_semantic_minimap"), Parsed.bIncludeSemanticMinimap);
        ReadJsonBool(*CaptureObject, TEXT("exit_after_capture"), Parsed.bExitAfterCapture);
        if (!ReadJsonNumber(*CaptureObject, TEXT("start_delay_seconds"), Parsed.StartDelaySeconds))
        {
            ReadJsonNumber(*CaptureObject, TEXT("warmup_seconds"), Parsed.StartDelaySeconds);
        }
        ReadJsonNumber(*CaptureObject, TEXT("minimap_size"), Parsed.MinimapSize);
    }

    if (Parsed.OutputDir.IsEmpty())
    {
        const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        Parsed.OutputDir = FPaths::ConvertRelativePathToFull(ProjectDir, FString::Printf(TEXT("Saved/Captures/%s"), *Parsed.SampleId));
        FPaths::NormalizeFilename(Parsed.OutputDir);
    }

    Parsed.MinimapSize = FMath::Clamp(Parsed.MinimapSize, 32, 1024);
    Parsed.StartDelaySeconds = FMath::Clamp(Parsed.StartDelaySeconds, 0.0f, 30.0f);
    OutSpec = Parsed;
    return true;
}

bool AMiniWorldSimulationActor::LoadCaptureSpecDirectory(
    const FString& SpecDirectory,
    TArray<FMiniWorldCaptureSpec>& OutSpecs,
    FString& OutBatchOutputDir
) const
{
    FString ResolvedSpecDirectory = SpecDirectory;
    if (FPaths::IsRelative(ResolvedSpecDirectory))
    {
        const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        ResolvedSpecDirectory = FPaths::ConvertRelativePathToFull(ProjectDir, ResolvedSpecDirectory);
    }
    else
    {
        ResolvedSpecDirectory = FPaths::ConvertRelativePathToFull(ResolvedSpecDirectory);
    }
    FPaths::NormalizeFilename(ResolvedSpecDirectory);

    TArray<FString> SpecFiles;
    IFileManager::Get().FindFiles(SpecFiles, *(ResolvedSpecDirectory / TEXT("*.json")), true, false);
    SpecFiles.Sort();
    if (SpecFiles.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("No JSON capture specs found in directory: %s"), *ResolvedSpecDirectory);
        return false;
    }

    const FString BatchName = FPaths::GetCleanFilename(ResolvedSpecDirectory);
    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    OutBatchOutputDir = FPaths::ConvertRelativePathToFull(ProjectDir, FString::Printf(TEXT("Saved/Captures/%s"), *BatchName));
    FPaths::NormalizeFilename(OutBatchOutputDir);

    OutSpecs.Reset();
    for (const FString& SpecFile : SpecFiles)
    {
        FMiniWorldCaptureSpec Spec;
        const FString FullSpecPath = ResolvedSpecDirectory / SpecFile;
        if (!LoadCaptureSpec(FullSpecPath, Spec))
        {
            return false;
        }

        Spec.OutputDir = OutBatchOutputDir;
        Spec.bOutputDirIsBatchRoot = true;
        OutSpecs.Add(Spec);
    }

    return !OutSpecs.IsEmpty();
}

FString AMiniWorldSimulationActor::ResolveCapturePath(const FString& Path, const FString& BaseDirectory, bool bPreferProjectRelative) const
{
    if (Path.IsEmpty())
    {
        return Path;
    }

    if (!FPaths::IsRelative(Path))
    {
        FString FullPath = FPaths::ConvertRelativePathToFull(Path);
        FPaths::NormalizeFilename(FullPath);
        return FullPath;
    }

    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FString ProjectRelativePath = FPaths::ConvertRelativePathToFull(ProjectDir, Path);
    FPaths::NormalizeFilename(ProjectRelativePath);
    if (bPreferProjectRelative || FPaths::FileExists(ProjectRelativePath) || FPaths::DirectoryExists(ProjectRelativePath))
    {
        return ProjectRelativePath;
    }

    FString BaseRelativePath = FPaths::ConvertRelativePathToFull(BaseDirectory, Path);
    FPaths::NormalizeFilename(BaseRelativePath);
    return BaseRelativePath;
}

void AMiniWorldSimulationActor::RunQueuedCaptureSpec()
{
    bool bSucceeded = RunActiveCaptureSpecs();
    if (bSucceeded && bCapturePackAfterRun)
    {
        bSucceeded = RunCapturePacker();
    }

    if (bSucceeded)
    {
        UE_LOG(LogTemp, Display, TEXT("Mini-world capture completed."));
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Mini-world capture failed."));
    }

    if (ActiveCaptureSpec.bExitAfterCapture)
    {
        FPlatformMisc::RequestExit(false);
        return;
    }

    bCaptureModeActive = false;
    SetActorTickEnabled(true);
    UE_LOG(LogTemp, Display, TEXT("Mini-world capture window kept open."));
}

bool AMiniWorldSimulationActor::RunActiveCaptureSpecs()
{
    bool bSucceeded = true;
    for (const FMiniWorldCaptureSpec& Spec : ActiveCaptureSpecs)
    {
        ResetPlayerForCaptureSpec(Spec);
        bSucceeded &= RunCaptureSpec(Spec);
    }

    return bSucceeded;
}

bool AMiniWorldSimulationActor::RunCaptureSpec(const FMiniWorldCaptureSpec& Spec)
{
    const int32 SequenceCount = FMath::Max(Spec.Actions.Num(), 1);
    if (Spec.OutputDir.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("Capture spec '%s' has no output directory."), *Spec.SpecPath);
        return false;
    }

    bool bSucceeded = true;
    for (int32 SequenceIndex = 0; SequenceIndex < SequenceCount; ++SequenceIndex)
    {
        const FMiniWorldCaptureAction& Action = Spec.Actions.IsValidIndex(SequenceIndex)
            ? Spec.Actions[SequenceIndex]
            : Spec.Actions[0];
        const FString OutputDir = BuildSampleOutputDir(Spec, SequenceIndex, SequenceCount);
        const FString EffectiveSampleId = BuildEffectiveSampleId(Spec, SequenceIndex, SequenceCount);

        if (!IFileManager::Get().MakeDirectory(*OutputDir, true))
        {
            UE_LOG(LogTemp, Error, TEXT("Could not create capture output directory: %s"), *OutputDir);
            bSucceeded = false;
            continue;
        }

        bCollidedLastFrame = false;
        bSucceeded &= CaptureFrameArtifacts(Spec, OutputDir, TEXT("t"), false);

        const FMiniWorldCaptureStepResult StepResult = ApplyCaptureAction(Action);
        bSucceeded &= CaptureFrameArtifacts(Spec, OutputDir, TEXT("t1"), StepResult.bCollided);
        bSucceeded &= SaveCaptureMetadata(Spec, Action, StepResult, OutputDir, EffectiveSampleId, SequenceIndex, SequenceCount);
    }

    return bSucceeded;
}

bool AMiniWorldSimulationActor::CaptureFrameArtifacts(
    const FMiniWorldCaptureSpec& Spec,
    const FString& OutputDir,
    const FString& Suffix,
    bool bCollisionIndicator
)
{
    UpdateWallAppearance();
    UpdateFogCurtains();
    UpdateCapture();
    FlushRenderingCommands();

    bool bSucceeded = true;
    if (Spec.bIncludeRgb)
    {
        bSucceeded &= SaveViewRenderTargetPng(OutputDir / FString::Printf(TEXT("rgb_%s.png"), *Suffix));
    }

    if (Spec.bIncludeMinimapRgb || Spec.bIncludeSemanticMinimap)
    {
        bSucceeded &= SaveMinimapArtifacts(Spec, OutputDir, Suffix, bCollisionIndicator);
    }

    return bSucceeded;
}

bool AMiniWorldSimulationActor::SaveViewRenderTargetPng(const FString& FilePath)
{
    if (!ViewRenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("Cannot save view render target; render target is null."));
        return false;
    }

    FImage Image;
    if (!FImageUtils::GetRenderTargetImage(ViewRenderTarget, Image))
    {
        UE_LOG(LogTemp, Error, TEXT("Could not read view render target for capture."));
        return false;
    }

    if (!FImageUtils::SaveImageByExtension(*FilePath, Image))
    {
        UE_LOG(LogTemp, Error, TEXT("Could not save view PNG: %s"), *FilePath);
        return false;
    }

    return true;
}

bool AMiniWorldSimulationActor::SaveMinimapArtifacts(
    const FMiniWorldCaptureSpec& Spec,
    const FString& OutputDir,
    const FString& Suffix,
    bool bCollisionIndicator
)
{
    bool bSucceeded = true;

    if (Spec.bIncludeMinimapRgb)
    {
        TArray<FColor> Pixels;
        BuildMinimapPixels(Spec.MinimapSize, bCollisionIndicator, Pixels);
        const FString FilePath = OutputDir / FString::Printf(TEXT("minimap_rgb_%s.png"), *Suffix);
        const FImageView ImageView(Pixels.GetData(), Spec.MinimapSize, Spec.MinimapSize, EGammaSpace::sRGB);
        if (!FImageUtils::SaveImageByExtension(*FilePath, ImageView))
        {
            UE_LOG(LogTemp, Error, TEXT("Could not save minimap PNG: %s"), *FilePath);
            bSucceeded = false;
        }
    }

    if (Spec.bIncludeSemanticMinimap)
    {
        bSucceeded &= SaveSemanticMinimap(
            OutputDir / FString::Printf(TEXT("minimap_semantic_%s.bin"), *Suffix),
            Spec.MinimapSize,
            bCollisionIndicator
        );
    }

    return bSucceeded;
}

bool AMiniWorldSimulationActor::SaveSemanticMinimap(const FString& FilePath, int32 MinimapSize, bool bCollisionIndicator)
{
    TArray<FColor> UnusedPixels;
    TArray<uint8> SemanticData;
    BuildMinimapPixels(MinimapSize, bCollisionIndicator, UnusedPixels, &SemanticData);

    if (!FFileHelper::SaveArrayToFile(SemanticData, *FilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("Could not save semantic minimap: %s"), *FilePath);
        return false;
    }

    return true;
}

void AMiniWorldSimulationActor::BuildMinimapPixels(
    int32 MinimapSize,
    bool bCollisionIndicator,
    TArray<FColor>& OutPixels,
    TArray<uint8>* OutSemanticData
) const
{
    constexpr int32 SemanticChannelCount = 6;
    const int32 PixelCount = MinimapSize * MinimapSize;
    OutPixels.SetNum(PixelCount);

    if (OutSemanticData)
    {
        OutSemanticData->SetNumZeroed(SemanticChannelCount * PixelCount);
    }

    const FColor PanelColor = FLinearColor(0.034f, 0.038f, 0.04f, 1.0f).ToFColor(true);
    const FColor MapBackgroundColor = FLinearColor(0.07f, 0.08f, 0.075f, 1.0f).ToFColor(true);
    const FColor BorderColor = FLinearColor(0.28f, 0.31f, 0.34f, 1.0f).ToFColor(true);
    const FColor PlayerColor = bCollisionIndicator
        ? FLinearColor(1.0f, 0.18f, 0.14f, 1.0f).ToFColor(true)
        : FLinearColor(0.96f, 0.98f, 1.0f, 1.0f).ToFColor(true);

    const FVector2D Center(MinimapSize * 0.5f, MinimapSize * 0.5f);
    const float CircleRadius = FMath::Max(MinimapSize * 0.5f - 1.0f, 1.0f);
    const float CircleRadiusSquared = FMath::Square(CircleRadius);
    const float PixelsPerUnit = CircleRadius / FMath::Max(WorldDefinition.ViewDistance, 1.0f);
    const FVector2D Forward = ForwardFromYaw(PlayerYawDegrees);
    const FVector2D Right = RightFromYaw(PlayerYawDegrees);

    const auto SetSemantic = [OutSemanticData, PixelCount](int32 Channel, int32 PixelIndex, uint8 Value)
    {
        if (OutSemanticData)
        {
            (*OutSemanticData)[Channel * PixelCount + PixelIndex] = Value;
        }
    };

    for (int32 Y = 0; Y < MinimapSize; ++Y)
    {
        for (int32 X = 0; X < MinimapSize; ++X)
        {
            const int32 PixelIndex = Y * MinimapSize + X;
            const FVector2D PixelPoint(X + 0.5f, Y + 0.5f);
            const FVector2D LocalPixel = PixelPoint - Center;
            const bool bInsideCircle = LocalPixel.SizeSquared() <= CircleRadiusSquared;

            OutPixels[PixelIndex] = bInsideCircle ? MapBackgroundColor : PanelColor;
            if (!bInsideCircle)
            {
                continue;
            }

            SetSemantic(0, PixelIndex, 255);
            SetSemantic(5, PixelIndex, bCollisionIndicator ? 255 : 0);

            const float LocalRight = LocalPixel.X / PixelsPerUnit;
            const float LocalForward = -LocalPixel.Y / PixelsPerUnit;
            const FVector2D WorldPoint = PlayerPosition + Forward * LocalForward + Right * LocalRight;

            for (const FMiniWorldLine& Line : WorldDefinition.Lines)
            {
                const float HalfThickness = FMath::Max(Line.Thickness, 1.0f) * 0.5f;
                if (DistanceSquaredToSegment(WorldPoint, Line.Start, Line.End) > FMath::Square(HalfThickness))
                {
                    continue;
                }

                const FColor WallColor = Line.Color.ToFColor(true);
                OutPixels[PixelIndex] = WallColor;
                SetSemantic(1, PixelIndex, 255);
                SetSemantic(2, PixelIndex, WallColor.R);
                SetSemantic(3, PixelIndex, WallColor.G);
                SetSemantic(4, PixelIndex, WallColor.B);
            }
        }
    }

    const auto DrawScreenLine = [&OutPixels, MinimapSize](const FVector2D& A, const FVector2D& B, float Thickness, const FColor& Color)
    {
        const float HalfThicknessSquared = FMath::Square(FMath::Max(Thickness, 1.0f) * 0.5f);
        for (int32 Y = 0; Y < MinimapSize; ++Y)
        {
            for (int32 X = 0; X < MinimapSize; ++X)
            {
                const FVector2D PixelPoint(X + 0.5f, Y + 0.5f);
                if (DistanceSquaredToSegment(PixelPoint, A, B) <= HalfThicknessSquared)
                {
                    OutPixels[Y * MinimapSize + X] = Color;
                }
            }
        }
    };

    const float ArrowLength = FMath::Max(12.0f, MinimapSize * 0.105f);
    const float ArrowWing = ArrowLength * 0.55f;
    const FVector2D Nose(Center.X, Center.Y - ArrowLength);
    const FVector2D Left(Center.X - ArrowWing, Center.Y + ArrowWing * 0.45f);
    const FVector2D RightWing(Center.X + ArrowWing, Center.Y + ArrowWing * 0.45f);
    DrawScreenLine(Nose, Left, 2.0f, PlayerColor);
    DrawScreenLine(Nose, RightWing, 2.0f, PlayerColor);
    DrawScreenLine(Left, RightWing, 2.0f, PlayerColor);

    for (int32 Y = 0; Y < MinimapSize; ++Y)
    {
        for (int32 X = 0; X < MinimapSize; ++X)
        {
            const int32 PixelIndex = Y * MinimapSize + X;
            const FVector2D PixelPoint(X + 0.5f, Y + 0.5f);
            const float DistanceFromCenter = (PixelPoint - Center).Size();
            if (FMath::Abs(DistanceFromCenter - CircleRadius) <= 0.85f || X == 0 || Y == 0 || X == MinimapSize - 1 || Y == MinimapSize - 1)
            {
                OutPixels[PixelIndex] = BorderColor;
            }
        }
    }
}

bool AMiniWorldSimulationActor::SaveCaptureMetadata(
    const FMiniWorldCaptureSpec& Spec,
    const FMiniWorldCaptureAction& CaptureAction,
    const FMiniWorldCaptureStepResult& StepResult,
    const FString& OutputDir,
    const FString& EffectiveSampleId,
    int32 SequenceIndex,
    int32 SequenceCount
)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("sample_id"), EffectiveSampleId);
    Root->SetStringField(TEXT("spec_sample_id"), Spec.SampleId);
    Root->SetStringField(TEXT("spec_path"), Spec.SpecPath);
    Root->SetStringField(TEXT("map_path"), Spec.MapPath);
    Root->SetStringField(TEXT("output_dir"), OutputDir);
    Root->SetNumberField(TEXT("sequence_index"), SequenceIndex);
    Root->SetNumberField(TEXT("sequence_count"), SequenceCount);
    Root->SetNumberField(TEXT("frame_width"), FrameWidth);
    Root->SetNumberField(TEXT("frame_height"), FrameHeight);
    Root->SetNumberField(TEXT("minimap_size"), Spec.MinimapSize);
    Root->SetNumberField(TEXT("view_distance"), WorldDefinition.ViewDistance);
    Root->SetNumberField(TEXT("move_speed"), MoveSpeed);
    Root->SetNumberField(TEXT("turn_rate_degrees"), TurnRateDegrees);
    Root->SetNumberField(TEXT("player_radius"), PlayerRadius);
    Root->SetStringField(TEXT("visibility_mode"), VisibilityMode == EMiniWorldVisibilityMode::Night ? TEXT("night") : TEXT("fog"));
    Root->SetNumberField(TEXT("visibility_horizon_scale"), VisibilityHorizonScale);
    Root->SetBoolField(TEXT("exit_after_capture"), Spec.bExitAfterCapture);
    Root->SetNumberField(TEXT("start_delay_seconds"), Spec.StartDelaySeconds);

    TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
    Action->SetStringField(TEXT("label"), CaptureAction.Label);
    Action->SetNumberField(TEXT("forward"), CaptureAction.Forward);
    Action->SetNumberField(TEXT("strafe"), CaptureAction.Strafe);
    Action->SetNumberField(TEXT("turn"), CaptureAction.Turn);
    Action->SetNumberField(TEXT("duration_seconds"), CaptureAction.DurationSeconds);
    Root->SetObjectField(TEXT("action"), Action);

    Root->SetObjectField(TEXT("pose_t"), PoseToJson(StepResult.StartPosition, StepResult.StartYawDegrees));
    Root->SetObjectField(TEXT("pose_t1"), PoseToJson(StepResult.EndPosition, StepResult.EndYawDegrees));

    TSharedRef<FJsonObject> Motion = MakeShared<FJsonObject>();
    Motion->SetNumberField(TEXT("delta_x"), StepResult.EndPosition.X - StepResult.StartPosition.X);
    Motion->SetNumberField(TEXT("delta_y"), StepResult.EndPosition.Y - StepResult.StartPosition.Y);
    Motion->SetNumberField(TEXT("delta_yaw_degrees"), FRotator::NormalizeAxis(StepResult.EndYawDegrees - StepResult.StartYawDegrees));
    Motion->SetBoolField(TEXT("collision"), StepResult.bCollided);
    Root->SetObjectField(TEXT("actual_motion"), Motion);

    TSharedRef<FJsonObject> Artifacts = MakeShared<FJsonObject>();
    if (Spec.bIncludeRgb)
    {
        Artifacts->SetStringField(TEXT("rgb_t"), TEXT("rgb_t.png"));
        Artifacts->SetStringField(TEXT("rgb_t1"), TEXT("rgb_t1.png"));
    }
    if (Spec.bIncludeMinimapRgb)
    {
        Artifacts->SetStringField(TEXT("minimap_rgb_t"), TEXT("minimap_rgb_t.png"));
        Artifacts->SetStringField(TEXT("minimap_rgb_t1"), TEXT("minimap_rgb_t1.png"));
    }
    if (Spec.bIncludeSemanticMinimap)
    {
        Artifacts->SetStringField(TEXT("minimap_semantic_t"), TEXT("minimap_semantic_t.bin"));
        Artifacts->SetStringField(TEXT("minimap_semantic_t1"), TEXT("minimap_semantic_t1.bin"));
    }
    Root->SetObjectField(TEXT("artifacts"), Artifacts);

    TSharedRef<FJsonObject> Semantic = MakeShared<FJsonObject>();
    Semantic->SetStringField(TEXT("dtype"), TEXT("uint8"));
    Semantic->SetStringField(TEXT("layout"), TEXT("CHW"));
    TArray<TSharedPtr<FJsonValue>> Shape;
    Shape.Add(MakeShared<FJsonValueNumber>(6));
    Shape.Add(MakeShared<FJsonValueNumber>(Spec.MinimapSize));
    Shape.Add(MakeShared<FJsonValueNumber>(Spec.MinimapSize));
    Semantic->SetArrayField(TEXT("shape"), Shape);
    TArray<TSharedPtr<FJsonValue>> Channels;
    Channels.Add(MakeShared<FJsonValueString>(TEXT("visible_mask")));
    Channels.Add(MakeShared<FJsonValueString>(TEXT("wall_occupancy")));
    Channels.Add(MakeShared<FJsonValueString>(TEXT("wall_color_r")));
    Channels.Add(MakeShared<FJsonValueString>(TEXT("wall_color_g")));
    Channels.Add(MakeShared<FJsonValueString>(TEXT("wall_color_b")));
    Channels.Add(MakeShared<FJsonValueString>(TEXT("collision_indicator")));
    Semantic->SetArrayField(TEXT("channels"), Channels);
    Root->SetObjectField(TEXT("semantic_minimap"), Semantic);

    FString JsonText;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
    if (!FJsonSerializer::Serialize(Root, Writer))
    {
        UE_LOG(LogTemp, Error, TEXT("Could not serialize capture metadata."));
        return false;
    }

    const FString FilePath = OutputDir / TEXT("metadata.json");
    if (!FFileHelper::SaveStringToFile(JsonText, *FilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("Could not save capture metadata: %s"), *FilePath);
        return false;
    }

    return true;
}

FString AMiniWorldSimulationActor::BuildEffectiveSampleId(const FMiniWorldCaptureSpec& Spec, int32 SequenceIndex, int32 SequenceCount) const
{
    if (SequenceCount <= 1)
    {
        return Spec.SampleId;
    }

    const FMiniWorldCaptureAction& Action = Spec.Actions.IsValidIndex(SequenceIndex)
        ? Spec.Actions[SequenceIndex]
        : Spec.Actions[0];
    if (!Action.Label.IsEmpty())
    {
        return FString::Printf(TEXT("%s_%03d_%s"), *Spec.SampleId, SequenceIndex + 1, *Action.Label);
    }

    return FString::Printf(TEXT("%s_%03d"), *Spec.SampleId, SequenceIndex + 1);
}

FString AMiniWorldSimulationActor::BuildSampleOutputDir(const FMiniWorldCaptureSpec& Spec, int32 SequenceIndex, int32 SequenceCount) const
{
    if (Spec.bOutputDirIsBatchRoot || SequenceCount > 1)
    {
        return Spec.OutputDir / BuildEffectiveSampleId(Spec, SequenceIndex, SequenceCount);
    }

    return Spec.OutputDir;
}

void AMiniWorldSimulationActor::ResetPlayerForCaptureSpec(const FMiniWorldCaptureSpec& Spec)
{
    PlayerPosition = Spec.bHasInitialPose ? Spec.InitialPose.Position : WorldDefinition.SpawnPosition;
    PlayerYawDegrees = Spec.bHasInitialPose ? Spec.InitialPose.YawDegrees : WorldDefinition.SpawnYawDegrees;
    bCollidedLastFrame = false;
    EnsureClearSpawn();
    UpdateWallAppearance();
    UpdateFogCurtains();
    UpdateCapture();
}

bool AMiniWorldSimulationActor::RunCapturePacker()
{
    FString InputDir = ActiveCaptureOutputRoot;
    if (InputDir.IsEmpty() && ActiveCaptureSpecs.Num() == 1)
    {
        InputDir = ActiveCaptureSpecs[0].OutputDir;
    }

    if (InputDir.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("Cannot pack capture output; no capture output root is known."));
        return false;
    }

    FString PythonExe = CapturePythonExe.IsEmpty() ? TEXT("python") : CapturePythonExe;
    if (!CapturePythonExe.IsEmpty() && FPaths::IsRelative(PythonExe))
    {
        const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        PythonExe = FPaths::ConvertRelativePathToFull(ProjectDir, PythonExe);
        FPaths::NormalizeFilename(PythonExe);
    }

    FString ScriptPath = CapturePackScriptPath;
    if (ScriptPath.IsEmpty())
    {
        const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        ScriptPath = FPaths::ConvertRelativePathToFull(ProjectDir, TEXT("python/pack_captures.py"));
    }
    else if (FPaths::IsRelative(ScriptPath))
    {
        const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        ScriptPath = FPaths::ConvertRelativePathToFull(ProjectDir, ScriptPath);
    }
    FPaths::NormalizeFilename(ScriptPath);

    FString OutputPath = CapturePackOutputPath;
    if (OutputPath.IsEmpty())
    {
        const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        const FString DatasetName = FPaths::GetCleanFilename(InputDir);
        OutputPath = FPaths::ConvertRelativePathToFull(ProjectDir, FString::Printf(TEXT("Saved/Datasets/%s.pt"), *DatasetName));
    }
    else if (FPaths::IsRelative(OutputPath))
    {
        const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        OutputPath = FPaths::ConvertRelativePathToFull(ProjectDir, OutputPath);
    }
    else
    {
        OutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
    }
    FPaths::NormalizeFilename(OutputPath);

    FString Arguments = FString::Printf(
        TEXT("%s --input-dir %s --output %s"),
        *QuoteProcessArgument(ScriptPath),
        *QuoteProcessArgument(InputDir),
        *QuoteProcessArgument(OutputPath)
    );
    if (CapturePackSamplesPerShard > 0)
    {
        Arguments += FString::Printf(TEXT(" --samples-per-shard %d"), CapturePackSamplesPerShard);
    }
    if (bCapturePackDeleteInput)
    {
        Arguments += TEXT(" --delete-input");
    }
    if (bCapturePackOverwrite)
    {
        Arguments += TEXT(" --overwrite");
    }

    int32 ReturnCode = 0;
    FString StdOut;
    FString StdErr;
    UE_LOG(LogTemp, Display, TEXT("Running capture packer: %s %s"), *PythonExe, *Arguments);
    const bool bProcessStarted = FPlatformProcess::ExecProcess(*PythonExe, *Arguments, &ReturnCode, &StdOut, &StdErr);
    if (!StdOut.IsEmpty())
    {
        UE_LOG(LogTemp, Display, TEXT("Capture packer output:\n%s"), *StdOut);
    }
    if (!StdErr.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("Capture packer stderr:\n%s"), *StdErr);
    }

    if (!bProcessStarted || ReturnCode != 0)
    {
        UE_LOG(LogTemp, Error, TEXT("Capture packer failed with return code %d."), ReturnCode);
        return false;
    }

    return true;
}

FMiniWorldCaptureStepResult AMiniWorldSimulationActor::ApplyCaptureAction(const FMiniWorldCaptureAction& Action)
{
    FMiniWorldCaptureStepResult Result;
    Result.StartPosition = PlayerPosition;
    Result.StartYawDegrees = PlayerYawDegrees;

    bCollidedLastFrame = false;

    const float DurationSeconds = FMath::Max(Action.DurationSeconds, 0.0f);
    PlayerYawDegrees = FRotator::NormalizeAxis(PlayerYawDegrees + Action.Turn * TurnRateDegrees * DurationSeconds);

    const float MoveDistance = MoveSpeed * DurationSeconds;
    if (!FMath::IsNearlyZero(Action.Forward))
    {
        TryMovePlayer(ForwardFromYaw(PlayerYawDegrees) * (Action.Forward * MoveDistance));
    }

    if (!FMath::IsNearlyZero(Action.Strafe))
    {
        TryMovePlayer(RightFromYaw(PlayerYawDegrees) * (Action.Strafe * MoveDistance));
    }

    Result.EndPosition = PlayerPosition;
    Result.EndYawDegrees = PlayerYawDegrees;
    Result.bCollided = bCollidedLastFrame;
    return Result;
}

void AMiniWorldSimulationActor::LoadOrCreateWorldDefinition()
{
    FString MapPath;
    if (bCaptureModeActive && !ActiveCaptureSpec.MapPath.IsEmpty())
    {
        MapPath = ActiveCaptureSpec.MapPath;
    }
    else
    {
        FParse::Value(FCommandLine::Get(), TEXT("-MiniWorldMap="), MapPath);
    }

    if (MapPath.IsEmpty())
    {
        MapPath = FPaths::ProjectContentDir() / TEXT("Levels/default_world.json");
    }
    else if (FPaths::IsRelative(MapPath))
    {
        MapPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), MapPath);
    }

    if (LoadWorldDefinitionFromJson(MapPath, WorldDefinition))
    {
        UE_LOG(LogTemp, Display, TEXT("Loaded mini-world map: %s (%d line segment(s))"), *MapPath, WorldDefinition.Lines.Num());
        return;
    }

    BuildFallbackWorld(WorldDefinition);
    UE_LOG(LogTemp, Warning, TEXT("Using built-in fallback mini-world map."));
}

bool AMiniWorldSimulationActor::LoadWorldDefinitionFromJson(const FString& FilePath, FMiniWorldDefinition& OutDefinition) const
{
    FString JsonText;
    if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("Could not parse mini-world JSON: %s"), *FilePath);
        return false;
    }

    FMiniWorldDefinition Parsed;

    const TSharedPtr<FJsonObject>* Spawn = nullptr;
    if (Root->TryGetObjectField(TEXT("spawn"), Spawn) && Spawn && Spawn->IsValid())
    {
        ReadJsonNumber(*Spawn, TEXT("x"), Parsed.SpawnPosition.X);
        ReadJsonNumber(*Spawn, TEXT("y"), Parsed.SpawnPosition.Y);
        ReadJsonNumber(*Spawn, TEXT("yaw"), Parsed.SpawnYawDegrees);
    }

    ReadJsonNumber(Root, TEXT("viewDistance"), Parsed.ViewDistance);

    const TArray<TSharedPtr<FJsonValue>>* Walls = nullptr;
    if (!Root->TryGetArrayField(TEXT("walls"), Walls))
    {
        UE_LOG(LogTemp, Warning, TEXT("Mini-world JSON has no walls array: %s"), *FilePath);
        return false;
    }

    for (const TSharedPtr<FJsonValue>& WallValue : *Walls)
    {
        const TSharedPtr<FJsonObject> Wall = WallValue.IsValid() ? WallValue->AsObject() : nullptr;
        if (!Wall.IsValid())
        {
            continue;
        }

        FMiniWorldLine Line;
        ReadJsonNumber(Wall, TEXT("x1"), Line.Start.X);
        ReadJsonNumber(Wall, TEXT("y1"), Line.Start.Y);
        ReadJsonNumber(Wall, TEXT("x2"), Line.End.X);
        ReadJsonNumber(Wall, TEXT("y2"), Line.End.Y);
        ReadJsonNumber(Wall, TEXT("thickness"), Line.Thickness);
        ReadJsonNumber(Wall, TEXT("height"), Line.Height);
        Line.Color = ReadJsonColor(Wall, Line.Color);

        if (!Line.Start.Equals(Line.End, 0.01f))
        {
            Parsed.Lines.Add(Line);
        }
    }

    if (Parsed.Lines.IsEmpty())
    {
        return false;
    }

    OutDefinition = Parsed;
    return true;
}

void AMiniWorldSimulationActor::BuildFallbackWorld(FMiniWorldDefinition& OutDefinition) const
{
    OutDefinition = FMiniWorldDefinition();
    OutDefinition.SpawnPosition = FVector2D(-1800.0f, -1780.0f);
    OutDefinition.SpawnYawDegrees = 30.0f;
    OutDefinition.ViewDistance = 1150.0f;
    OutDefinition.Lines = {
        {FVector2D(-2400.0f, -2400.0f), FVector2D(2400.0f, -2400.0f), FLinearColor(0.18f, 0.45f, 0.86f, 1.0f), 110.0f, 230.0f},
        {FVector2D(2400.0f, -2400.0f), FVector2D(2400.0f, 2400.0f), FLinearColor(0.18f, 0.45f, 0.86f, 1.0f), 135.0f, 230.0f},
        {FVector2D(2400.0f, 2400.0f), FVector2D(-2400.0f, 2400.0f), FLinearColor(0.18f, 0.45f, 0.86f, 1.0f), 120.0f, 230.0f},
        {FVector2D(-2400.0f, 2400.0f), FVector2D(-2400.0f, -2400.0f), FLinearColor(0.18f, 0.45f, 0.86f, 1.0f), 150.0f, 230.0f},
        {FVector2D(-2000.0f, -1100.0f), FVector2D(-820.0f, -1720.0f), FLinearColor(0.86f, 0.22f, 0.18f, 1.0f), 46.0f, 230.0f},
        {FVector2D(-1450.0f, -540.0f), FVector2D(-300.0f, 50.0f), FLinearColor(0.17f, 0.64f, 0.37f, 1.0f), 88.0f, 230.0f},
        {FVector2D(-2180.0f, 100.0f), FVector2D(-960.0f, 580.0f), FLinearColor(0.9f, 0.72f, 0.28f, 1.0f), 172.0f, 230.0f},
        {FVector2D(-1100.0f, 1100.0f), FVector2D(380.0f, 1540.0f), FLinearColor(0.66f, 0.37f, 0.84f, 1.0f), 120.0f, 230.0f},
        {FVector2D(-320.0f, -1960.0f), FVector2D(210.0f, -720.0f), FLinearColor(0.28f, 0.66f, 0.85f, 1.0f), 64.0f, 230.0f},
        {FVector2D(320.0f, -560.0f), FVector2D(1650.0f, -1320.0f), FLinearColor(0.88f, 0.46f, 0.22f, 1.0f), 155.0f, 230.0f},
        {FVector2D(740.0f, -1280.0f), FVector2D(1860.0f, -380.0f), FLinearColor(0.22f, 0.64f, 0.58f, 1.0f), 96.0f, 230.0f},
        {FVector2D(620.0f, 220.0f), FVector2D(2050.0f, 860.0f), FLinearColor(0.8f, 0.24f, 0.36f, 1.0f), 188.0f, 230.0f},
        {FVector2D(220.0f, 920.0f), FVector2D(1480.0f, 1920.0f), FLinearColor(0.42f, 0.72f, 0.26f, 1.0f), 74.0f, 230.0f},
        {FVector2D(-1850.0f, 1740.0f), FVector2D(-360.0f, 930.0f), FLinearColor(0.76f, 0.34f, 0.72f, 1.0f), 130.0f, 230.0f},
        {FVector2D(-2260.0f, 760.0f), FVector2D(-1680.0f, 1880.0f), FLinearColor(0.26f, 0.66f, 0.72f, 1.0f), 52.0f, 230.0f},
        {FVector2D(-620.0f, -310.0f), FVector2D(580.0f, 310.0f), FLinearColor(0.72f, 0.72f, 0.68f, 1.0f), 200.0f, 230.0f},
        {FVector2D(-160.0f, 480.0f), FVector2D(540.0f, -260.0f), FLinearColor(0.55f, 0.78f, 0.24f, 1.0f), 42.0f, 230.0f},
        {FVector2D(1220.0f, 280.0f), FVector2D(1580.0f, 1660.0f), FLinearColor(0.92f, 0.66f, 0.22f, 1.0f), 166.0f, 230.0f},
        {FVector2D(-2140.0f, -2000.0f), FVector2D(-1080.0f, -2100.0f), FLinearColor(0.88f, 0.34f, 0.54f, 1.0f), 58.0f, 230.0f},
        {FVector2D(-760.0f, -1800.0f), FVector2D(-980.0f, -980.0f), FLinearColor(0.28f, 0.78f, 0.9f, 1.0f), 112.0f, 230.0f},
        {FVector2D(1800.0f, -2100.0f), FVector2D(2140.0f, -760.0f), FLinearColor(0.56f, 0.4f, 0.86f, 1.0f), 145.0f, 230.0f},
        {FVector2D(1920.0f, 1260.0f), FVector2D(760.0f, 2100.0f), FLinearColor(0.32f, 0.52f, 0.9f, 1.0f), 68.0f, 230.0f},
        {FVector2D(-2100.0f, -360.0f), FVector2D(-1360.0f, -860.0f), FLinearColor(0.86f, 0.5f, 0.18f, 1.0f), 182.0f, 230.0f},
        {FVector2D(-520.0f, 2060.0f), FVector2D(420.0f, 2140.0f), FLinearColor(0.86f, 0.26f, 0.22f, 1.0f), 99.0f, 230.0f}
    };
}

void AMiniWorldSimulationActor::EnsureClearSpawn()
{
    if (!IsPointBlocked(PlayerPosition))
    {
        return;
    }

    const FVector2D OriginalPosition = PlayerPosition;
    for (float Radius = 80.0f; Radius <= 900.0f; Radius += 80.0f)
    {
        for (int32 Step = 0; Step < 24; ++Step)
        {
            const float Angle = (TWO_PI * Step) / 24.0f;
            const FVector2D Candidate = OriginalPosition + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius;
            if (!IsPointBlocked(Candidate))
            {
                PlayerPosition = Candidate;
                WorldDefinition.SpawnPosition = Candidate;
                UE_LOG(
                    LogTemp,
                    Warning,
                    TEXT("Mini-world spawn was blocked at (%.1f, %.1f); moved to clear point (%.1f, %.1f)."),
                    OriginalPosition.X,
                    OriginalPosition.Y,
                    Candidate.X,
                    Candidate.Y
                );
                return;
            }
        }
    }

    UE_LOG(
        LogTemp,
        Warning,
        TEXT("Mini-world spawn is blocked at (%.1f, %.1f), and no clear nearby point was found."),
        OriginalPosition.X,
        OriginalPosition.Y
    );
}

void AMiniWorldSimulationActor::SpawnScene()
{
    if (AWorldSettings* WorldSettings = GetWorld()->GetWorldSettings())
    {
        WorldSettings->bForceNoPrecomputedLighting = true;
    }

    CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    WallBaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/LevelColorationUnlitMaterial.LevelColorationUnlitMaterial"));
    if (!WallBaseMaterial)
    {
        WallBaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    }

    SpawnGround();
    SpawnWalls();
    SpawnLights();
    SpawnFog();
    SpawnFogCurtains();
    SpawnViewCapture();
    UpdateWallAppearance();
    UpdateFogCurtains();
}

void AMiniWorldSimulationActor::SpawnGround()
{
    if (!PlaneMesh)
    {
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AStaticMeshActor* Ground = GetWorld()->SpawnActor<AStaticMeshActor>(
        FVector(0.0f, 0.0f, -8.0f),
        FRotator::ZeroRotator,
        SpawnParams
    );

    if (!Ground)
    {
        return;
    }

    Ground->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
    Ground->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);
    Ground->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Ground->GetStaticMeshComponent()->SetCastShadow(false);
    Ground->SetActorScale3D(FVector(64.0f, 64.0f, 0.08f));
    Ground->GetStaticMeshComponent()->SetMaterial(0, CreateColorMaterial(FLinearColor(0.22f, 0.25f, 0.23f, 1.0f)));
}

void AMiniWorldSimulationActor::SpawnLights()
{
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    const auto SpawnDirectionalFill = [this, &SpawnParams](const FRotator& Rotation, float Intensity, const FLinearColor& Color)
    {
        ADirectionalLight* DirectionalLight = GetWorld()->SpawnActor<ADirectionalLight>(
            FVector::ZeroVector,
            Rotation,
            SpawnParams
        );

        if (DirectionalLight)
        {
            DirectionalLight->GetLightComponent()->SetMobility(EComponentMobility::Movable);
            DirectionalLight->GetLightComponent()->SetIntensity(Intensity);
            DirectionalLight->GetLightComponent()->SetCastShadows(false);
            DirectionalLight->GetLightComponent()->SetLightColor(Color);
        }
    };

    SpawnDirectionalFill(FRotator(-42.0f, -35.0f, 0.0f), 1.25f, FLinearColor(1.0f, 0.93f, 0.82f, 1.0f));
    SpawnDirectionalFill(FRotator(-28.0f, 55.0f, 0.0f), 0.58f, FLinearColor(0.68f, 0.82f, 1.0f, 1.0f));
    SpawnDirectionalFill(FRotator(-24.0f, 145.0f, 0.0f), 0.46f, FLinearColor(0.88f, 0.94f, 1.0f, 1.0f));
    SpawnDirectionalFill(FRotator(-24.0f, -135.0f, 0.0f), 0.46f, FLinearColor(1.0f, 0.88f, 0.76f, 1.0f));

    ASkyLight* SkyLight = GetWorld()->SpawnActor<ASkyLight>(
        FVector(0.0f, 0.0f, 400.0f),
        FRotator::ZeroRotator,
        SpawnParams
    );

    if (SkyLight)
    {
        SkyLight->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        SkyLight->GetLightComponent()->SetIntensity(6.0f);
        SkyLight->GetLightComponent()->SetLightColor(FLinearColor(0.78f, 0.86f, 1.0f, 1.0f));
        SkyLight->GetLightComponent()->bLowerHemisphereIsBlack = false;
        SkyLight->GetLightComponent()->SetLowerHemisphereColor(FLinearColor(0.62f, 0.68f, 0.72f, 1.0f));
        SkyLight->GetLightComponent()->RecaptureSky();
    }
}

void AMiniWorldSimulationActor::SpawnFog()
{
    if (VisibilityMode == EMiniWorldVisibilityMode::Night)
    {
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AExponentialHeightFog* Fog = GetWorld()->SpawnActor<AExponentialHeightFog>(
        FVector(0.0f, 0.0f, 0.0f),
        FRotator::ZeroRotator,
        SpawnParams
    );

    if (!Fog || !Fog->GetComponent())
    {
        return;
    }

    UExponentialHeightFogComponent* FogComponent = Fog->GetComponent();
    const float VisibleDistance = FMath::Max(WorldDefinition.ViewDistance, 100.0f);
    FogComponent->SetMobility(EComponentMobility::Movable);
    FogComponent->SetFogDensity(0.012f);
    FogComponent->SetFogHeightFalloff(0.08f);
    FogComponent->SetFogMaxOpacity(1.0f);
    FogComponent->SetStartDistance(VisibleDistance * 0.42f);
    FogComponent->SetEndDistance(VisibleDistance * 0.92f);
    FogComponent->SetFogCutoffDistance(1000000.0f);
    FogComponent->SetFogInscatteringColor(FogColor);
    FogComponent->SetDirectionalInscatteringColor(FLinearColor(0.8f, 0.78f, 0.72f, 1.0f));
    FogComponent->SetDirectionalInscatteringStartDistance(VisibleDistance * 0.55f);
    FogComponent->SetVolumetricFog(false);
}

void AMiniWorldSimulationActor::SpawnWalls()
{
    if (!CubeMesh)
    {
        return;
    }

    WallMaterials.Reset();

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    for (const FMiniWorldLine& Line : WorldDefinition.Lines)
    {
        const FVector2D Delta = Line.End - Line.Start;
        const float Length = FMath::Max(Delta.Size(), 1.0f);
        const FVector2D Center = (Line.Start + Line.End) * 0.5f;
        const float YawDegrees = FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
        const float Thickness = FMath::Max(Line.Thickness, 1.0f);
        const float Height = FMath::Max(Line.Height, 1.0f);

        AStaticMeshActor* Wall = GetWorld()->SpawnActor<AStaticMeshActor>(
            FVector(Center.X, Center.Y, Height * 0.5f),
            FRotator(0.0f, YawDegrees, 0.0f),
            SpawnParams
        );

        if (!Wall)
        {
            continue;
        }

        Wall->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Wall->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);
        Wall->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Wall->GetStaticMeshComponent()->SetCastShadow(false);
        Wall->SetActorScale3D(FVector(Length / 100.0f, Thickness / 100.0f, Height / 100.0f));
        UMaterialInstanceDynamic* WallMaterial = CreateColorMaterial(Line.Color);
        Wall->GetStaticMeshComponent()->SetMaterial(0, WallMaterial);
        WallMaterials.Add(WallMaterial);
    }
}

void AMiniWorldSimulationActor::SpawnFogCurtains()
{
    UMaterialInterface* FogMaterialBase = LoadObject<UMaterialInterface>(
        nullptr,
        TEXT("/Paper2D/TranslucentUnlitSpriteMaterial.TranslucentUnlitSpriteMaterial")
    );

    if (!FogMaterialBase)
    {
        return;
    }

    FogCurtainRings.Reset();
    FogCurtainMaterials.Reset();
    FogCurtainTextures.Reset();

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    const int32 SegmentCount = FMath::Max(FogCurtainSegmentsPerRing, 8);
    FogCurtainRings.Reserve(FogCurtainCount);
    FogCurtainMaterials.Reserve(FogCurtainCount);
    FogCurtainTextures.Reserve(FogCurtainCount);

    for (int32 CurtainIndex = 0; CurtainIndex < FogCurtainCount; ++CurtainIndex)
    {
        const float CurtainT = FogCurtainCount > 1
            ? static_cast<float>(CurtainIndex) / static_cast<float>(FogCurtainCount - 1)
            : 1.0f;
        const bool bHorizonRing = CurtainIndex == FogCurtainCount - 1;
        const float Opacity = GetVisibilityLayerOpacity(CurtainT, bHorizonRing);
        const FLinearColor RingVertexColor = GetVisibilityLayerColor(CurtainT, bHorizonRing);

        const float HorizonRadius = WorldDefinition.ViewDistance * VisibilityHorizonScale;
        const float RingRadius = FMath::Lerp(WorldDefinition.ViewDistance * 0.28f, HorizonRadius, CurtainT);
        const float RingHeight = FMath::Max(900.0f, RingRadius * 2.25f);
        const float TextureVerticalAspect = RingRadius > KINDA_SMALL_NUMBER
            ? RingHeight / (TWO_PI * RingRadius)
            : 1.0f;

        AActor* FogRing = GetWorld()->SpawnActor<AActor>(
            FVector::ZeroVector,
            FRotator::ZeroRotator,
            SpawnParams
        );

        if (!FogRing)
        {
            continue;
        }

        UProceduralMeshComponent* MeshComponent = NewObject<UProceduralMeshComponent>(FogRing);
        if (!MeshComponent)
        {
            FogRing->Destroy();
            continue;
        }

        MeshComponent->SetMobility(EComponentMobility::Movable);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComponent->SetCastShadow(false);
        MeshComponent->TranslucencySortPriority = bHorizonRing ? 1000 : 200 + CurtainIndex;
        FogRing->SetRootComponent(MeshComponent);
        FogRing->AddInstanceComponent(MeshComponent);
        MeshComponent->RegisterComponent();
        BuildFogRingMesh(MeshComponent, RingRadius, RingHeight, RingVertexColor, SegmentCount);

        UMaterialInstanceDynamic* FogMaterial = UMaterialInstanceDynamic::Create(FogMaterialBase, this);
        if (FogMaterial)
        {
            UTexture2D* NoiseTexture = CreateFogNoiseTexture(CurtainIndex, CurtainT, TextureVerticalAspect);
            const FLinearColor CurtainColor(
                1.0f,
                1.0f,
                1.0f,
                1.0f
            );

            if (NoiseTexture)
            {
                FogMaterial->SetTextureParameterValue(TEXT("SpriteTexture"), NoiseTexture);
                FogMaterial->SetTextureParameterValue(TEXT("SlateUI"), NoiseTexture);
                FogMaterial->SetTextureParameterValue(TEXT("Texture"), NoiseTexture);
                FogCurtainTextures.Add(NoiseTexture);
            }

            FogMaterial->SetVectorParameterValue(TEXT("TintColorAndOpacity"), CurtainColor);
            FogMaterial->SetVectorParameterValue(TEXT("ColorAndOpacity"), CurtainColor);
            FogMaterial->SetVectorParameterValue(TEXT("Color"), CurtainColor);
            FogMaterial->SetScalarParameterValue(TEXT("OpacityFromTexture"), 1.0f);
            FogMaterial->SetScalarParameterValue(TEXT("Opacity"), Opacity);
            FogMaterial->SetScalarParameterValue(TEXT("InOpacity"), Opacity);
            FogMaterial->SetScalarParameterValue(TEXT("Density"), Opacity);
            MeshComponent->SetMaterial(0, FogMaterial);
        }

        FogCurtainRings.Add(FogRing);
        FogCurtainMaterials.Add(FogMaterial);
    }
}

void AMiniWorldSimulationActor::SpawnViewCapture()
{
    ViewRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("MiniWorldViewRenderTarget"));
    if (!ViewRenderTarget)
    {
        return;
    }

    ViewRenderTarget->ClearColor = GetVisibilityHorizonColor();
    ViewRenderTarget->TargetGamma = 2.2f;
    ViewRenderTarget->Filter = TextureFilter::TF_Nearest;
    ViewRenderTarget->InitCustomFormat(FrameWidth, FrameHeight, PF_B8G8R8A8, false);
    ViewRenderTarget->UpdateResourceImmediate(true);

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    ViewCapture = GetWorld()->SpawnActor<ASceneCapture2D>(
        FVector::ZeroVector,
        FRotator::ZeroRotator,
        SpawnParams
    );

    if (!ViewCapture)
    {
        return;
    }

    USceneCaptureComponent2D* CaptureComponent = ViewCapture->GetCaptureComponent2D();
    CaptureComponent->TextureTarget = ViewRenderTarget;
    CaptureComponent->FOVAngle = CameraFov;
    CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    CaptureComponent->bCaptureEveryFrame = false;
    CaptureComponent->bCaptureOnMovement = false;
    CaptureComponent->bAlwaysPersistRenderingState = true;
    CaptureComponent->MaxViewDistanceOverride = 0.0f;
    CaptureComponent->ShowFlags.SetLighting(true);
    CaptureComponent->ShowFlags.SetMotionBlur(false);
    CaptureComponent->ShowFlags.SetTemporalAA(false);
    CaptureComponent->ShowFlags.SetAntiAliasing(false);
    CaptureComponent->ShowFlags.SetPostProcessing(true);
    CaptureComponent->ShowFlags.SetEyeAdaptation(false);
    CaptureComponent->ShowFlags.SetBloom(false);
    CaptureComponent->ShowFlags.SetAmbientOcclusion(false);
    CaptureComponent->ShowFlags.SetScreenSpaceReflections(false);
    CaptureComponent->ShowFlags.SetFog(true);
    CaptureComponent->ShowFlags.SetVolumetricFog(false);
    CaptureComponent->ShowFlags.SetSeparateTranslucency(true);
    CaptureComponent->ShowFlags.SetAtmosphere(true);
    CaptureComponent->PostProcessSettings.bOverride_AutoExposureMethod = true;
    CaptureComponent->PostProcessSettings.AutoExposureMethod = AEM_Manual;
    CaptureComponent->PostProcessSettings.bOverride_MotionBlurAmount = true;
    CaptureComponent->PostProcessSettings.MotionBlurAmount = 0.0f;
    CaptureComponent->PostProcessSettings.bOverride_FilmGrainIntensity = true;
    CaptureComponent->PostProcessSettings.FilmGrainIntensity = 0.0f;
    CaptureComponent->PostProcessBlendWeight = 1.0f;
}

void AMiniWorldSimulationActor::UpdatePlayer(float DeltaSeconds)
{
    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
    if (!PlayerController)
    {
        return;
    }

    bCollidedLastFrame = false;

    const int32 TurnInput =
        (PlayerController->IsInputKeyDown(EKeys::D) ? 1 : 0) -
        (PlayerController->IsInputKeyDown(EKeys::A) ? 1 : 0);

    PlayerYawDegrees = FRotator::NormalizeAxis(PlayerYawDegrees + TurnInput * TurnRateDegrees * DeltaSeconds);

    const int32 MoveInput =
        (PlayerController->IsInputKeyDown(EKeys::W) ? 1 : 0) -
        (PlayerController->IsInputKeyDown(EKeys::S) ? 1 : 0);

    if (MoveInput != 0)
    {
        TryMovePlayer(ForwardFromYaw(PlayerYawDegrees) * (MoveInput * MoveSpeed * DeltaSeconds));
    }
}

void AMiniWorldSimulationActor::TryMovePlayer(const FVector2D& Delta)
{
    const FVector2D Proposed = PlayerPosition + Delta;
    if (!IsPointBlocked(Proposed))
    {
        PlayerPosition = Proposed;
        return;
    }

    bCollidedLastFrame = true;

    const FVector2D XOnly(PlayerPosition.X + Delta.X, PlayerPosition.Y);
    if (!IsPointBlocked(XOnly))
    {
        PlayerPosition = XOnly;
    }

    const FVector2D YOnly(PlayerPosition.X, PlayerPosition.Y + Delta.Y);
    if (!IsPointBlocked(YOnly))
    {
        PlayerPosition = YOnly;
    }
}

bool AMiniWorldSimulationActor::IsPointBlocked(const FVector2D& Point) const
{
    for (const FMiniWorldLine& Line : WorldDefinition.Lines)
    {
        const float Radius = PlayerRadius + FMath::Max(Line.Thickness * 0.5f, 1.0f);
        if (DistanceSquaredToSegment(Point, Line.Start, Line.End) <= FMath::Square(Radius))
        {
            return true;
        }
    }

    return false;
}

void AMiniWorldSimulationActor::UpdateWallAppearance()
{
    const int32 Count = FMath::Min(WorldDefinition.Lines.Num(), WallMaterials.Num());
    for (int32 Index = 0; Index < Count; ++Index)
    {
        UMaterialInstanceDynamic* Material = WallMaterials[Index];
        if (!Material)
        {
            continue;
        }

        const FLinearColor DisplayColor = ComputeWallDisplayColor(WorldDefinition.Lines[Index]);
        Material->SetVectorParameterValue(TEXT("Color"), DisplayColor);
        Material->SetVectorParameterValue(TEXT("BaseColor"), DisplayColor);
        Material->SetVectorParameterValue(TEXT("EmissiveColor"), DisplayColor);
    }
}

void AMiniWorldSimulationActor::UpdateFogCurtains()
{
    if (FogCurtainRings.IsEmpty())
    {
        return;
    }

    const FVector RingCenter(PlayerPosition.X, PlayerPosition.Y, EyeHeight);
    for (AActor* FogRing : FogCurtainRings)
    {
        if (FogRing)
        {
            FogRing->SetActorLocation(RingCenter);
        }
    }
}

void AMiniWorldSimulationActor::UpdateCapture()
{
    if (!ViewCapture)
    {
        return;
    }

    const FVector CaptureLocation(PlayerPosition.X, PlayerPosition.Y, EyeHeight);
    const FRotator CaptureRotation(0.0f, PlayerYawDegrees, 0.0f);
    ViewCapture->SetActorLocationAndRotation(CaptureLocation, CaptureRotation);

    if (USceneCaptureComponent2D* CaptureComponent = ViewCapture->GetCaptureComponent2D())
    {
        CaptureComponent->CaptureScene();
    }
}

void AMiniWorldSimulationActor::MaybeRequestModelFrame()
{
    if (!bModelRenderModeActive || bModelRequestInFlight || ModelServerUrl.IsEmpty())
    {
        return;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    const double MinIntervalSeconds = 1.0 / FMath::Max(ModelRequestFps, 1.0f);
    if (LastModelRequestTimeSeconds > 0.0 && NowSeconds - LastModelRequestTimeSeconds < MinIntervalSeconds)
    {
        return;
    }

    TArray<FColor> UnusedPixels;
    TArray<uint8> SemanticData;
    BuildMinimapPixels(ModelMinimapSize, bCollidedLastFrame, UnusedPixels, &SemanticData);

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("request_id"), ++ModelRequestCounter);
    Root->SetNumberField(TEXT("frame_width"), FrameWidth);
    Root->SetNumberField(TEXT("frame_height"), FrameHeight);
    Root->SetNumberField(TEXT("minimap_size"), ModelMinimapSize);
    Root->SetNumberField(TEXT("semantic_channels"), 6);
    Root->SetStringField(TEXT("semantic_layout"), TEXT("CHW"));
    Root->SetStringField(TEXT("minimap_semantic_b64"), FBase64::Encode(SemanticData));
    Root->SetNumberField(TEXT("view_distance"), WorldDefinition.ViewDistance);
    Root->SetBoolField(TEXT("collision_indicator"), bCollidedLastFrame);
    Root->SetObjectField(TEXT("pose"), PoseToJson(PlayerPosition, PlayerYawDegrees));

    FString JsonText;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
    if (!FJsonSerializer::Serialize(Root, Writer))
    {
        LogModelServerWarning(TEXT("Could not serialize model render request."));
        return;
    }

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(ModelServerUrl);
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
    Request->SetContentAsString(JsonText);
    Request->SetTimeout(2.0f);
    Request->OnProcessRequestComplete().BindUObject(this, &AMiniWorldSimulationActor::HandleModelFrameResponse);

    bModelRequestInFlight = true;
    LastModelRequestTimeSeconds = NowSeconds;
    if (!Request->ProcessRequest())
    {
        bModelRequestInFlight = false;
        LogModelServerWarning(FString::Printf(TEXT("Could not start model render request to %s."), *ModelServerUrl));
    }
}

void AMiniWorldSimulationActor::HandleModelFrameResponse(
    FHttpRequestPtr Request,
    FHttpResponsePtr Response,
    bool bWasSuccessful
)
{
    bModelRequestInFlight = false;

    if (!bWasSuccessful || !Response.IsValid())
    {
        LogModelServerWarning(TEXT("Model server request failed."));
        return;
    }

    const int32 StatusCode = Response->GetResponseCode();
    if (StatusCode < 200 || StatusCode >= 300)
    {
        LogModelServerWarning(FString::Printf(
            TEXT("Model server returned HTTP %d: %s"),
            StatusCode,
            *Response->GetContentAsString()
        ));
        return;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        LogModelServerWarning(TEXT("Could not parse model server JSON response."));
        return;
    }

    bool bOk = false;
    if (Root->TryGetBoolField(TEXT("ok"), bOk) && !bOk)
    {
        FString ErrorText;
        Root->TryGetStringField(TEXT("error"), ErrorText);
        LogModelServerWarning(FString::Printf(TEXT("Model server error: %s"), *ErrorText));
        return;
    }

    int32 Width = FrameWidth;
    int32 Height = FrameHeight;
    int32 Channels = 3;
    ReadJsonNumber(Root, TEXT("width"), Width);
    ReadJsonNumber(Root, TEXT("height"), Height);
    ReadJsonNumber(Root, TEXT("channels"), Channels);

    FString RgbBase64;
    if (!Root->TryGetStringField(TEXT("rgb_b64"), RgbBase64) || RgbBase64.IsEmpty())
    {
        LogModelServerWarning(TEXT("Model server response did not include rgb_b64."));
        return;
    }

    TArray<uint8> RgbData;
    if (!FBase64::Decode(RgbBase64, RgbData))
    {
        LogModelServerWarning(TEXT("Could not decode model server rgb_b64 response."));
        return;
    }

    if (Channels != 3 || RgbData.Num() != Width * Height * 3)
    {
        LogModelServerWarning(FString::Printf(
            TEXT("Model server returned unexpected frame shape %dx%dx%d with %d bytes."),
            Width,
            Height,
            Channels,
            RgbData.Num()
        ));
        return;
    }

    UpdateModelFrameTexture(RgbData, Width, Height);
}

void AMiniWorldSimulationActor::UpdateModelFrameTexture(const TArray<uint8>& RgbData, int32 Width, int32 Height)
{
    if (Width <= 0 || Height <= 0 || RgbData.Num() != Width * Height * 3)
    {
        return;
    }

    if (!ModelFrameTexture || ModelFrameTexture->GetSizeX() != Width || ModelFrameTexture->GetSizeY() != Height)
    {
        ModelFrameTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8, TEXT("MiniWorldModelFrameTexture"));
        if (!ModelFrameTexture)
        {
            LogModelServerWarning(TEXT("Could not create model frame texture."));
            return;
        }

        ModelFrameTexture->SRGB = true;
        ModelFrameTexture->Filter = TextureFilter::TF_Nearest;
        ModelFrameTexture->UpdateResource();
    }

    TArray<FColor> BgraPixels;
    const int32 PixelCount = Width * Height;
    BgraPixels.SetNumUninitialized(PixelCount);
    for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
    {
        const int32 SourceIndex = PixelIndex * 3;
        BgraPixels[PixelIndex] = FColor(
            RgbData[SourceIndex + 0],
            RgbData[SourceIndex + 1],
            RgbData[SourceIndex + 2],
            255
        );
    }

    if (!ModelFrameTexture->GetPlatformData() || ModelFrameTexture->GetPlatformData()->Mips.IsEmpty())
    {
        LogModelServerWarning(TEXT("Model frame texture has no writable platform data."));
        return;
    }

    FTexture2DMipMap& Mip = ModelFrameTexture->GetPlatformData()->Mips[0];
    void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(TextureData, BgraPixels.GetData(), BgraPixels.Num() * sizeof(FColor));
    Mip.BulkData.Unlock();
    ModelFrameTexture->UpdateResource();
}

void AMiniWorldSimulationActor::LogModelServerWarning(const FString& Message)
{
    const double NowSeconds = FPlatformTime::Seconds();
    if (NowSeconds - LastModelWarningTimeSeconds < 2.0)
    {
        return;
    }

    LastModelWarningTimeSeconds = NowSeconds;
    UE_LOG(LogTemp, Warning, TEXT("%s"), *Message);
}

void AMiniWorldSimulationActor::BuildFogRingMesh(
    UProceduralMeshComponent* MeshComponent,
    float Radius,
    float Height,
    const FLinearColor& VertexColor,
    int32 SegmentCount
) const
{
    if (!MeshComponent)
    {
        return;
    }

    const int32 ClampedSegmentCount = FMath::Max(SegmentCount, 8);
    const float HalfHeight = Height * 0.5f;
    const FLinearColor ClampedVertexColor(
        FMath::Clamp(VertexColor.R, 0.0f, 1.0f),
        FMath::Clamp(VertexColor.G, 0.0f, 1.0f),
        FMath::Clamp(VertexColor.B, 0.0f, 1.0f),
        FMath::Clamp(VertexColor.A, 0.0f, 1.0f)
    );

    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> VertexColors;
    TArray<FProcMeshTangent> Tangents;

    Vertices.Reserve((ClampedSegmentCount + 1) * 2);
    Normals.Reserve((ClampedSegmentCount + 1) * 2);
    UVs.Reserve((ClampedSegmentCount + 1) * 2);
    VertexColors.Reserve((ClampedSegmentCount + 1) * 2);
    Tangents.Reserve((ClampedSegmentCount + 1) * 2);
    Triangles.Reserve(ClampedSegmentCount * 6);

    for (int32 SegmentIndex = 0; SegmentIndex <= ClampedSegmentCount; ++SegmentIndex)
    {
        const float SegmentT = static_cast<float>(SegmentIndex) / static_cast<float>(ClampedSegmentCount);
        const float Angle = SegmentT * TWO_PI;
        const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
        const FVector Tangent(-Direction.Y, Direction.X, 0.0f);
        const FVector InwardNormal = -Direction;

        Vertices.Add(FVector(Direction.X * Radius, Direction.Y * Radius, -HalfHeight));
        Vertices.Add(FVector(Direction.X * Radius, Direction.Y * Radius, HalfHeight));
        Normals.Add(InwardNormal);
        Normals.Add(InwardNormal);
        UVs.Add(FVector2D(SegmentT, 1.0f));
        UVs.Add(FVector2D(SegmentT, 0.0f));
        VertexColors.Add(ClampedVertexColor);
        VertexColors.Add(ClampedVertexColor);
        Tangents.Add(FProcMeshTangent(Tangent.X, Tangent.Y, Tangent.Z));
        Tangents.Add(FProcMeshTangent(Tangent.X, Tangent.Y, Tangent.Z));
    }

    for (int32 SegmentIndex = 0; SegmentIndex < ClampedSegmentCount; ++SegmentIndex)
    {
        const int32 Bottom0 = SegmentIndex * 2;
        const int32 Top0 = Bottom0 + 1;
        const int32 Bottom1 = Bottom0 + 2;
        const int32 Top1 = Bottom1 + 1;

        Triangles.Add(Bottom0);
        Triangles.Add(Top1);
        Triangles.Add(Bottom1);
        Triangles.Add(Bottom0);
        Triangles.Add(Top0);
        Triangles.Add(Top1);
    }

    MeshComponent->ClearAllMeshSections();
    MeshComponent->CreateMeshSection_LinearColor(
        0,
        Vertices,
        Triangles,
        Normals,
        UVs,
        VertexColors,
        Tangents,
        false
    );
}

UMaterialInstanceDynamic* AMiniWorldSimulationActor::CreateColorMaterial(const FLinearColor& Color)
{
    UMaterialInterface* BaseMaterial = WallBaseMaterial ? WallBaseMaterial : UMaterial::GetDefaultMaterial(MD_Surface);
    UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(BaseMaterial, this);
    if (Material)
    {
        Material->SetVectorParameterValue(TEXT("Color"), Color);
        Material->SetVectorParameterValue(TEXT("BaseColor"), Color);
        Material->SetVectorParameterValue(TEXT("EmissiveColor"), Color);
        Material->SetScalarParameterValue(TEXT("Roughness"), 0.82f);
    }

    return Material;
}

UTexture2D* AMiniWorldSimulationActor::CreateFogNoiseTexture(int32 TextureSeedIndex, float CurtainT, float VerticalAspect)
{
    if (CurtainT >= 0.999f)
    {
        constexpr int32 SolidTextureSize = 4;
        TArray64<uint8> SolidPixels;
        SolidPixels.SetNum(SolidTextureSize * SolidTextureSize * 4);
        for (int64 PixelIndex = 0; PixelIndex < SolidPixels.Num(); PixelIndex += 4)
        {
            SolidPixels[PixelIndex + 0] = 255;
            SolidPixels[PixelIndex + 1] = 255;
            SolidPixels[PixelIndex + 2] = 255;
            SolidPixels[PixelIndex + 3] = 255;
        }

        UTexture2D* Texture = UTexture2D::CreateTransient(
            SolidTextureSize,
            SolidTextureSize,
            PF_B8G8R8A8,
            FName(*FString::Printf(TEXT("MiniWorldFogHorizon_%d"), TextureSeedIndex)),
            SolidPixels
        );

        if (Texture)
        {
            Texture->SRGB = true;
            Texture->Filter = TF_Nearest;
            Texture->AddressX = TA_Wrap;
            Texture->AddressY = TA_Clamp;
            Texture->MipGenSettings = TMGS_NoMipmaps;
            Texture->UpdateResource();
        }

        return Texture;
    }

    constexpr int32 TextureSizeX = 2048;
    constexpr int32 CoarseGridSizeX = 160;
    const float ClampedVerticalAspect = FMath::Clamp(VerticalAspect, 0.12f, 1.0f);
    const int32 TextureSizeY = FMath::Clamp(
        FMath::RoundToInt(static_cast<float>(TextureSizeX) * ClampedVerticalAspect),
        256,
        1024
    );
    const int32 CoarseGridSizeY = FMath::Clamp(
        FMath::RoundToInt(static_cast<float>(CoarseGridSizeX) * ClampedVerticalAspect),
        16,
        96
    );
    const int32 CoarseStrideX = CoarseGridSizeX;
    const int32 CoarseStrideY = CoarseGridSizeY + 1;

    FRandomStream RandomStream(7301 + TextureSeedIndex * 97);
    TArray<float> CoarseValues;
    CoarseValues.SetNum(CoarseStrideX * CoarseStrideY);
    for (float& Value : CoarseValues)
    {
        Value = RandomStream.FRandRange(-1.0f, 1.0f);
    }

    const auto SampleCoarse = [&CoarseValues, CoarseStrideX, CoarseGridSizeX, CoarseGridSizeY](int32 X, int32 Y) -> float
    {
        const int32 ClampedX = ((X % CoarseGridSizeX) + CoarseGridSizeX) % CoarseGridSizeX;
        const int32 ClampedY = FMath::Clamp(Y, 0, CoarseGridSizeY);
        return CoarseValues[ClampedY * CoarseStrideX + ClampedX];
    };

    const auto Hash01 = [](int32 X, int32 Y, int32 Seed) -> float
    {
        uint32 Hash = 2166136261u;
        Hash = (Hash ^ static_cast<uint32>(X)) * 16777619u;
        Hash = (Hash ^ static_cast<uint32>(Y)) * 16777619u;
        Hash = (Hash ^ static_cast<uint32>(Seed)) * 16777619u;
        Hash ^= Hash >> 13;
        Hash *= 1274126177u;
        Hash ^= Hash >> 16;
        return static_cast<float>(Hash & 0x00FFFFFFu) / 16777215.0f;
    };

    TArray64<uint8> Pixels;
    Pixels.SetNum(static_cast<int64>(TextureSizeX) * TextureSizeY * 4);

    const float LayerAlphaBias = FMath::Lerp(0.54f, 0.88f, CurtainT);
    const float NoiseStrength = FMath::Lerp(0.28f, 0.18f, CurtainT);

    for (int32 Y = 0; Y < TextureSizeY; ++Y)
    {
        for (int32 X = 0; X < TextureSizeX; ++X)
        {
            const float GridX = (static_cast<float>(X) / static_cast<float>(TextureSizeX - 1)) * CoarseGridSizeX;
            const float GridY = (static_cast<float>(Y) / static_cast<float>(TextureSizeY - 1)) * CoarseGridSizeY;
            const int32 X0 = FMath::FloorToInt(GridX);
            const int32 Y0 = FMath::FloorToInt(GridY);
            const float Tx = SmoothStep(GridX - X0);
            const float Ty = SmoothStep(GridY - Y0);

            const float A = SampleCoarse(X0, Y0);
            const float B = SampleCoarse(X0 + 1, Y0);
            const float C = SampleCoarse(X0, Y0 + 1);
            const float D = SampleCoarse(X0 + 1, Y0 + 1);
            const float LowFrequencyNoise = FMath::Lerp(
                FMath::Lerp(A, B, Tx),
                FMath::Lerp(C, D, Tx),
                Ty
            );

            const int32 WrappedPixelX = X == TextureSizeX - 1 ? 0 : X;
            const float SpeckleChance = Hash01(WrappedPixelX, Y, TextureSeedIndex * 2 + 17);
            const float Speckle = SpeckleChance < 0.08f
                ? FMath::Lerp(-0.24f, 0.30f, Hash01(WrappedPixelX, Y, TextureSeedIndex * 2 + 41))
                : 0.0f;

            const float Alpha01 = FMath::Clamp(
                LayerAlphaBias + LowFrequencyNoise * NoiseStrength + Speckle,
                0.08f,
                1.0f
            );
            const uint8 AlphaByte = static_cast<uint8>(FMath::RoundToInt(Alpha01 * 255.0f));

            const int64 PixelIndex = (static_cast<int64>(Y) * TextureSizeX + X) * 4;
            Pixels[PixelIndex + 0] = 255;
            Pixels[PixelIndex + 1] = 255;
            Pixels[PixelIndex + 2] = 255;
            Pixels[PixelIndex + 3] = AlphaByte;
        }
    }

    UTexture2D* Texture = UTexture2D::CreateTransient(
        TextureSizeX,
        TextureSizeY,
        PF_B8G8R8A8,
        FName(*FString::Printf(TEXT("MiniWorldFogNoise_%d"), TextureSeedIndex)),
        Pixels
    );

    if (Texture)
    {
        Texture->SRGB = true;
        Texture->Filter = TF_Nearest;
        Texture->AddressX = TA_Wrap;
        Texture->AddressY = TA_Clamp;
        Texture->MipGenSettings = TMGS_NoMipmaps;
        Texture->UpdateResource();
    }

    return Texture;
}

float AMiniWorldSimulationActor::GetVisibilityLayerOpacity(float CurtainT, bool bHorizonRing) const
{
    if (bHorizonRing)
    {
        return 1.0f;
    }

    const float DensityT = FMath::Pow(CurtainT, 1.35f);
    if (VisibilityMode == EMiniWorldVisibilityMode::Night)
    {
        return FMath::Lerp(0.08f, 0.52f, DensityT);
    }

    return FMath::Lerp(0.04f, 0.24f, DensityT);
}

FLinearColor AMiniWorldSimulationActor::GetVisibilityLayerColor(float CurtainT, bool bHorizonRing) const
{
    if (bHorizonRing)
    {
        return GetVisibilityHorizonColor();
    }

    const float Opacity = GetVisibilityLayerOpacity(CurtainT, false);
    if (VisibilityMode == EMiniWorldVisibilityMode::Night)
    {
        return FLinearColor(NightLayerColor.R, NightLayerColor.G, NightLayerColor.B, Opacity);
    }

    return FLinearColor(1.0f, 1.0f, 1.0f, Opacity);
}

FLinearColor AMiniWorldSimulationActor::GetVisibilityHorizonColor() const
{
    return VisibilityMode == EMiniWorldVisibilityMode::Night
        ? NightHorizonColor
        : FogHorizonColor;
}

FLinearColor AMiniWorldSimulationActor::ComputeWallDisplayColor(const FMiniWorldLine& Line) const
{
    const FVector2D Delta = Line.End - Line.Start;
    FVector2D Normal(-Delta.Y, Delta.X);
    if (!Normal.Normalize())
    {
        Normal = FVector2D(1.0f, 0.0f);
    }

    const FVector2D KeyLight = ForwardFromYaw(-35.0f);
    const FVector2D FillLight = ForwardFromYaw(145.0f);
    const float KeyAmount = FMath::Abs(FVector2D::DotProduct(Normal, KeyLight));
    const float FillAmount = FMath::Abs(FVector2D::DotProduct(Normal, FillLight));
    const float Shade = FMath::Clamp(0.66f + KeyAmount * 0.24f + FillAmount * 0.10f, 0.66f, 1.0f);

    FLinearColor ShadedColor(
        FMath::Clamp(Line.Color.R * Shade + 0.045f, 0.0f, 1.0f),
        FMath::Clamp(Line.Color.G * Shade + 0.045f, 0.0f, 1.0f),
        FMath::Clamp(Line.Color.B * Shade + 0.045f, 0.0f, 1.0f),
        1.0f
    );

    return ShadedColor;
}
