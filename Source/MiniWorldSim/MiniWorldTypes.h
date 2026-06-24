#pragma once

#include "CoreMinimal.h"

struct FMiniWorldLine
{
    FVector2D Start = FVector2D::ZeroVector;
    FVector2D End = FVector2D::ZeroVector;
    FLinearColor Color = FLinearColor(0.7f, 0.7f, 0.72f, 1.0f);
    float Thickness = 48.0f;
    float Height = 220.0f;
};

struct FMiniWorldDefinition
{
    TArray<FMiniWorldLine> Lines;
    FVector2D SpawnPosition = FVector2D(0.0f, -520.0f);
    float SpawnYawDegrees = 0.0f;
    float ViewDistance = 950.0f;
};
