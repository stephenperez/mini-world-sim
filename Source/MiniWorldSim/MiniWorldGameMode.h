#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "MiniWorldGameMode.generated.h"

UCLASS()
class MINIWORLDSIM_API AMiniWorldGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    AMiniWorldGameMode();

protected:
    virtual void BeginPlay() override;
};
