#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IHttpRequest.h"
#include "MiniWorldTypes.h"
#include "MiniWorldSimulationActor.generated.h"

class ASceneCapture2D;
class AStaticMeshActor;
class UProceduralMeshComponent;
class UTexture;
class UTexture2D;
class UTextureRenderTarget2D;

enum class EMiniWorldVisibilityMode : uint8
{
    Fog,
    Night
};

struct FMiniWorldCaptureAction
{
    FString Label;
    float Forward = 0.0f;
    float Strafe = 0.0f;
    float Turn = 0.0f;
    float DurationSeconds = 0.05f;
};

struct FMiniWorldCapturePose
{
    FVector2D Position = FVector2D::ZeroVector;
    float YawDegrees = 0.0f;
};

struct FMiniWorldCaptureSpec
{
    FString SpecPath;
    FString SpecDirectory;
    FString SampleId = TEXT("sample");
    FString MapPath;
    FString OutputDir;
    bool bOutputDirIsBatchRoot = false;
    FMiniWorldCapturePose InitialPose;
    bool bHasInitialPose = false;
    TArray<FMiniWorldCaptureAction> Actions;
    bool bIncludeRgb = true;
    bool bIncludeMinimapRgb = true;
    bool bIncludeSemanticMinimap = true;
    bool bExitAfterCapture = true;
    float StartDelaySeconds = 1.0f;
    int32 MinimapSize = 128;
};

struct FMiniWorldCaptureStepResult
{
    FVector2D StartPosition = FVector2D::ZeroVector;
    FVector2D EndPosition = FVector2D::ZeroVector;
    float StartYawDegrees = 0.0f;
    float EndYawDegrees = 0.0f;
    bool bCollided = false;
};

UCLASS()
class MINIWORLDSIM_API AMiniWorldSimulationActor : public AActor
{
    GENERATED_BODY()

public:
    AMiniWorldSimulationActor();

    virtual void Tick(float DeltaSeconds) override;

    UTextureRenderTarget2D* GetViewRenderTarget() const { return ViewRenderTarget; }
    UTexture* GetDisplayViewTexture() const;
    const TArray<FMiniWorldLine>& GetLines() const { return WorldDefinition.Lines; }
    FVector2D GetPlayerPosition() const { return PlayerPosition; }
    float GetPlayerYawDegrees() const { return PlayerYawDegrees; }
    float GetViewDistance() const { return WorldDefinition.ViewDistance; }
    bool HadCollisionLastFrame() const { return bCollidedLastFrame; }
    bool IsModelRenderModeActive() const { return bModelRenderModeActive; }

protected:
    virtual void BeginPlay() override;

private:
    bool ReadCaptureSpecCommandLine(FMiniWorldCaptureSpec& OutSpec);
    bool LoadCaptureSpec(const FString& SpecPath, FMiniWorldCaptureSpec& OutSpec) const;
    bool LoadCaptureSpecDirectory(const FString& SpecDirectory, TArray<FMiniWorldCaptureSpec>& OutSpecs, FString& OutBatchOutputDir) const;
    FString ResolveCapturePath(const FString& Path, const FString& BaseDirectory, bool bPreferProjectRelative) const;
    void RunQueuedCaptureSpec();
    bool RunActiveCaptureSpecs();
    bool RunCaptureSpec(const FMiniWorldCaptureSpec& Spec);
    bool CaptureFrameArtifacts(const FMiniWorldCaptureSpec& Spec, const FString& OutputDir, const FString& Suffix, bool bCollisionIndicator);
    bool SaveViewRenderTargetPng(const FString& FilePath);
    bool SaveMinimapArtifacts(const FMiniWorldCaptureSpec& Spec, const FString& OutputDir, const FString& Suffix, bool bCollisionIndicator);
    bool SaveSemanticMinimap(const FString& FilePath, int32 MinimapSize, bool bCollisionIndicator);
    void BuildMinimapPixels(int32 MinimapSize, bool bCollisionIndicator, TArray<FColor>& OutPixels, TArray<uint8>* OutSemanticData = nullptr) const;
    bool SaveCaptureMetadata(
        const FMiniWorldCaptureSpec& Spec,
        const FMiniWorldCaptureAction& Action,
        const FMiniWorldCaptureStepResult& StepResult,
        const FString& OutputDir,
        const FString& EffectiveSampleId,
        int32 SequenceIndex,
        int32 SequenceCount
    );
    FMiniWorldCaptureStepResult ApplyCaptureAction(const FMiniWorldCaptureAction& Action);
    FString BuildSampleOutputDir(const FMiniWorldCaptureSpec& Spec, int32 SequenceIndex, int32 SequenceCount) const;
    FString BuildEffectiveSampleId(const FMiniWorldCaptureSpec& Spec, int32 SequenceIndex, int32 SequenceCount) const;
    void ResetPlayerForCaptureSpec(const FMiniWorldCaptureSpec& Spec);
    bool RunCapturePacker();
    void LoadOrCreateWorldDefinition();
    bool LoadWorldDefinitionFromJson(const FString& FilePath, FMiniWorldDefinition& OutDefinition) const;
    void BuildFallbackWorld(FMiniWorldDefinition& OutDefinition) const;
    void EnsureClearSpawn();

