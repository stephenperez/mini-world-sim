#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "MiniWorldPlayerController.generated.h"

UCLASS()
class MINIWORLDSIM_API AMiniWorldPlayerController : public APlayerController
{
    GENERATED_BODY()

protected:
    virtual void BeginPlay() override;
    virtual void PlayerTick(float DeltaTime) override;

private:
    void CaptureMouseForGame();
    void ReleaseMouseToDesktop();

    bool bMouseReleased = false;
    bool bWasEscapeDown = false;
    bool bWasLeftMouseDown = false;
};
