#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "OWSContactWorldSubsystem.generated.h"

class UOWSVehicleContactComponent;

/** Installs the shared contact contract on OWS characters and stock-compatible vehicles. */
UCLASS()
class OWS_API UOWSContactWorldSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void OnWorldBeginPlay(UWorld& World) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    const TArray<TWeakObjectPtr<UOWSVehicleContactComponent>>& GetVehicles() const { return Vehicles; }
protected:
    virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
private:
    void ConsiderActor(AActor* Actor);
    FDelegateHandle SpawnHandle;
    TArray<TWeakObjectPtr<AActor>> Pending;
    TArray<TWeakObjectPtr<UOWSVehicleContactComponent>> Vehicles;
};
