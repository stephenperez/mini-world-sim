#include "MiniWorldHud.h"

#include "Engine/Canvas.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "MiniWorldSimulationActor.h"

namespace
{
float Cross2D(const FVector2D& A, const FVector2D& B)
{
    return A.X * B.Y - A.Y * B.X;
}

bool IsInsideClipEdge(const FVector2D& Point, const FVector2D& EdgeA, const FVector2D& EdgeB)
{
    return Cross2D(EdgeB - EdgeA, Point - EdgeA) >= -KINDA_SMALL_NUMBER;
}

FVector2D IntersectClipEdge(
    const FVector2D& SegmentA,
    const FVector2D& SegmentB,
    const FVector2D& EdgeA,
    const FVector2D& EdgeB
)
{
    const FVector2D SegmentDelta = SegmentB - SegmentA;
    const FVector2D EdgeDelta = EdgeB - EdgeA;
    const float Denominator = Cross2D(SegmentDelta, EdgeDelta);
    if (FMath::Abs(Denominator) <= KINDA_SMALL_NUMBER)
    {
        return SegmentB;
    }

    const float T = Cross2D(EdgeA - SegmentA, EdgeDelta) / Denominator;
    return SegmentA + SegmentDelta * FMath::Clamp(T, 0.0f, 1.0f);
}

void ClipPolygonToCircle(
    const TArray<FVector2D>& SourcePolygon,
    const FVector2D& Center,
    float Radius,
    int32 Segments,
    TArray<FVector2D>& OutPolygon
)
{
    OutPolygon = SourcePolygon;
    if (OutPolygon.Num() < 3)
    {
        OutPolygon.Reset();
        return;
    }

    const int32 ClampedSegments = FMath::Max(Segments, 16);
    TArray<FVector2D> WorkingPolygon;
    WorkingPolygon.Reserve(OutPolygon.Num() + ClampedSegments);

    for (int32 ClipIndex = 0; ClipIndex < ClampedSegments; ++ClipIndex)
    {
        const float AngleA = (TWO_PI * ClipIndex) / static_cast<float>(ClampedSegments);
        const float AngleB = (TWO_PI * (ClipIndex + 1)) / static_cast<float>(ClampedSegments);
        const FVector2D EdgeA = Center + FVector2D(FMath::Cos(AngleA), FMath::Sin(AngleA)) * Radius;
        const FVector2D EdgeB = Center + FVector2D(FMath::Cos(AngleB), FMath::Sin(AngleB)) * Radius;

        WorkingPolygon.Reset();
        if (OutPolygon.Num() < 3)
        {
            break;
        }

        FVector2D PreviousPoint = OutPolygon.Last();
        bool bPreviousInside = IsInsideClipEdge(PreviousPoint, EdgeA, EdgeB);

        for (const FVector2D& CurrentPoint : OutPolygon)
        {
            const bool bCurrentInside = IsInsideClipEdge(CurrentPoint, EdgeA, EdgeB);

            if (bCurrentInside)
            {
                if (!bPreviousInside)
                {
                    WorkingPolygon.Add(IntersectClipEdge(PreviousPoint, CurrentPoint, EdgeA, EdgeB));
                }
                WorkingPolygon.Add(CurrentPoint);
            }
            else if (bPreviousInside)
            {
                WorkingPolygon.Add(IntersectClipEdge(PreviousPoint, CurrentPoint, EdgeA, EdgeB));
            }

            PreviousPoint = CurrentPoint;
            bPreviousInside = bCurrentInside;
        }

        Swap(OutPolygon, WorkingPolygon);
    }

    if (OutPolygon.Num() < 3)
    {
        OutPolygon.Reset();
    }
}

FVector2D ToEgocentricMapPoint(
    const FVector2D& WorldPoint,
    const FVector2D& PlayerPosition,
    float PlayerYawDegrees,
    const FVector2D& Center,
    float PixelsPerUnit
)
{
    const FVector2D Relative = WorldPoint - PlayerPosition;
    const float YawRadians = FMath::DegreesToRadians(PlayerYawDegrees);
    const FVector2D Forward(FMath::Cos(YawRadians), FMath::Sin(YawRadians));
    const FVector2D Right(-Forward.Y, Forward.X);
    const float LocalForward = FVector2D::DotProduct(Relative, Forward);
    const float LocalRight = FVector2D::DotProduct(Relative, Right);
    return FVector2D(Center.X + LocalRight * PixelsPerUnit, Center.Y - LocalForward * PixelsPerUnit);
}
}

