#include "MiniWorldPlayerController.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"

void AMiniWorldPlayerController::BeginPlay()
{
    Super::BeginPlay();

    CaptureMouseForGame();
}

void AMiniWorldPlayerController::PlayerTick(float DeltaTime)
{
    Super::PlayerTick(DeltaTime);

    const bool bEscapeDown = IsInputKeyDown(EKeys::Escape);
    if (bEscapeDown && !bWasEscapeDown)
    {
        ReleaseMouseToDesktop();
    }
    bWasEscapeDown = bEscapeDown;

    const bool bLeftMouseDown = IsInputKeyDown(EKeys::LeftMouseButton);
    if (bMouseReleased && bLeftMouseDown && !bWasLeftMouseDown)
    {
        CaptureMouseForGame();
    }
    bWasLeftMouseDown = bLeftMouseDown;
}

void AMiniWorldPlayerController::CaptureMouseForGame()
{
    bMouseReleased = false;
    bShowMouseCursor = false;

    FInputModeGameOnly InputMode;
    InputMode.SetConsumeCaptureMouseDown(false);
    SetInputMode(InputMode);

    if (UGameViewportClient* ViewportClient = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
    {
        ViewportClient->SetMouseCaptureMode(EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown);
        ViewportClient->SetMouseLockMode(EMouseLockMode::LockInFullscreen);
        ViewportClient->SetHideCursorDuringCapture(true);
    }
}

void AMiniWorldPlayerController::ReleaseMouseToDesktop()
{
    bMouseReleased = true;
    bShowMouseCursor = true;

    FInputModeGameAndUI InputMode;
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    InputMode.SetHideCursorDuringCapture(false);
    SetInputMode(InputMode);

    if (UGameViewportClient* ViewportClient = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
    {
        ViewportClient->SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
        ViewportClient->SetMouseLockMode(EMouseLockMode::DoNotLock);
        ViewportClient->SetHideCursorDuringCapture(false);
    }
}