    void SpawnScene();
    void SpawnGround();
    void SpawnLights();
    void SpawnFog();
    void SpawnFogCurtains();
    void SpawnWalls();
    void SpawnViewCapture();

    void UpdatePlayer(float DeltaSeconds);
    void TryMovePlayer(const FVector2D& Delta);
    bool IsPointBlocked(const FVector2D& Point) const;
    void UpdateWallAppearance();
    void UpdateFogCurtains();
    void UpdateCapture();
    void MaybeRequestModelFrame();
    void HandleModelFrameResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
    void UpdateModelFrameTexture(const TArray<uint8>& RgbData, int32 Width, int32 Height);
    void LogModelServerWarning(const FString& Message);

    UMaterialInstanceDynamic* CreateColorMaterial(const FLinearColor& Color);
    void BuildFogRingMesh(UProceduralMeshComponent* MeshComponent, float Radius, float Height, const FLinearColor& VertexColor, int32 SegmentCount) const;
    UTexture2D* CreateFogNoiseTexture(int32 TextureSeedIndex, float CurtainT, float VerticalAspect);
    float GetVisibilityLayerOpacity(float CurtainT, bool bHorizonRing) const;
    FLinearColor GetVisibilityLayerColor(float CurtainT, bool bHorizonRing) const;
    FLinearColor GetVisibilityHorizonColor() const;
    FLinearColor ComputeWallDisplayColor(const FMiniWorldLine& Line) const;

    UPROPERTY()
    UTextureRenderTarget2D* ViewRenderTarget = nullptr;

    UPROPERTY()
    UTexture2D* ModelFrameTexture = nullptr;

    UPROPERTY()
    ASceneCapture2D* ViewCapture = nullptr;

    UPROPERTY()
    UMaterialInterface* WallBaseMaterial = nullptr;

    UPROPERTY()
    UStaticMesh* CubeMesh = nullptr;

    UPROPERTY()
    UStaticMesh* PlaneMesh = nullptr;

    UPROPERTY()
    TArray<TObjectPtr<AActor>> FogCurtainRings;

    UPROPERTY()
    TArray<TObjectPtr<UMaterialInstanceDynamic>> FogCurtainMaterials;

    UPROPERTY()
    TArray<TObjectPtr<UTexture2D>> FogCurtainTextures;

    UPROPERTY()
    TArray<TObjectPtr<UMaterialInstanceDynamic>> WallMaterials;

    FMiniWorldDefinition WorldDefinition;
    FVector2D PlayerPosition = FVector2D::ZeroVector;
    float PlayerYawDegrees = 0.0f;
    bool bCollidedLastFrame = false;

    float MoveSpeed = 260.0f;
    float TurnRateDegrees = 130.0f;
    float PlayerRadius = 28.0f;
    float EyeHeight = 92.0f;
    float CameraFov = 86.0f;
    int32 FrameWidth = 128;
    int32 FrameHeight = 128;
    FLinearColor FogColor = FLinearColor(0.58f, 0.64f, 0.68f, 1.0f);
    FLinearColor FogHorizonColor = FLinearColor(0.86f, 0.88f, 0.87f, 1.0f);
    FLinearColor NightHorizonColor = FLinearColor(0.015f, 0.02f, 0.035f, 1.0f);
    FLinearColor NightLayerColor = FLinearColor(0.025f, 0.035f, 0.075f, 1.0f);
    EMiniWorldVisibilityMode VisibilityMode = EMiniWorldVisibilityMode::Night;
    float VisibilityHorizonScale = 0.9823f;
    int32 FogCurtainCount = 5;
    int32 FogCurtainSegmentsPerRing = 96;

    bool bModelRenderModeActive = false;
    bool bModelRequestInFlight = false;
    FString ModelServerUrl = TEXT("http://127.0.0.1:8765/render");
    float ModelRequestFps = 20.0f;
    int32 ModelMinimapSize = 128;
    int32 ModelRequestCounter = 0;
    double LastModelRequestTimeSeconds = 0.0;
    double LastModelWarningTimeSeconds = -1000.0;

    bool bCaptureModeActive = false;
    FMiniWorldCaptureSpec ActiveCaptureSpec;
    TArray<FMiniWorldCaptureSpec> ActiveCaptureSpecs;
    FString ActiveCaptureOutputRoot;
    bool bCapturePackAfterRun = false;
    bool bCapturePackDeleteInput = false;
    bool bCapturePackOverwrite = true;
    int32 CapturePackSamplesPerShard = 0;
    FString CapturePythonExe;
    FString CapturePackScriptPath;
    FString CapturePackOutputPath;
};