void AMiniWorldHud::DrawHUD()
{
    Super::DrawHUD();

    if (!Canvas)
    {
        return;
    }

    DrawRect(FLinearColor(0.015f, 0.016f, 0.018f, 1.0f), 0.0f, 0.0f, Canvas->SizeX, Canvas->SizeY);

    AMiniWorldSimulationActor* Simulation = FindSimulationActor();
    if (!Simulation)
    {
        return;
    }

    constexpr float LogicalPadding = 8.0f;
    constexpr float LogicalGap = 8.0f;
    constexpr float LogicalMiniMapWidth = 128.0f;
    constexpr float LogicalPanelHeight = 128.0f;
    constexpr float LogicalViewSize = 128.0f;
    constexpr float LogicalTotalWidth = LogicalPadding * 2.0f + LogicalMiniMapWidth + LogicalGap + LogicalViewSize;
    constexpr float LogicalTotalHeight = LogicalPadding * 2.0f + LogicalPanelHeight;

    const float FitScale = FMath::Min(Canvas->SizeX / LogicalTotalWidth, Canvas->SizeY / LogicalTotalHeight);
    const float Scale = FitScale >= 1.0f ? FMath::FloorToFloat(FitScale) : FMath::Max(0.25f, FitScale);
    const FVector2D Origin(
        (Canvas->SizeX - LogicalTotalWidth * Scale) * 0.5f,
        (Canvas->SizeY - LogicalTotalHeight * Scale) * 0.5f
    );

    const FVector2D MiniMapOrigin = Origin + FVector2D(LogicalPadding, LogicalPadding) * Scale;
    const FVector2D MiniMapSize(LogicalMiniMapWidth * Scale, LogicalPanelHeight * Scale);
    const FVector2D ViewOrigin = MiniMapOrigin + FVector2D(LogicalMiniMapWidth + LogicalGap, 0.0f) * Scale;
    const FVector2D ViewSize(LogicalViewSize * Scale, LogicalViewSize * Scale);

    DrawMiniMap(*Simulation, MiniMapOrigin, MiniMapSize);

    DrawRect(FLinearColor(0.035f, 0.037f, 0.04f, 1.0f), ViewOrigin.X, ViewOrigin.Y, ViewSize.X, ViewSize.Y);
    if (UTexture* ViewTexture = Simulation->GetDisplayViewTexture())
    {
        DrawTexture(
            ViewTexture,
            ViewOrigin.X,
            ViewOrigin.Y,
            ViewSize.X,
            ViewSize.Y,
            0.0f,
            0.0f,
            1.0f,
            1.0f,
            FLinearColor::White
        );
    }

    DrawPanelBorder(ViewOrigin, ViewSize, FLinearColor(0.28f, 0.31f, 0.34f, 1.0f));
}

AMiniWorldSimulationActor* AMiniWorldHud::FindSimulationActor() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    for (TActorIterator<AMiniWorldSimulationActor> It(World); It; ++It)
    {
        return *It;
    }

    return nullptr;
}

void AMiniWorldHud::DrawFilledTriangle(
    const FVector2D& A,
    const FVector2D& B,
    const FVector2D& C,
    const FLinearColor& Color
)
{
    if (!Canvas)
    {
        return;
    }

    FCanvasUVTri Triangle;
    Triangle.V0_Pos = A;
    Triangle.V1_Pos = B;
    Triangle.V2_Pos = C;
    Triangle.V0_UV = FVector2D::ZeroVector;
    Triangle.V1_UV = FVector2D::ZeroVector;
    Triangle.V2_UV = FVector2D::ZeroVector;
    Triangle.V0_Color = Color;
    Triangle.V1_Color = Color;
    Triangle.V2_Color = Color;

    TArray<FCanvasUVTri> Triangles;
    Triangles.Add(Triangle);
    Canvas->K2_DrawTriangle(nullptr, Triangles);
}

