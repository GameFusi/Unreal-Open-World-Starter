#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "OWSCharacterContactComponent.generated.h"

class ACharacter;
class UAnimMontage;
class UPhysicalAnimationComponent;
class UPrimitiveComponent;

UENUM(BlueprintType)
enum class EOWSCharacterContactState : uint8 { Upright, Fallen, Pinned, Recovering };

/** Bodily contact and immediate safety reflexes, shared by player and NPC characters. */
UCLASS(ClassGroup=(OWS), meta=(BlueprintSpawnableComponent))
class OWS_API UOWSCharacterContactComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UOWSCharacterContactComponent();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="OWS|Contact")
    EOWSCharacterContactState State = EOWSCharacterContactState::Upright;
    /** Aurora-approved initial tuning, not a universal human injury threshold. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact", meta=(ClampMin="0.0", Units="cm/s"))
    float KnockdownClosingSpeed = 312.928f; // 7 mph
    /** Sustained contact travel, expressed in character capsule radii. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact", meta=(ClampMin="0.1"))
    float SustainedPushRadiusMultiplier = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact", meta=(ClampMin="0.0", Units="cm/s"))
    float RecoveryLinearSpeed = 60.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact", meta=(ClampMin="0.0"))
    float RecoveryAngularSpeedDegrees = 45.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact")
    FName PelvisBody = TEXT("pelvis");
    /** Local limb motors only. Never pull the pelvis toward an upright world-space target. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Physics", meta=(ClampMin="0.0"))
    float LimbOrientationStrength = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Physics", meta=(ClampMin="0.0"))
    float LimbAngularDamping = 10.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Physics", meta=(ClampMin="0.0", Units="cm/s"))
    float InitialOverlapDepenetrationSpeed = 200.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Recovery")
    TSoftObjectPtr<UAnimMontage> GetUpFront;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Recovery")
    TSoftObjectPtr<UAnimMontage> GetUpBack;
    /** Player input is NEVER replaced by this reflex. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Awareness")
    bool bNpcEvasion = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Awareness", meta=(ClampMin="0.0", Units="cm"))
    float HearingRadius = 2000.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Awareness", meta=(ClampMin="0.01", Units="s"))
    float AwarenessInterval = .1f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Awareness", meta=(ClampMin="0.0", Units="s"))
    float ReactionSeconds = .25f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Awareness", meta=(ClampMin="0.01", Units="s"))
    float ThreatHorizonSeconds = 2.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Debug")
    bool bLogTransitions = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="OWS|Contact|Debug", meta=(ClampMin="0.01", Units="s"))
    float ContactSampleInterval = .1f;

    void ReceiveVehicleContact(UPrimitiveComponent& Body, const FHitResult& Hit, float DeltaTime);
    bool IsUpright() const { return State == EOWSCharacterContactState::Upright; }
    bool IsContactAvailable() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick Type, FActorComponentTickFunction* Tick) override;

private:
    friend struct FOWSContactRecoveryFixture;
    UFUNCTION()
    void OnCapsuleHit(UPrimitiveComponent* Self, AActor* Other, UPrimitiveComponent* OtherComponent,
        FVector NormalImpulse, const FHitResult& Hit);
    bool BeginFall();
    void PushFrom(UPrimitiveComponent* VehicleBody, ACharacter* OtherCharacter,
        const FVector& Point, const FVector& Direction, float DeltaTime, float PenetrationDepth);
    void UpdateRecovery();
    bool RecoverySpace(FVector& OutCapsuleLocation, bool& OutSupported) const;
    void RestoreMesh();
    void SetState(EOWSCharacterContactState Next);
    void UpdateAwareness(float DeltaTime);
    bool CanSeeVehicle(const AActor& Vehicle) const;
    bool CanHearVehicle(const AActor& Vehicle) const;
    bool FindEscape(const AActor& Vehicle, FVector& OutDestination) const;
    void OrderMovementReflex();

    UPROPERTY(Transient) TObjectPtr<ACharacter> Character;
    UPROPERTY(Transient) TObjectPtr<UPhysicalAnimationComponent> PhysicalAnimation;
    UPROPERTY(Transient) TObjectPtr<UAnimMontage> RecoveryMontage;
    TWeakObjectPtr<AActor> Threat;
    TWeakObjectPtr<AActor> PressureSource;
    TWeakObjectPtr<USceneComponent> MeshParent;
    FName MeshSocket;
    FTransform MeshRelativeTransform;
    FCollisionResponseContainer MeshResponses;
    FName MeshProfile;
    ECollisionChannel MeshObjectType = ECC_Pawn;
    ECollisionEnabled::Type MeshCollision = ECollisionEnabled::NoCollision;
    ECollisionEnabled::Type CapsuleCollision = ECollisionEnabled::QueryAndPhysics;
    bool bSavedPauseAnims = false;
    float PressureDistance = 0.f;
    double LastPressureAt = -1.;
    float AwarenessElapsed = 0.f;
    float ThreatAge = 0.f;
    double NextSampleAt = 0.;
    bool bEscaping = false;
    FVector EscapeDestination = FVector::ZeroVector;
    TArray<TWeakObjectPtr<USceneComponent>> ParkedCameras;
    TArray<FTransform> CameraTransforms;
    struct FBodySettings
    {
        FName Bone;
        float AngularDamping = 0.f;
        float DepenetrationSpeed = 0.f;
        bool bOverrideDepenetration = false;
        bool bCCD = false;
    };
    TArray<FBodySettings> SavedBodies;
};
