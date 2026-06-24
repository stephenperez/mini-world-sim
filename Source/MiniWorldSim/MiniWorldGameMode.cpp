#include "MiniWorldGameMode.h"

#include "Engine/World.h"
#include "MiniWorldHud.h"
#include "MiniWorldPlayerController.h"
#include "MiniWorldSimulationActor.h"

AMiniWorldGameMode::AMiniWorldGameMode()
{
    DefaultPawnClass = nullptr;
    HUDClass = AMiniWorldHud::StaticClass();
    PlayerControllerClass = AMiniWorldPlayerController::StaticClass();
}

void AMiniWorldGameMode::BeginPlay()
{
    Super::BeginPlay();

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    GetWorld()->SpawnActor<AMiniWorldSimulationActor>(
        AMiniWorldSimulationActor::StaticClass(),
        FVector::ZeroVector,
        FRotator::ZeroRotator,
        SpawnParams
    );
}