void AMiniWorldHud::DrawFilledQuad(
    const FVector2D& A,
    const FVector2D& B,
    const FVector2D& C,
    const FVector2D& D,
    const FLinearColor& Color
)
{
    if (!Canvas)
    {
        return;
    }

    FCanvasUVTri Triangle0;
    Triangle0.V0_Pos = A;
    Triangle0.V1_Pos = B;
    Triangle0.V2_Pos = C;
    Triangle0.V0_UV = FVector2D::ZeroVector;
    Triangle0.V1_UV = FVector2D::ZeroVector;
    Triangle0.V2_UV = FVector2D::ZeroVector;
    Triangle0.V0_Color = Color;
    Triangle0.V1_Color = Color;
    Triangle0.V2_Color = Color;

    FCanvasUVTri Triangle1;
    Triangle1.V0_Pos = A;
    Triangle1.V1_Pos = C;
    Triangle1.V2_Pos = D;
    Triangle1.V0_UV = FVector2D::ZeroVector;
    Triangle1.V1_UV = FVector2D::ZeroVector;
    Triangle1.V2_UV = FVector2D::ZeroVector;
    Triangle1.V0_Color = Color;
    Triangle1.V1_Color = Color;
    Triangle1.V2_Color = Color;

    TArray<FCanvasUVTri> Triangles;
    Triangles.Reserve(2);
    Triangles.Add(Triangle0);
    Triangles.Add(Triangle1);
    Canvas->K2_DrawTriangle(nullptr, Triangles);
}

void AMiniWorldHud::DrawFilledPolygon(const TArray<FVector2D>& Points, const FLinearColor& Color)
{
    if (!Canvas || Points.Num() < 3)
    {
        return;
    }

    TArray<FCanvasUVTri> Triangles;
    Triangles.Reserve(Points.Num() - 2);

    for (int32 Index = 1; Index < Points.Num() - 1; ++Index)
    {
        FCanvasUVTri Triangle;
        Triangle.V0_Pos = Points[0];
        Triangle.V1_Pos = Points[Index];
        Triangle.V2_Pos = Points[Index + 1];
        Triangle.V0_UV = FVector2D::ZeroVector;
        Triangle.V1_UV = FVector2D::ZeroVector;
        Triangle.V2_UV = FVector2D::ZeroVector;
        Triangle.V0_Color = Color;
        Triangle.V1_Color = Color;
        Triangle.V2_Color = Color;
        Triangles.Add(Triangle);
    }

    Canvas->K2_DrawTriangle(nullptr, Triangles);
}

