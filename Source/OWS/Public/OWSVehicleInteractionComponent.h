#pragma once

#include "Components/ActorComponent.h"

#include "OWSVehicleInteractionComponent.generated.h"

class ACharacter;
class APawn;
class APlayerController;
class UOWSSelectorComponent;
class UOWSStockVehicleInteractionComponent;
class UOWSInteractionTargetComponent;
class UOWSControllerHotbarComponent;
class UAnimInstance;
class UAnimSequenceBase;
class UCharacterMovementComponent;
class USkeletalMeshComponent;

/**
 * OWS-owned controller component for moving between an on-foot OWS character
 * and compatible vehicles. It survives possession changes because it belongs
 * to the PlayerController rather than either pawn.
 */
UCLASS(ClassGroup=(OWS), meta=(BlueprintSpawnableComponent))
class OWS_API UOWSVehicleInteractionComponent final
	: public UActorComponent
{
	GENERATED_BODY()

public:
	UOWSVehicleInteractionComponent();

	/** Debug helper: enter the nearest faced vehicle (bypasses the key press). */
	UFUNCTION(BlueprintCallable, Category="OWS|Vehicle|Debug")
	bool DebugEnterNearestVehicle();

	/** Debug helper: exit the current vehicle (bypasses the key press). */
	UFUNCTION(BlueprintCallable, Category="OWS|Vehicle|Debug")
	bool DebugExitVehicle();

	/** Exact interaction-point entry used by the shared selector contract. */
	bool CanEnterVehicleThroughDoor(
		APawn* RequestedVehicle,
		FName RequestedDoorId,
		FText& OutFailureReason) const;
	bool TryEnterVehicleThroughDoor(
		APawn* RequestedVehicle,
		FName RequestedDoorId,
		FText& OutFailureReason);
	/** Contact responses must not take ownership of the verified bailout motion. */
	bool IsCharacterInBailout(const ACharacter* Character) const
	{
		return (bControlledBailoutActive && ControlledBailoutCharacter == Character) ||
			(bRagdollActive && RagdollCharacter == Character);
	}

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	friend struct FOWSMissingRollFixture;
	// Polled every frame from TickComponent. Reading raw key state via
	// IsInputKeyDown is immune to Enhanced Input consuming the key, which the
	// previous BindKey approach was not.
	void UpdateInteraction(float DeltaTime);
	void RefreshSelectorBinding();
	void RefreshCancelBinding();
	void SetVehicleCancelContextActive(bool bActive);

	UFUNCTION()
	void HandleCancelRequested();

	UFUNCTION()
	void HandleCancelReleased();

	void ShowPrompt(const FString& Text) const;
	bool TryEnterVehicle(
		APawn* RequestedVehicle = nullptr,
		FName RequestedDoorId = NAME_None);
	bool TryExitVehicle();
	void EjectCharacterWithPhysics(
		ACharacter& Character,
		const FVector& VehicleVelocity,
		const FVector& DoorDirection);
	void BeginControlledBailout(
		ACharacter& Character,
		const FVector& VehicleVelocity,
		const FVector& DoorDirection);
	void UpdateControlledBailout(float DeltaTime);
	void FinishControlledBailout();
	float CalculateControlledRollContinuousDeceleration(
		const UCharacterMovementComponent& Movement,
		float SpeedCmPerSecond) const;
	void StartVehicleExitTrace(ACharacter& Character, APawn& Vehicle);
	void UpdateVehicleExitTrace(float DeltaTime);
	/** Ticks the bail-out ragdoll and stands the character back up once settled. */
	void UpdateRagdollRecovery(float DeltaTime);
	void RecoverFromRagdoll();
	/** Moves mesh-parented camera components onto the capsule and back again. */
	void ParkCameraRigsOnCapsule(ACharacter& Character);
	void RestoreCameraRigs(ACharacter& Character);
	APawn* FindEnterableVehicle(
		ACharacter& Character,
		UOWSStockVehicleInteractionComponent*& OutInteraction,
		FName& OutDoorId,
		FName& OutSeatId,
		const APawn* RequiredVehicle = nullptr,
		FName RequiredDoorId = NAME_None) const;
	bool PlaceCharacterAtDoorExit(
		ACharacter& Character,
		const APawn& Vehicle,
		const UOWSStockVehicleInteractionComponent& Interaction,
		FName DoorId) const;
	bool PlaceCharacterAtSafeDoorExit(
		ACharacter& Character,
		const APawn& Vehicle,
		const UOWSStockVehicleInteractionComponent& Interaction) const;
	FVector GetVehicleVelocity() const;
	float GetVehicleSpeedMph() const;
	void ApplyVehicleInputContext(bool bVehicleActive) const;
	/**
	 * Points the camera at the given actor. Possession alone does not move the
	 * view target here (the OWS controller keeps it on itself), which leaves the
	 * vehicle camera unused and the view stuck at the car's origin.
	 */
	void SetCameraTarget(AActor* Target, float BlendSeconds) const;

	UPROPERTY(Transient)
	TObjectPtr<ACharacter> HomeCharacter = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<APawn> OccupiedVehicle = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UOWSSelectorComponent> BoundSelector = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UOWSControllerHotbarComponent> BoundHotbar = nullptr;

	UPROPERTY(Transient)
	FName OccupiedSeatId = NAME_None;

	/** Door used for the current entry and therefore preferred for stopped exit. */
	UPROPERTY(Transient)
	FName OccupiedDoorId = NAME_None;

	/**
	 * Vehicle the player bailed out of. Re-entry is refused until they have
	 * walked back out of range, so a bail-out cannot be instantly undone.
	 */
	UPROPERTY(Transient)
	TObjectPtr<APawn> ReentryBlockedVehicle = nullptr;

	/** Character position at bailout; the lock clears only after the player leaves it. */
	FVector ReentryBlockOrigin = FVector::ZeroVector;

	bool bCancelHeld = false;
	bool bVehicleCancelContextActive = false;
	bool bPreviousCancelContextActive = false;

	/** Seconds the exit key has been held while the vehicle is moving. */
	float ExitHoldElapsed = 0.0f;

	/** Character currently ragdolling after a bail-out (outlives HomeCharacter). */
	UPROPERTY(Transient)
	TObjectPtr<ACharacter> RagdollCharacter = nullptr;

	/** True while the character is ragdolling after a bail-out. */
	bool bRagdollActive = false;

	/** Seconds since the bail-out ragdoll started. */
	float RagdollElapsed = 0.0f;

	/** Character playing a non-ragdoll controlled roll after a moving exit. */
	UPROPERTY(Transient)
	TObjectPtr<ACharacter> ControlledBailoutCharacter = nullptr;

	bool bControlledBailoutActive = false;
	bool bControlledBailoutAddedMoveIgnore = false;
	bool bControlledBailoutUsesSingleNode = false;
	bool bControlledBailoutOwnsMeshAnimation = false;
	float ControlledBailoutElapsed = 0.0f;
	float CachedRollBrakingDeceleration = 0.0f;
	float CachedRollGroundFriction = 0.0f;
	float ControlledRollTargetSpeed = 0.0f;
	double ControlledRollKineticEnergyJoules = 0.0;
	float ControlledRollCycleDistanceCm = 0.0f;
	float ControlledRollDistanceIntoCycleCm = 0.0f;
	FVector ControlledRollPreviousLocation = FVector::ZeroVector;
	int32 ControlledRollContactCount = 0;
	double ControlledRollIntervalSlidingWorkJoules = 0.0;
	double ControlledRollIntervalAerodynamicWorkJoules = 0.0;
	double ControlledRollIntervalImpactLossJoules = 0.0;
	int32 ControlledRollIntervalContacts = 0;
	bool bControlledRollTickAfterMovement = false;
	float BailoutTelemetryAccumulator = 0.0f;
	FVector BailoutStartLocation = FVector::ZeroVector;
	FVector BailoutPreviousLocation = FVector::ZeroVector;
	FVector BailoutPreviousObservedVelocity = FVector::ZeroVector;

	UPROPERTY(Transient)
	TObjectPtr<APawn> ControlledBailoutSourceVehicle = nullptr;

	bool bVehicleExitTraceActive = false;
	float VehicleExitTraceElapsed = 0.0f;
	float VehicleExitTraceAccumulator = 0.0f;
	FVector VehicleExitTracePreviousLocation = FVector::ZeroVector;
	FVector VehicleExitTracePreviousVelocity = FVector::ZeroVector;

	UPROPERTY(Transient)
	TObjectPtr<ACharacter> VehicleExitTraceCharacter = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<APawn> VehicleExitTraceVehicle = nullptr;
	uint8 CachedControlledAnimationMode = 0;

	UPROPERTY(Transient)
	TSubclassOf<UAnimInstance> CachedControlledAnimClass;

	/** Transient root-locked copy used so a roll cannot separate from its capsule. */
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequenceBase> ActiveControlledAnimation = nullptr;

	/** Mesh state captured before ragdolling so it can be restored on recovery. */
	FTransform CachedMeshRelativeTransform = FTransform::Identity;
	FName CachedMeshCollisionProfile = NAME_None;

	/**
	 * Camera components normally parented to the skeletal mesh. They are moved
	 * onto the capsule for the duration of a ragdoll, because a camera bolted to
	 * the mesh tumbles with the body instead of framing it.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> ParkedCameraRigs;
	TArray<FTransform> ParkedCameraRigTransforms;

	UPROPERTY(EditAnywhere, Category="OWS|Vehicle", meta=(ClampMin="0.0"))
	float MaximumEnterDistance = 500.0f;

	UPROPERTY(EditAnywhere, Category="OWS|Vehicle", meta=(ClampMin="0.0"))
	float ImmediateExitSpeedMph = 5.0f;

	UPROPERTY(EditAnywhere, Category="OWS|Vehicle", meta=(ClampMin="0.1"))
	float MovingExitHoldSeconds = 2.0f;

	/** Speeds above this threshold use the controlled roll. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="5.0"))
	float ControlledRollMinimumSpeedMph = 15.0f;

	/** First grounded rolling frame inside the source land-and-roll animation. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut|Animation", meta=(ClampMin="0.0"))
	float ControlledRollClipStartSeconds = 1.1f;

	/** Last grounded rolling frame before the source animation resumes locomotion. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut|Animation", meta=(ClampMin="0.0"))
	float ControlledRollClipEndSeconds = 1.63f;

	/** Maximum trajectory correction at full left/right stick while rolling. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float ControlledRollSteeringDegreesPerSecond = 15.0f;

	/** Grounded horizontal speed at which a roll may transition back to the feet. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float ControlledRollStandSpeed = 500.0f;

	/** Measured dry-pavement body sliding coefficient used between contacts. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float ControlledRollSlidingFrictionCoefficient = 0.6f;

	/**
	 * Fraction of translational kinetic energy dissipated by each completed body
	 * rotation. Fifteen percent preserves the previously authorized 1.4-g
	 * baseline distance while keeping impact loss distinct from surface friction.
	 */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ControlledRollImpactEnergyLossFraction = 0.15f;

	/** Sea-level air density used by the quadratic drag equation. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float ControlledRollAirDensityKgPerCubicMeter = 1.225f;

	/** Approximate drag coefficient of a tumbling human body. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float ControlledRollAerodynamicDragCoefficient = 1.0f;

	/** Approximate projected frontal area of an adult human body. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float ControlledRollFrontalAreaSquareMeters = 0.465f;

	/** Logs the complete controlled-bailout motion trace while issue #25 is tuned. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut|Debug")
	bool bLogBailoutTelemetry = true;

	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut|Animation")
	TSoftObjectPtr<UAnimSequenceBase> RollRightAnimation;

	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut|Animation")
	TSoftObjectPtr<UAnimSequenceBase> RollLeftAnimation;

	/**
	 * Legacy physics-ragdoll upward velocity. Controlled player bailouts do not
	 * use an artificial upward kick.
	 */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle", meta=(ClampMin="0.0"))
	float EjectionUpwardBoost = 0.0f;

	/** Seconds to blend the camera when moving between character and vehicle. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|Camera", meta=(ClampMin="0.0"))
	float CameraBlendSeconds = 0.4f;

	/** Legacy physics-ragdoll side push. Controlled player bailouts do not use it. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float EjectionSidePush = 0.0f;

	/** How far outside the door the body starts, so it never spawns in the car. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float BailOutDoorClearance = 110.0f;

	/** Earliest the ragdoll may stand back up after a bail-out. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float RagdollMinimumSeconds = 1.5f;

	/** Hard cap on ragdoll time, even if the body never settles. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.1"))
	float RagdollMaximumSeconds = 6.0f;

	/** Body speed (cm/s) below which the ragdoll counts as settled. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float RagdollSettleSpeed = 150.0f;

	/** Distance the player must retreat before a bailed-out car accepts them. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="0.0"))
	float ReentryReleaseDistance = 550.0f;

	/**
	 * Top speed (cm/s) the camera anchor may travel while chasing the tumbling
	 * body. The gameplay camera rig is tuned for locomotion, so dragging the
	 * anchor at the car's speed makes it lag and collide; capping it near a
	 * sprint keeps the framing identical to running around on foot.
	 */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="1.0"))
	float RagdollAnchorMaxSpeed = 700.0f;

	/** How close the anchor must get to the body before standing back up. */
	UPROPERTY(EditAnywhere, Category="OWS|Vehicle|BailOut", meta=(ClampMin="1.0"))
	float RagdollAnchorCatchUpDistance = 200.0f;
};
