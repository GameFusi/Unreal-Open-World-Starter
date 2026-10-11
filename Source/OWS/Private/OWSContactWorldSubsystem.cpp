#include "OWSContactWorldSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "OWSCharacterContactComponent.h"
#include "OWSStockVehicleInteractionComponent.h"
#include "OWSVehicleContactComponent.h"

bool UOWSContactWorldSubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{
    return Type == EWorldType::Game || Type == EWorldType::PIE;
}

void UOWSContactWorldSubsystem::OnWorldBeginPlay(UWorld& World)
{
    Super::OnWorldBeginPlay(World);
    for (TActorIterator<AActor> It(&World); It; ++It) Pending.Add(*It);
    SpawnHandle = World.AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateUObject(
        this, &ThisClass::ConsiderActor));
}

void UOWSContactWorldSubsystem::ConsiderActor(AActor* Actor)
{
    if (IsValid(Actor)) Pending.AddUnique(Actor);
}

void UOWSContactWorldSubsystem::Tick(float)
{
    for (auto It = Pending.CreateIterator(); It; ++It)
    {
        AActor* Actor = It->Get();
        if (!IsValid(Actor)) { It.RemoveCurrent(); continue; }
        if (!Actor->HasActorBegunPlay()) continue;
        if (Actor->FindComponentByClass<UOWSStockVehicleInteractionComponent>())
        {
            auto* Contact = Actor->FindComponentByClass<UOWSVehicleContactComponent>();
            if (!Contact)
            {
                Contact = NewObject<UOWSVehicleContactComponent>(Actor, NAME_None, RF_Transient);
                Actor->AddInstanceComponent(Contact);
                Contact->RegisterComponent();
            }
            Vehicles.AddUnique(Contact);
        }
        if (ACharacter* Character = Cast<ACharacter>(Actor))
        {
            bool bOWSCharacter = false;
            for (UClass* Class = Character->GetClass(); Class; Class = Class->GetSuperClass())
                bOWSCharacter |= Class->GetPathName().StartsWith(TEXT("/Game/OWS/Characters/"));
            if (bOWSCharacter && !Character->FindComponentByClass<UOWSCharacterContactComponent>())
            {
                auto* Contact = NewObject<UOWSCharacterContactComponent>(Actor, NAME_None, RF_Transient);
                Actor->AddInstanceComponent(Contact);
                Contact->RegisterComponent();
            }
        }
        It.RemoveCurrent();
    }
    Vehicles.RemoveAll([](const auto& Entry) { return !Entry.IsValid(); });
}

TStatId UOWSContactWorldSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UOWSContactWorldSubsystem, STATGROUP_Tickables);
}

void UOWSContactWorldSubsystem::Deinitialize()
{
    if (GetWorld() && SpawnHandle.IsValid()) GetWorld()->RemoveOnActorSpawnedHandler(SpawnHandle);
    SpawnHandle.Reset(); Pending.Reset(); Vehicles.Reset();
    Super::Deinitialize();
}