void AMiniWorldHud::DrawMiniMap(const AMiniWorldSimulationActor& Simulation, const FVector2D& Origin, const FVector2D& Size)
{
    const FLinearColor PanelColor(0.034f, 0.038f, 0.04f, 1.0f);
    DrawRect(PanelColor, Origin.X, Origin.Y, Size.X, Size.Y);

    const FVector2D Center = Origin + Size * 0.5f;
    const float CircleRadius = FMath::Min(Size.X, Size.Y) * 0.5f;
    const float PixelsPerUnit = CircleRadius / FMath::Max(Simulation.GetViewDistance(), 1.0f);
    constexpr int32 CircleSegments = 192;
    const float ClipRadius = FMath::Max(CircleRadius - 0.5f, 0.0f);

    Canvas->K2_DrawPolygon(
        nullptr,
        Center,
        FVector2D(CircleRadius, CircleRadius),
        CircleSegments,
        FLinearColor(0.07f, 0.08f, 0.075f, 1.0f)
    );

    for (const FMiniWorldLine& Line : Simulation.GetLines())
    {
        const FVector2D WallDelta = Line.End - Line.Start;
        const float WallLength = WallDelta.Size();
        if (WallLength <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        const FVector2D WallDirection = WallDelta / WallLength;
        const FVector2D WallNormal(-WallDirection.Y, WallDirection.X);
        const float HalfThickness = FMath::Max(Line.Thickness, 1.0f) * 0.5f;

        const FVector2D WorldA = Line.Start + WallNormal * HalfThickness;
        const FVector2D WorldB = Line.End + WallNormal * HalfThickness;
        const FVector2D WorldC = Line.End - WallNormal * HalfThickness;
        const FVector2D WorldD = Line.Start - WallNormal * HalfThickness;

        const FVector2D A = ToEgocentricMapPoint(
            WorldA,
            Simulation.GetPlayerPosition(),
            Simulation.GetPlayerYawDegrees(),
            Center,
            PixelsPerUnit
        );
        const FVector2D B = ToEgocentricMapPoint(
            WorldB,
            Simulation.GetPlayerPosition(),
            Simulation.GetPlayerYawDegrees(),
            Center,
            PixelsPerUnit
        );
        const FVector2D C = ToEgocentricMapPoint(
            WorldC,
            Simulation.GetPlayerPosition(),
            Simulation.GetPlayerYawDegrees(),
            Center,
            PixelsPerUnit
        );
        const FVector2D D = ToEgocentricMapPoint(
            WorldD,
            Simulation.GetPlayerPosition(),
            Simulation.GetPlayerYawDegrees(),
            Center,
            PixelsPerUnit
        );

        TArray<FVector2D> WallPolygon;
        WallPolygon.Reserve(4);
        WallPolygon.Add(A);
        WallPolygon.Add(B);
        WallPolygon.Add(C);
        WallPolygon.Add(D);

        TArray<FVector2D> ClippedPolygon;
        ClippedPolygon.Reserve(CircleSegments);
        ClipPolygonToCircle(WallPolygon, Center, ClipRadius, CircleSegments, ClippedPolygon);
        DrawFilledPolygon(ClippedPolygon, Line.Color);
    }

    const FLinearColor PlayerColor = Simulation.HadCollisionLastFrame()
        ? FLinearColor(1.0f, 0.18f, 0.14f, 1.0f)
        : FLinearColor(0.96f, 0.98f, 1.0f, 1.0f);

    const float ArrowLength = FMath::Max(12.0f, Size.Y * 0.105f);
    const float ArrowWing = ArrowLength * 0.55f;
    const FVector2D Nose(Center.X, Center.Y - ArrowLength);
    const FVector2D Left(Center.X - ArrowWing, Center.Y + ArrowWing * 0.45f);
    const FVector2D Right(Center.X + ArrowWing, Center.Y + ArrowWing * 0.45f);

    Canvas->K2_DrawLine(Nose, Left, 2.0f, PlayerColor);
    Canvas->K2_DrawLine(Nose, Right, 2.0f, PlayerColor);
    Canvas->K2_DrawLine(Left, Right, 2.0f, PlayerColor);

    DrawCircleOutline(Center, CircleRadius, 1.5f, FLinearColor(0.28f, 0.31f, 0.34f, 1.0f));
    DrawPanelBorder(Origin, Size, FLinearColor(0.28f, 0.31f, 0.34f, 1.0f));
}

void AMiniWorldHud::DrawPanelBorder(const FVector2D& Origin, const FVector2D& Size, const FLinearColor& Color)
{
    const FVector2D TopLeft = Origin;
    const FVector2D TopRight(Origin.X + Size.X, Origin.Y);
    const FVector2D BottomRight = Origin + Size;
    const FVector2D BottomLeft(Origin.X, Origin.Y + Size.Y);

    Canvas->K2_DrawLine(TopLeft, TopRight, 1.0f, Color);
    Canvas->K2_DrawLine(TopRight, BottomRight, 1.0f, Color);
    Canvas->K2_DrawLine(BottomRight, BottomLeft, 1.0f, Color);
    Canvas->K2_DrawLine(BottomLeft, TopLeft, 1.0f, Color);
}

void AMiniWorldHud::DrawCircleOutline(const FVector2D& Center, float Radius, float Thickness, const FLinearColor& Color, int32 Segments)
{
    const int32 ClampedSegments = FMath::Max(Segments, 8);
    FVector2D Previous = Center + FVector2D(Radius, 0.0f);
    for (int32 Index = 1; Index <= ClampedSegments; ++Index)
    {
        const float Angle = (TWO_PI * Index) / static_cast<float>(ClampedSegments);
        const FVector2D Current = Center + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius;
        Canvas->K2_DrawLine(Previous, Current, Thickness, Color);
        Previous = Current;
    }
}
