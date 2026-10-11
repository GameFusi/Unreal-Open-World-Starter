#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "OWSVehicleContactComponent.generated.h"

class UPrimitiveComponent;

/** Query hulls block locomotion without treating an upright capsule as an infinite-mass physics body. */
UCLASS(ClassGroup=(OWS), meta=(BlueprintSpawnableComponent))
class OWS_API UOWSVehicleContactComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UOWSVehicleContactComponent();
    UPrimitiveComponent* GetPrimaryBody() const;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick Type, FActorComponentTickFunction* Tick) override;
private:
    struct FProxy
    {
        TWeakObjectPtr<UPrimitiveComponent> Body;
        TWeakObjectPtr<UPrimitiveComponent> Query;
        ECollisionResponse PreviousPawnResponse = ECR_Block;
        FVector PreviousLocation = FVector::ZeroVector;
    };
    TArray<FProxy> Bodies;
};
