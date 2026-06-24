#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "MiniWorldHud.generated.h"

class AMiniWorldSimulationActor;

UCLASS()
class MINIWORLDSIM_API AMiniWorldHud : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;

private:
    AMiniWorldSimulationActor* FindSimulationActor() const;
    void DrawMiniMap(const AMiniWorldSimulationActor& Simulation, const FVector2D& Origin, const FVector2D& Size);
    void DrawFilledTriangle(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FLinearColor& Color);
    void DrawFilledQuad(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D, const FLinearColor& Color);
    void DrawFilledPolygon(const TArray<FVector2D>& Points, const FLinearColor& Color);
    void DrawPanelBorder(const FVector2D& Origin, const FVector2D& Size, const FLinearColor& Color);
    void DrawCircleOutline(const FVector2D& Center, float Radius, float Thickness, const FLinearColor& Color, int32 Segments = 96);
};
