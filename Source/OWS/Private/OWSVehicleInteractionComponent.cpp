#include "OWSVehicleInteractionComponent.h"
#include "OWSCharacterContactComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/LocalPlayer.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "OWSControllerHotbarComponent.h"
#include "OWSInteractionTargetComponent.h"
#include "OWSSelectorComponent.h"
#include "OWSStockVehicleInteractionComponent.h"

namespace
{
	// Stable on-screen message key so the prompt refreshes in place each frame.
	constexpr uint64 GVehiclePromptKey = 715120412ull;

	UAnimSequenceBase* MakeRootLockedRuntimeSequence(
		UAnimSequenceBase* Source,
		UObject* Outer)
	{
		if (const UAnimSequence* SourceSequence = Cast<UAnimSequence>(Source))
		{
			UAnimSequence* RootLockedSequence = DuplicateObject<UAnimSequence>(
				SourceSequence, Outer);
			RootLockedSequence->bEnableRootMotion = false;
			RootLockedSequence->bForceRootLock = true;
			return RootLockedSequence;
		}
		return Source;
	}
}

UOWSVehicleInteractionComponent::UOWSVehicleInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	RollRightAnimation = TSoftObjectPtr<UAnimSequenceBase>(FSoftObjectPath(
		TEXT("/GASPALS/Characters/UEFN_Mannequin/Animations/Jump/M_Neutral_Jump_F_Land_Roll_Rfoot.M_Neutral_Jump_F_Land_Roll_Rfoot")));
	RollLeftAnimation = TSoftObjectPtr<UAnimSequenceBase>(FSoftObjectPath(
		TEXT("/GASPALS/Characters/UEFN_Mannequin/Animations/Jump/M_Neutral_Jump_F_Land_Roll_Lfoot.M_Neutral_Jump_F_Land_Roll_Lfoot")));
}

void UOWSVehicleInteractionComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UOWSVehicleInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (BoundSelector)
	{
		BoundSelector = nullptr;
	}
	SetVehicleCancelContextActive(false);
	if (BoundHotbar)
	{
		BoundHotbar->OnCancelRequested.RemoveDynamic(
			this, &UOWSVehicleInteractionComponent::HandleCancelRequested);
		BoundHotbar->OnCancelReleased.RemoveDynamic(
			this, &UOWSVehicleInteractionComponent::HandleCancelReleased);
		BoundHotbar = nullptr;
	}
	FinishControlledBailout();
	if (GEngine)
	{
		GEngine->RemoveOnScreenDebugMessage(GVehiclePromptKey);
	}
	Super::EndPlay(EndPlayReason);
}

void UOWSVehicleInteractionComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	RefreshSelectorBinding();
	RefreshCancelBinding();
	UpdateInteraction(DeltaTime);
}

void UOWSVehicleInteractionComponent::RefreshSelectorBinding()
{
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	const ACharacter* Character = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
	UOWSSelectorComponent* DesiredSelector = Character
		? Character->FindComponentByClass<UOWSSelectorComponent>()
		: BoundSelector.Get();
	if (DesiredSelector == BoundSelector)
	{
		return;
	}
	BoundSelector = DesiredSelector;
}

void UOWSVehicleInteractionComponent::RefreshCancelBinding()
{
	UOWSControllerHotbarComponent* DesiredHotbar = GetOwner()
		? GetOwner()->FindComponentByClass<UOWSControllerHotbarComponent>()
		: nullptr;
	if (DesiredHotbar == BoundHotbar)
	{
		return;
	}
	if (BoundHotbar)
	{
		if (bVehicleCancelContextActive)
		{
			BoundHotbar->SetCancelContextActive(bPreviousCancelContextActive);
			bVehicleCancelContextActive = false;
			bPreviousCancelContextActive = false;
		}
		BoundHotbar->OnCancelRequested.RemoveDynamic(
			this, &UOWSVehicleInteractionComponent::HandleCancelRequested);
		BoundHotbar->OnCancelReleased.RemoveDynamic(
			this, &UOWSVehicleInteractionComponent::HandleCancelReleased);
	}
	BoundHotbar = DesiredHotbar;
	if (BoundHotbar)
	{
		BoundHotbar->OnCancelRequested.AddUniqueDynamic(
			this, &UOWSVehicleInteractionComponent::HandleCancelRequested);
		BoundHotbar->OnCancelReleased.AddUniqueDynamic(
			this, &UOWSVehicleInteractionComponent::HandleCancelReleased);
		if (OccupiedVehicle)
		{
			SetVehicleCancelContextActive(true);
		}
	}
}

void UOWSVehicleInteractionComponent::SetVehicleCancelContextActive(const bool bActive)
{
	if (!BoundHotbar || bActive == bVehicleCancelContextActive)
	{
		return;
	}
	if (bActive)
	{
		bPreviousCancelContextActive = BoundHotbar->IsCancelContextActive();
		BoundHotbar->SetCancelContextActive(true);
		bVehicleCancelContextActive = true;
	}
	else
	{
		BoundHotbar->SetCancelContextActive(bPreviousCancelContextActive);
		bVehicleCancelContextActive = false;
		bPreviousCancelContextActive = false;
	}
}

void UOWSVehicleInteractionComponent::HandleCancelRequested()
{
	if (!OccupiedVehicle)
	{
		return;
	}
	bCancelHeld = true;
	ExitHoldElapsed = 0.0f;
	if (GetVehicleSpeedMph() <= ImmediateExitSpeedMph)
	{
		TryExitVehicle();
	}
}

void UOWSVehicleInteractionComponent::HandleCancelReleased()
{
	bCancelHeld = false;
	ExitHoldElapsed = 0.0f;
}

void UOWSVehicleInteractionComponent::ShowPrompt(const FString& Text) const
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			GVehiclePromptKey, 0.2f, FColor(255, 220, 60), Text,
			false, FVector2D(1.6f, 1.6f));
	}
}

bool UOWSVehicleInteractionComponent::DebugEnterNearestVehicle()
{
	const bool bEntered = TryEnterVehicle();
	UE_LOG(LogTemp, Warning, TEXT("[VIC] DebugEnterNearestVehicle -> %s (occupied=%s)"),
		bEntered ? TEXT("ENTERED") : TEXT("FAILED"),
		OccupiedVehicle ? *OccupiedVehicle->GetName() : TEXT("none"));
	return bEntered;
}

bool UOWSVehicleInteractionComponent::DebugExitVehicle()
{
	const bool bExited = TryExitVehicle();
	UE_LOG(LogTemp, Warning, TEXT("[VIC] DebugExitVehicle -> %s"),
		bExited ? TEXT("EXITED") : TEXT("FAILED"));
	return bExited;
}

void UOWSVehicleInteractionComponent::UpdateInteraction(const float DeltaTime)
{
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Controller->IsLocalController())
	{
		return;
	}
	UpdateVehicleExitTrace(DeltaTime);

	if (!OccupiedVehicle)
	{
		ExitHoldElapsed = 0.0f;
		ACharacter* Character = Cast<ACharacter>(Controller->GetPawn());

		if (bControlledBailoutActive)
		{
			UpdateControlledBailout(DeltaTime);
			ShowPrompt(TEXT("Bailed out!"));
			return;
		}

		// Still tumbling from a bail-out: no entering until back on your feet.
		if (bRagdollActive)
		{
			UpdateRagdollRecovery(DeltaTime);
			ShowPrompt(TEXT("Bailed out!"));
			return;
		}

		// Clear the bailout lock only after the character leaves the bailout
		// area. A moving vehicle driving away must not clear the lock by itself.
		if (ReentryBlockedVehicle && Character)
		{
			const float AwayDistance = FVector::Dist(
				Character->GetActorLocation(), ReentryBlockOrigin);
			if (AwayDistance > ReentryReleaseDistance)
			{
				ReentryBlockedVehicle = nullptr;
				ReentryBlockOrigin = FVector::ZeroVector;
			}
		}

		if (Character && BoundSelector)
		{
			APawn* SelectedVehicle = Cast<APawn>(BoundSelector->GetDetectedActor());
			UOWSInteractionTargetComponent* SelectedTarget =
				BoundSelector->GetDetectedInteractionTarget();
			FText FailureReason;
			if (SelectedVehicle && SelectedTarget &&
				SelectedTarget->GetOwner() == SelectedVehicle &&
				CanEnterVehicleThroughDoor(
					SelectedVehicle, SelectedTarget->InteractionId, FailureReason))
			{
				ShowPrompt(FString::Printf(
					TEXT("Press  [F / Square]  to enter  %s through %s"),
					*SelectedVehicle->GetActorNameOrLabel(),
					*SelectedTarget->InteractionId.ToString()));
			}
		}
		return;
	}

	// Keep the view on the vehicle. The OWS controller re-targets the camera on
	// possession changes, so re-assert it if something stole it back.
	if (Controller->GetViewTarget() != OccupiedVehicle)
	{
		SetCameraTarget(OccupiedVehicle, 0.0f);
	}

	// Occupying a vehicle: exit on tap when stopped, hold when moving. Keep the
	// live MPH visible so bailout thresholds can be tested deliberately.
	const float CurrentVehicleSpeedMph = GetVehicleSpeedMph();
	if (CurrentVehicleSpeedMph <= ImmediateExitSpeedMph)
	{
		ExitHoldElapsed = 0.0f;
		ShowPrompt(FString::Printf(
			TEXT("Speed: %.1f mph\nPress  [Escape / Circle]  to exit"), CurrentVehicleSpeedMph));
	}
	else if (bCancelHeld)
	{
		ExitHoldElapsed += DeltaTime;
		const float Remaining = FMath::Max(0.0f, MovingExitHoldSeconds - ExitHoldElapsed);
		ShowPrompt(FString::Printf(
			TEXT("Speed: %.1f mph\nHold  [Escape / Circle]  to bail out...  %.1fs"),
			CurrentVehicleSpeedMph, Remaining));
		if (ExitHoldElapsed >= MovingExitHoldSeconds)
		{
			ExitHoldElapsed = 0.0f;
			TryExitVehicle();
		}
	}
	else
	{
		ExitHoldElapsed = 0.0f;
		ShowPrompt(FString::Printf(
			TEXT("Speed: %.1f mph\nHold  [Escape / Circle]  to bail out while moving"),
			CurrentVehicleSpeedMph));
	}
}

bool UOWSVehicleInteractionComponent::CanEnterVehicleThroughDoor(
	APawn* RequestedVehicle,
	const FName RequestedDoorId,
	FText& OutFailureReason) const
{
	OutFailureReason = FText::GetEmpty();
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	ACharacter* Character = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
	const auto* Contact = Character ? Character->FindComponentByClass<UOWSCharacterContactComponent>() : nullptr;
	if (!Controller || !Character || !RequestedVehicle || RequestedDoorId.IsNone() ||
		OccupiedVehicle || bControlledBailoutActive || bRagdollActive || (Contact && !Contact->IsUpright()))
	{
		OutFailureReason = NSLOCTEXT(
			"OWSVehicleInteraction", "EntryStateUnavailable",
			"Vehicle entry is unavailable in the current player state.");
		return false;
	}
	UOWSStockVehicleInteractionComponent* Interaction = nullptr;
	FName DoorId = NAME_None;
	FName SeatId = NAME_None;
	if (!FindEnterableVehicle(
		*Character, Interaction, DoorId, SeatId,
		RequestedVehicle, RequestedDoorId))
	{
		OutFailureReason = NSLOCTEXT(
			"OWSVehicleInteraction", "DoorUnavailable",
			"That vehicle door is unavailable or out of range.");
		return false;
	}
	return true;
}

bool UOWSVehicleInteractionComponent::TryEnterVehicleThroughDoor(
	APawn* RequestedVehicle,
	const FName RequestedDoorId,
	FText& OutFailureReason)
{
	if (!CanEnterVehicleThroughDoor(
		RequestedVehicle, RequestedDoorId, OutFailureReason))
	{
		return false;
	}
	if (!TryEnterVehicle(RequestedVehicle, RequestedDoorId))
	{
		OutFailureReason = NSLOCTEXT(
			"OWSVehicleInteraction", "EntryRejected",
			"Vehicle entry could not be completed.");
		return false;
	}
	UE_LOG(LogTemp, Display, TEXT("[VIC] Entered Activate target %s door=%s"),
		*RequestedVehicle->GetActorNameOrLabel(), *RequestedDoorId.ToString());
	return true;
}

bool UOWSVehicleInteractionComponent::TryEnterVehicle(
	APawn* RequestedVehicle,
	const FName RequestedDoorId)
{
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	ACharacter* Character = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
	if (!Controller || !Character)
	{
		return false;
	}
	const auto* Contact = Character->FindComponentByClass<UOWSCharacterContactComponent>();
	if (Contact && !Contact->IsUpright()) return false;

	UOWSStockVehicleInteractionComponent* Interaction = nullptr;
	FName DoorId = NAME_None;
	FName SeatId = NAME_None;
	APawn* Vehicle = FindEnterableVehicle(
		*Character, Interaction, DoorId, SeatId,
		RequestedVehicle, RequestedDoorId);
	if (!Vehicle || !Interaction || !Interaction->IsControlSeat(SeatId) ||
		!Interaction->OccupySeat(SeatId, Character))
	{
		return false;
	}

	FTransform SeatTransform;
	if (!Interaction->GetSeatWorldTransform(SeatId, SeatTransform))
	{
		Interaction->ReleaseSeat(Character);
		return false;
	}
	HomeCharacter = Character;
	OccupiedVehicle = Vehicle;
	OccupiedSeatId = SeatId;
	OccupiedDoorId = DoorId;
	if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}
	Character->SetActorEnableCollision(false);
	Character->SetActorHiddenInGame(true);
	Character->AttachToActor(Vehicle, FAttachmentTransformRules::KeepWorldTransform);
	Character->SetActorTransform(SeatTransform, false, nullptr, ETeleportType::TeleportPhysics);
	Controller->Possess(Vehicle);
	if (Controller->GetPawn() != Vehicle)
	{
		Character->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		Character->SetActorHiddenInGame(false);
		Character->SetActorEnableCollision(true);
		if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			Movement->SetMovementMode(MOVE_Walking);
		}
		Interaction->ReleaseSeat(Character);
		HomeCharacter = nullptr;
		OccupiedVehicle = nullptr;
		OccupiedSeatId = NAME_None;
		OccupiedDoorId = NAME_None;
		return false;
	}

	Interaction->SetDriverPresent(true);
	ApplyVehicleInputContext(true);
	StartVehicleExitTrace(*Character, *Vehicle);
	bCancelHeld = false;
	SetVehicleCancelContextActive(true);
	// Hand the view to the vehicle so its spring-arm chase camera is used.
	SetCameraTarget(Vehicle, CameraBlendSeconds);
	return true;
}

bool UOWSVehicleInteractionComponent::TryExitVehicle()
{
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	ACharacter* Character = HomeCharacter.Get();
	APawn* Vehicle = OccupiedVehicle.Get();
	UOWSStockVehicleInteractionComponent* Interaction = Vehicle
		? Vehicle->FindComponentByClass<UOWSStockVehicleInteractionComponent>() : nullptr;
	if (!Controller || !Character || !Vehicle || !Interaction ||
		Controller->GetPawn() != Vehicle)
	{
		return false;
	}

	// Snapshot the vehicle's motion before we detach; if it is still rolling the
	// driver bails out into a physics ragdoll instead of stepping to the door.
	const FVector VehicleVelocity = GetVehicleVelocity();
	const float VehicleSpeedMph = VehicleVelocity.Size() * 0.0223694f;
	const bool bMovingFast = VehicleSpeedMph > ImmediateExitSpeedMph;

	// Moving bailouts use the same configured door that admitted the occupant.
	FVector DoorDirection = -Vehicle->GetActorRightVector();
	FTransform ConfiguredExitTransform;
	if (Interaction->GetDoorExitWorldTransform(OccupiedDoorId, ConfiguredExitTransform))
	{
		DoorDirection = ConfiguredExitTransform.GetRotation().GetForwardVector();
	}
	DoorDirection.Z = 0.0f;
	DoorDirection.Normalize();

	Character->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	if (bMovingFast)
	{
		// Authored door exits already clear the vehicle. Ground and collision
		// validation are safer than manufacturing a sideways/upward launch.
		if (!PlaceCharacterAtSafeDoorExit(*Character, *Vehicle, *Interaction))
		{
			FTransform SeatTransform;
			if (Interaction->GetSeatWorldTransform(OccupiedSeatId, SeatTransform))
			{
				Character->AttachToActor(
					Vehicle, FAttachmentTransformRules::KeepWorldTransform);
				Character->SetActorTransform(
					SeatTransform, false, nullptr, ETeleportType::TeleportPhysics);
			}
			return false;
		}
		const UCapsuleComponent* ExitCapsule = Character->GetCapsuleComponent();
		const UPrimitiveComponent* VehicleBody = Interaction->GetVehiclePhysicsBody();
		if (ExitCapsule && VehicleBody && VehicleBody->OverlapComponent(
			Character->GetActorLocation(), Character->GetActorQuat(),
			FCollisionShape::MakeCapsule(
				ExitCapsule->GetScaledCapsuleRadius(),
				ExitCapsule->GetScaledCapsuleHalfHeight())))
		{
			FTransform SeatTransform;
			if (Interaction->GetSeatWorldTransform(OccupiedSeatId, SeatTransform))
			{
				Character->AttachToActor(
					Vehicle, FAttachmentTransformRules::KeepWorldTransform);
				Character->SetActorTransform(
					SeatTransform, false, nullptr, ETeleportType::TeleportPhysics);
			}
			return false;
		}
	}
	else if (!PlaceCharacterAtSafeDoorExit(*Character, *Vehicle, *Interaction))
	{
		FTransform SeatTransform;
		if (Interaction->GetSeatWorldTransform(OccupiedSeatId, SeatTransform))
		{
			Character->AttachToActor(Vehicle, FAttachmentTransformRules::KeepWorldTransform);
			Character->SetActorTransform(SeatTransform, false, nullptr, ETeleportType::TeleportPhysics);
		}
		return false;
	}

	Character->SetActorHiddenInGame(false);
	Character->SetActorEnableCollision(true);
	Controller->Possess(Character);
	if (Controller->GetPawn() != Character)
	{
		return false;
	}
	if (bMovingFast)
	{
		// The character was just teleported from its hidden seat transform to the
		// authored door exit. Update its complete component hierarchy before the
		// camera cut so the first bailout frame is at the door, not the old seat.
		FRotator BailoutViewRotation = Controller->GetControlRotation();
		BailoutViewRotation.Yaw = VehicleVelocity.Rotation().Yaw;
		BailoutViewRotation.Roll = 0.0f;
		Controller->SetControlRotation(BailoutViewRotation);
		Character->UpdateComponentTransforms();
		SetCameraTarget(Character, 0.0f);
	}
	Interaction->ReleaseSeat(Character);
	Interaction->SetDriverPresent(false);
	if (bMovingFast)
	{
		if (VehicleSpeedMph <= ControlledRollMinimumSpeedMph)
		{
			if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
			{
				Movement->SetMovementMode(MOVE_Walking);
				Movement->Velocity = VehicleVelocity;
			}
		}
		else
		{
			BeginControlledBailout(*Character, VehicleVelocity, DoorDirection);
		}
		if (bControlledBailoutActive)
		{
			ReentryBlockedVehicle = Vehicle;
			ReentryBlockOrigin = Character->GetActorLocation();
		}
	}
	else if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
	{
		Movement->SetMovementMode(MOVE_Walking);
	}
	OccupiedVehicle = nullptr;
	OccupiedSeatId = NAME_None;
	OccupiedDoorId = NAME_None;
	HomeCharacter = nullptr;
	bCancelHeld = false;
	ExitHoldElapsed = 0.0f;
	SetVehicleCancelContextActive(false);
	ApplyVehicleInputContext(false);
	// Cut straight to the character when bailing: blending from a camera that is
	// racing away on the car sweeps the view across the level and reads as a
	// flicker. A stationary car can afford the smooth blend.
	if (!bMovingFast)
	{
		SetCameraTarget(Character, CameraBlendSeconds);
	}
	return true;
}

void UOWSVehicleInteractionComponent::BeginControlledBailout(
	ACharacter& Character,
	const FVector& VehicleVelocity,
	const FVector& DoorDirection)
{
	USkeletalMeshComponent* Mesh = Character.GetMesh();
	UCharacterMovementComponent* Movement = Character.GetCharacterMovement();
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Mesh || !Movement || !Controller)
	{
		return;
	}

	ControlledBailoutCharacter = &Character;
	bControlledBailoutActive = true;
	ControlledBailoutElapsed = 0.0f;
	BailoutTelemetryAccumulator = 0.0f;
	BailoutStartLocation = Character.GetActorLocation();
	BailoutPreviousLocation = BailoutStartLocation;
	BailoutPreviousObservedVelocity = VehicleVelocity;
	ControlledBailoutSourceVehicle = OccupiedVehicle;
	bControlledBailoutAddedMoveIgnore = !Controller->IsMoveInputIgnored();
	if (bControlledBailoutAddedMoveIgnore)
	{
		Controller->SetIgnoreMoveInput(true);
	}

	Character.SetActorRotation(
		FRotator(0.0f, VehicleVelocity.Rotation().Yaw, 0.0f),
		ETeleportType::TeleportPhysics);
	Movement->SetMovementMode(MOVE_Walking);
	// Classical relative motion: at separation the driver retains the vehicle's
	// velocity. Authored placement handles clearance, so no fake side/up kick.
	Movement->Velocity = VehicleVelocity;
	ControlledRollTargetSpeed = VehicleVelocity.Size2D();
	ControlledRollKineticEnergyJoules = 0.0;
	ControlledRollCycleDistanceCm = 0.0f;
	ControlledRollDistanceIntoCycleCm = 0.0f;
	ControlledRollPreviousLocation = Character.GetActorLocation();
	ControlledRollContactCount = 0;
	ControlledRollIntervalSlidingWorkJoules = 0.0;
	ControlledRollIntervalAerodynamicWorkJoules = 0.0;
	ControlledRollIntervalImpactLossJoules = 0.0;
	ControlledRollIntervalContacts = 0;
	const double SpeedMetersPerSecond = ControlledRollTargetSpeed * 0.01;
	ControlledRollKineticEnergyJoules =
		0.5 * Movement->Mass * FMath::Square(SpeedMetersPerSecond);
	const UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
	const float EffectiveRollRadiusCm = Capsule
		? Capsule->GetScaledCapsuleRadius()
		: 42.0f;
	ControlledRollCycleDistanceCm =
		2.0f * UE_PI * FMath::Max(1.0f, EffectiveRollRadiusCm);
	if (bLogBailoutTelemetry)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VIC][BailoutTrace] START phase=%s vehicle_speed=%.2f_mph vehicle_velocity=%s character_location=%s stand_speed=%.2f_cmps movement_mode=%d kinetic_energy=%.2f_J cycle_distance=%.2f_cm sliding_mu=%.3f impact_energy_loss=%.3f"),
			TEXT("Roll"),
			VehicleVelocity.Size() * 0.0223694f,
			*VehicleVelocity.ToCompactString(),
			*BailoutStartLocation.ToCompactString(),
			ControlledRollStandSpeed,
			static_cast<int32>(Movement->MovementMode),
			ControlledRollKineticEnergyJoules,
			ControlledRollCycleDistanceCm,
			ControlledRollSlidingFrictionCoefficient,
			ControlledRollImpactEnergyLossFraction);
	}

	const bool bRightSide = FVector::DotProduct(
		DoorDirection, Character.GetActorRightVector()) >= 0.0f;
	TSoftObjectPtr<UAnimSequenceBase>& AnimationAsset = bRightSide
		? RollRightAnimation
		: RollLeftAnimation;
	UAnimSequenceBase* Animation = AnimationAsset.LoadSynchronous();
	if (Animation)
	{
		// The bailout roll is full-body and root-locked so animation displacement
		// cannot overwrite the energy-model velocity or leave the capsule behind.
		UAnimSequenceBase* ControlledAnimation =
			MakeRootLockedRuntimeSequence(Animation, this);
		ActiveControlledAnimation = ControlledAnimation;
		CachedControlledAnimationMode = static_cast<uint8>(Mesh->GetAnimationMode());
		CachedControlledAnimClass = Mesh->GetAnimClass();
		bControlledBailoutOwnsMeshAnimation = true;
		bControlledBailoutUsesSingleNode = true;
		CachedRollBrakingDeceleration = Movement->BrakingDecelerationWalking;
		CachedRollGroundFriction = Movement->GroundFriction;
		Movement->BrakingDecelerationWalking = 0.0f;
		Movement->GroundFriction = 0.0f;
		AddTickPrerequisiteComponent(Movement);
		bControlledRollTickAfterMovement = true;
		Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Mesh->PlayAnimation(ControlledAnimation, false);
		if (UAnimSingleNodeInstance* SingleNode = Mesh->GetSingleNodeInstance())
		{
			SingleNode->SetLooping(false);
			SingleNode->SetPlayRate(1.0f);
			SingleNode->SetPosition(FMath::Clamp(
				ControlledRollClipStartSeconds, 0.0f,
				ControlledAnimation->GetPlayLength()), false);
			SingleNode->SetPlaying(false);
		}
		if (bLogBailoutTelemetry)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[VIC][BailoutTrace] ANIMATION asset=%s source_length=%.3f_seconds clip_start=%.3f_seconds clip_end=%.3f_seconds play_rate=%.3f looping=%d"),
				*Animation->GetPathName(), Animation->GetPlayLength(),
				ControlledRollClipStartSeconds,
				ControlledRollClipEndSeconds,
				1.0f, 1);
			for (const FAnimNotifyEvent& NotifyEvent : Animation->Notifies)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[VIC][BailoutTrace] ANIMATION_NOTIFY name=%s time=%.3f duration=%.3f"),
					*NotifyEvent.NotifyName.ToString(), NotifyEvent.GetTime(),
					NotifyEvent.GetDuration());
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VIC] Controlled bailout animation could not be loaded for %s."),
			*Character.GetName());
		FinishControlledBailout();
	}
}

void UOWSVehicleInteractionComponent::UpdateControlledBailout(const float DeltaTime)
{
	ACharacter* Character = ControlledBailoutCharacter.Get();
	if (!Character)
	{
		FinishControlledBailout();
		return;
	}

	ControlledBailoutElapsed += DeltaTime;
	BailoutTelemetryAccumulator += DeltaTime;
	if (bLogBailoutTelemetry && BailoutTelemetryAccumulator >= 0.1f)
	{
		UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
		APlayerController* Controller = Cast<APlayerController>(GetOwner());
		const FVector CurrentLocation = Character->GetActorLocation();
		const FVector ObservedVelocity =
			(CurrentLocation - BailoutPreviousLocation) / BailoutTelemetryAccumulator;
		const FVector ObservedAcceleration =
			(ObservedVelocity - BailoutPreviousObservedVelocity) / BailoutTelemetryAccumulator;
		const float DistanceFromStart = FVector::Dist2D(CurrentLocation, BailoutStartLocation);
		const float DistanceFromVehicle = ControlledBailoutSourceVehicle
			? FVector::Dist2D(CurrentLocation, ControlledBailoutSourceVehicle->GetActorLocation())
			: -1.0f;
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		UAnimSingleNodeInstance* SingleNode = Mesh ? Mesh->GetSingleNodeInstance() : nullptr;
		const FVector CameraLocation = Controller && Controller->PlayerCameraManager
			? Controller->PlayerCameraManager->GetCameraLocation()
			: FVector::ZeroVector;
		UE_LOG(LogTemp, Warning,
			TEXT("[VIC][BailoutTrace] SAMPLE phase=%s elapsed=%.3f location=%s movement_velocity=%s observed_velocity=%s observed_acceleration=%s speed=%.2f_cmps target_speed=%.2f_cmps kinetic_energy=%.2f_J sliding_work=%.2f_J aerodynamic_work=%.2f_J impact_loss=%.2f_J interval_contacts=%d total_contacts=%d cycle_progress=%.3f distance_from_start=%.2f_cm distance_from_vehicle=%.2f_cm grounded=%d movement_mode=%d root_motion=%d animation=%s animation_time=%.3f camera_location=%s camera_distance=%.2f_cm view_target=%s steer=%.3f"),
			TEXT("Roll"),
			ControlledBailoutElapsed,
			*CurrentLocation.ToCompactString(),
			Movement ? *Movement->Velocity.ToCompactString() : TEXT("none"),
			*ObservedVelocity.ToCompactString(),
			*ObservedAcceleration.ToCompactString(),
			Movement ? Movement->Velocity.Size2D() : -1.0f,
			ControlledRollTargetSpeed,
			ControlledRollKineticEnergyJoules,
			ControlledRollIntervalSlidingWorkJoules,
			ControlledRollIntervalAerodynamicWorkJoules,
			ControlledRollIntervalImpactLossJoules,
			ControlledRollIntervalContacts,
			ControlledRollContactCount,
			ControlledRollCycleDistanceCm > KINDA_SMALL_NUMBER
				? ControlledRollDistanceIntoCycleCm / ControlledRollCycleDistanceCm
				: 0.0f,
			DistanceFromStart,
			DistanceFromVehicle,
			Movement && Movement->IsMovingOnGround() ? 1 : 0,
			Movement ? static_cast<int32>(Movement->MovementMode) : -1,
			Character->IsPlayingRootMotion() ? 1 : 0,
			SingleNode && SingleNode->GetCurrentAsset()
				? *SingleNode->GetCurrentAsset()->GetName() : TEXT("none"),
			SingleNode ? SingleNode->GetCurrentTime() : -1.0f,
			*CameraLocation.ToCompactString(),
			FVector::Dist(CameraLocation, CurrentLocation),
			Controller && Controller->GetViewTarget()
				? *Controller->GetViewTarget()->GetName() : TEXT("none"),
			Controller ? Controller->GetInputAnalogKeyState(EKeys::Gamepad_LeftX) : 0.0f);
		BailoutPreviousLocation = CurrentLocation;
		BailoutPreviousObservedVelocity = ObservedVelocity;
		BailoutTelemetryAccumulator = 0.0f;
		ControlledRollIntervalSlidingWorkJoules = 0.0;
		ControlledRollIntervalAerodynamicWorkJoules = 0.0;
		ControlledRollIntervalImpactLossJoules = 0.0;
		ControlledRollIntervalContacts = 0;
	}
	if (bControlledBailoutUsesSingleNode)
	{
		UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
		APlayerController* Controller = Cast<APlayerController>(GetOwner());
		if (!Movement || !Controller)
		{
			FinishControlledBailout();
			return;
		}

		// Gait logic restores ordinary walking braking every frame. A controlled
		// roll owns its speed curve, so neutralize those locomotion values after
		// CharacterMovement ticks and drive from the retained bailout speed rather
		// than compounding two unrelated braking systems.
		Movement->BrakingDecelerationWalking = 0.0f;
		Movement->GroundFriction = 0.0f;

		FVector HorizontalVelocity = Movement->Velocity;
		HorizontalVelocity.Z = 0.0f;
		const float HorizontalSpeed = HorizontalVelocity.Size();
		if (HorizontalSpeed > KINDA_SMALL_NUMBER)
		{
			const float SteeringInput = Controller->GetInputAnalogKeyState(EKeys::Gamepad_LeftX);
			const float SteeringAngle =
				SteeringInput * ControlledRollSteeringDegreesPerSecond * DeltaTime;
			const FVector SteeredVelocity = HorizontalVelocity.RotateAngleAxis(
				SteeringAngle, FVector::UpVector);
			const FVector CurrentLocation = Character->GetActorLocation();
			const float TravelDistanceCm = FVector::Dist2D(
				CurrentLocation, ControlledRollPreviousLocation);
			ControlledRollPreviousLocation = CurrentLocation;
			const double TravelDistanceMeters = TravelDistanceCm * 0.01;
			const double MassKilograms = FMath::Max(1.0f, Movement->Mass);
			const double SpeedMetersPerSecond = ControlledRollTargetSpeed * 0.01;
			const double SlidingForceNewtons =
				ControlledRollSlidingFrictionCoefficient *
				MassKilograms * 9.80665;
			const double AerodynamicForceNewtons =
				0.5 * ControlledRollAirDensityKgPerCubicMeter *
				ControlledRollAerodynamicDragCoefficient *
				ControlledRollFrontalAreaSquareMeters *
				FMath::Square(SpeedMetersPerSecond);
			const double StandSpeedMetersPerSecond =
				ControlledRollStandSpeed * 0.01;
			const double StandEnergyJoules =
				0.5 * MassKilograms * FMath::Square(StandSpeedMetersPerSecond);
			const double SlidingWorkJoules = FMath::Min(
				FMath::Max(0.0, ControlledRollKineticEnergyJoules - StandEnergyJoules),
				SlidingForceNewtons * TravelDistanceMeters);
			ControlledRollKineticEnergyJoules -= SlidingWorkJoules;
			const double AerodynamicWorkJoules = FMath::Min(
				FMath::Max(0.0, ControlledRollKineticEnergyJoules - StandEnergyJoules),
				AerodynamicForceNewtons * TravelDistanceMeters);
			ControlledRollKineticEnergyJoules -= AerodynamicWorkJoules;
			ControlledRollIntervalSlidingWorkJoules += SlidingWorkJoules;
			ControlledRollIntervalAerodynamicWorkJoules += AerodynamicWorkJoules;

			ControlledRollDistanceIntoCycleCm += TravelDistanceCm;
			while (ControlledRollCycleDistanceCm > KINDA_SMALL_NUMBER &&
				ControlledRollDistanceIntoCycleCm >= ControlledRollCycleDistanceCm &&
				ControlledRollKineticEnergyJoules > StandEnergyJoules)
			{
				ControlledRollDistanceIntoCycleCm -= ControlledRollCycleDistanceCm;
				const double ImpactLossJoules = FMath::Min(
					ControlledRollKineticEnergyJoules - StandEnergyJoules,
					ControlledRollKineticEnergyJoules *
					ControlledRollImpactEnergyLossFraction);
				ControlledRollKineticEnergyJoules -= ImpactLossJoules;
				ControlledRollIntervalImpactLossJoules += ImpactLossJoules;
				++ControlledRollContactCount;
				++ControlledRollIntervalContacts;
				if (bLogBailoutTelemetry)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("[VIC][BailoutTrace] CONTACT index=%d impact_loss=%.2f_J remaining_energy=%.2f_J location=%s"),
						ControlledRollContactCount,
						ImpactLossJoules,
						ControlledRollKineticEnergyJoules,
						*CurrentLocation.ToCompactString());
				}
			}

			ControlledRollKineticEnergyJoules = FMath::Max(
				StandEnergyJoules, ControlledRollKineticEnergyJoules);
			ControlledRollTargetSpeed = static_cast<float>(
				FMath::Sqrt(2.0 * ControlledRollKineticEnergyJoules /
					MassKilograms) * 100.0);
			const FVector NewHorizontalVelocity =
				SteeredVelocity.GetSafeNormal() * ControlledRollTargetSpeed;
			Movement->Velocity.X = NewHorizontalVelocity.X;
			Movement->Velocity.Y = NewHorizontalVelocity.Y;
			Character->SetActorRotation(
				FRotator(0.0f, SteeredVelocity.Rotation().Yaw, 0.0f),
				ETeleportType::None);

			if (USkeletalMeshComponent* Mesh = Character->GetMesh())
			{
				if (UAnimSingleNodeInstance* SingleNode = Mesh->GetSingleNodeInstance())
				{
					const float CycleAlpha = ControlledRollCycleDistanceCm > KINDA_SMALL_NUMBER
						? ControlledRollDistanceIntoCycleCm / ControlledRollCycleDistanceCm
						: 0.0f;
					SingleNode->SetPosition(FMath::Lerp(
						ControlledRollClipStartSeconds,
						ControlledRollClipEndSeconds,
						CycleAlpha), false);
					SingleNode->SetPlaying(false);
				}
			}
		}

		const bool bStoppedByCollision = Movement->IsMovingOnGround() &&
			HorizontalSpeed <= 1.0f;
		if (Movement->IsMovingOnGround() &&
			(ControlledRollTargetSpeed <= ControlledRollStandSpeed ||
				bStoppedByCollision))
		{
			if (bLogBailoutTelemetry)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[VIC][BailoutTrace] TRANSITION Roll->Locomotion speed=%.2f_cmps location=%s distance_from_start=%.2f_cm"),
					Movement->Velocity.Size2D(),
					*Character->GetActorLocation().ToCompactString(),
					FVector::Dist2D(Character->GetActorLocation(), BailoutStartLocation));
			}
			FinishControlledBailout();
		}
		return;
	}
}

float UOWSVehicleInteractionComponent::CalculateControlledRollContinuousDeceleration(
	const UCharacterMovementComponent& Movement,
	const float SpeedCmPerSecond) const
{
	constexpr float StandardGravityMetersPerSecondSquared = 9.80665f;
	const float SpeedMetersPerSecond = SpeedCmPerSecond * 0.01f;
	const float SlidingDecelerationMetersPerSecondSquared =
		ControlledRollSlidingFrictionCoefficient *
		StandardGravityMetersPerSecondSquared;
	const float AerodynamicForceNewtons =
		0.5f * ControlledRollAirDensityKgPerCubicMeter *
		ControlledRollAerodynamicDragCoefficient *
		ControlledRollFrontalAreaSquareMeters *
		FMath::Square(SpeedMetersPerSecond);
	const float AerodynamicDecelerationMetersPerSecondSquared =
		AerodynamicForceNewtons / FMath::Max(1.0f, Movement.Mass);
	return (SlidingDecelerationMetersPerSecondSquared +
		AerodynamicDecelerationMetersPerSecondSquared) * 100.0f;
}

void UOWSVehicleInteractionComponent::FinishControlledBailout()
{
	ACharacter* Character = ControlledBailoutCharacter.Get();
	if (bLogBailoutTelemetry && Character)
	{
		const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
		UE_LOG(LogTemp, Warning,
			TEXT("[VIC][BailoutTrace] FINISH phase=Roll elapsed=%.3f location=%s movement_velocity=%s distance_from_start=%.2f_cm distance_from_vehicle=%.2f_cm"),
			ControlledBailoutElapsed,
			*Character->GetActorLocation().ToCompactString(),
			Movement ? *Movement->Velocity.ToCompactString() : TEXT("none"),
			FVector::Dist2D(Character->GetActorLocation(), BailoutStartLocation),
			ControlledBailoutSourceVehicle
				? FVector::Dist2D(
					Character->GetActorLocation(), ControlledBailoutSourceVehicle->GetActorLocation())
				: -1.0f);
	}
	if (Character)
	{
		if (bControlledRollTickAfterMovement)
		{
			if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
			{
				RemoveTickPrerequisiteComponent(Movement);
				Movement->BrakingDecelerationWalking = CachedRollBrakingDeceleration;
				Movement->GroundFriction = CachedRollGroundFriction;
			}
		}
		else if (bControlledBailoutUsesSingleNode)
		{
			if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
			{
				Movement->BrakingDecelerationWalking = CachedRollBrakingDeceleration;
				Movement->GroundFriction = CachedRollGroundFriction;
			}
		}
		if (USkeletalMeshComponent* Mesh = Character->GetMesh())
		{
			if (bControlledBailoutOwnsMeshAnimation)
			{
				if (CachedControlledAnimClass)
				{
					Mesh->SetAnimInstanceClass(CachedControlledAnimClass);
				}
				else
				{
					Mesh->SetAnimationMode(static_cast<EAnimationMode::Type>(
						CachedControlledAnimationMode));
				}
			}
			// A missing roll never acquired animation ownership. In that case,
			// leave unrelated dynamic montages and their slot state untouched.
		}
	}
	if (bControlledBailoutAddedMoveIgnore)
	{
		if (APlayerController* Controller = Cast<APlayerController>(GetOwner()))
		{
			Controller->SetIgnoreMoveInput(false);
		}
	}
	// The player has completed the authored recovery and regained control. A
	// bailout must not permanently blacklist a nearby vehicle merely because a
	// low-speed exit did not carry the character beyond an arbitrary radius.
	ReentryBlockedVehicle = nullptr;
	ReentryBlockOrigin = FVector::ZeroVector;

	bControlledBailoutActive = false;
	bControlledBailoutAddedMoveIgnore = false;
	bControlledBailoutUsesSingleNode = false;
	bControlledBailoutOwnsMeshAnimation = false;
	bControlledRollTickAfterMovement = false;
	ControlledBailoutElapsed = 0.0f;
	CachedRollBrakingDeceleration = 0.0f;
	CachedRollGroundFriction = 0.0f;
	ControlledRollTargetSpeed = 0.0f;
	ControlledRollKineticEnergyJoules = 0.0;
	ControlledRollCycleDistanceCm = 0.0f;
	ControlledRollDistanceIntoCycleCm = 0.0f;
	ControlledRollPreviousLocation = FVector::ZeroVector;
	ControlledRollContactCount = 0;
	ControlledRollIntervalSlidingWorkJoules = 0.0;
	ControlledRollIntervalAerodynamicWorkJoules = 0.0;
	ControlledRollIntervalImpactLossJoules = 0.0;
	ControlledRollIntervalContacts = 0;
	BailoutTelemetryAccumulator = 0.0f;
	BailoutStartLocation = FVector::ZeroVector;
	BailoutPreviousLocation = FVector::ZeroVector;
	BailoutPreviousObservedVelocity = FVector::ZeroVector;
	ControlledBailoutSourceVehicle = nullptr;
	CachedControlledAnimationMode = 0;
	CachedControlledAnimClass = nullptr;
	ActiveControlledAnimation = nullptr;
	ControlledBailoutCharacter = nullptr;
}

void UOWSVehicleInteractionComponent::StartVehicleExitTrace(
	ACharacter& Character,
	APawn& Vehicle)
{
	if (!bLogBailoutTelemetry)
	{
		return;
	}
	bVehicleExitTraceActive = true;
	VehicleExitTraceElapsed = 0.0f;
	VehicleExitTraceAccumulator = 0.0f;
	VehicleExitTraceCharacter = &Character;
	VehicleExitTraceVehicle = &Vehicle;
	VehicleExitTracePreviousLocation = Character.GetActorLocation();
	VehicleExitTracePreviousVelocity = Vehicle.GetVelocity();

	const UCharacterMovementComponent* Movement = Character.GetCharacterMovement();
	const UOWSStockVehicleInteractionComponent* Interaction =
		Vehicle.FindComponentByClass<UOWSStockVehicleInteractionComponent>();
	const UPrimitiveComponent* VehicleBody = Interaction
		? Interaction->GetVehiclePhysicsBody()
		: nullptr;
	UE_LOG(LogTemp, Warning,
		TEXT("[VIC][FullTrace] ENTRY character_mass=%.2f_kg vehicle_mass=%.2f_kg character_location=%s vehicle_location=%s vehicle_velocity=%s braking=%.2f_cmps2 ground_friction=%.3f falling_lateral_friction=%.3f"),
		Movement ? Movement->Mass : -1.0f,
		VehicleBody ? VehicleBody->GetMass() : -1.0f,
		*Character.GetActorLocation().ToCompactString(),
		*Vehicle.GetActorLocation().ToCompactString(),
		*(VehicleBody ? VehicleBody->GetPhysicsLinearVelocity() : Vehicle.GetVelocity()).ToCompactString(),
		Movement ? Movement->BrakingDecelerationWalking : -1.0f,
		Movement ? Movement->GroundFriction : -1.0f,
		Movement ? Movement->FallingLateralFriction : -1.0f);
}

void UOWSVehicleInteractionComponent::UpdateVehicleExitTrace(const float DeltaTime)
{
	ACharacter* Character = VehicleExitTraceCharacter.Get();
	APawn* Vehicle = VehicleExitTraceVehicle.Get();
	if (!bVehicleExitTraceActive || !bLogBailoutTelemetry || !Character || !Vehicle)
	{
		return;
	}

	VehicleExitTraceElapsed += DeltaTime;
	VehicleExitTraceAccumulator += DeltaTime;
	if (VehicleExitTraceAccumulator < 0.1f)
	{
		return;
	}

	UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	UOWSStockVehicleInteractionComponent* Interaction =
		Vehicle->FindComponentByClass<UOWSStockVehicleInteractionComponent>();
	UPrimitiveComponent* VehicleBody = Interaction
		? Interaction->GetVehiclePhysicsBody()
		: nullptr;
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	const FVector CharacterLocation = Character->GetActorLocation();
	const FVector ObservedVelocity =
		(CharacterLocation - VehicleExitTracePreviousLocation) /
		VehicleExitTraceAccumulator;
	const FVector ObservedAcceleration =
		(ObservedVelocity - VehicleExitTracePreviousVelocity) /
		VehicleExitTraceAccumulator;
	const FVector VehicleVelocity = VehicleBody
		? VehicleBody->GetPhysicsLinearVelocity()
		: Vehicle->GetVelocity();
	const float CharacterSpeed = Movement ? Movement->Velocity.Size2D() : -1.0f;
	const bool bGrounded = Movement && Movement->IsMovingOnGround();
	const float ActiveRollDeceleration = bControlledBailoutUsesSingleNode && Movement
		? CalculateControlledRollContinuousDeceleration(*Movement, CharacterSpeed)
		: 0.0f;
	const TCHAR* Phase = OccupiedVehicle
		? TEXT("Vehicle")
		: (bControlledBailoutActive ? TEXT("Roll") : TEXT("PostControl"));

	UE_LOG(LogTemp, Warning,
		TEXT("[VIC][FullTrace] SAMPLE phase=%s elapsed=%.3f character_mass=%.2f_kg vehicle_mass=%.2f_kg character_location=%s vehicle_location=%s character_movement_velocity=%s character_observed_velocity=%s character_observed_acceleration=%s vehicle_velocity=%s character_speed=%.2f_cmps vehicle_speed=%.2f_cmps distance=%.2f_cm grounded=%d movement_mode=%d root_motion=%d braking=%.2f_cmps2 ground_friction=%.3f roll_deceleration=%.2f_cmps2 steer=%.3f"),
		Phase,
		VehicleExitTraceElapsed,
		Movement ? Movement->Mass : -1.0f,
		VehicleBody ? VehicleBody->GetMass() : -1.0f,
		*CharacterLocation.ToCompactString(),
		*Vehicle->GetActorLocation().ToCompactString(),
		Movement ? *Movement->Velocity.ToCompactString() : TEXT("none"),
		*ObservedVelocity.ToCompactString(),
		*ObservedAcceleration.ToCompactString(),
		*VehicleVelocity.ToCompactString(),
		CharacterSpeed,
		VehicleVelocity.Size(),
		FVector::Dist2D(CharacterLocation, Vehicle->GetActorLocation()),
		bGrounded ? 1 : 0,
		Movement ? static_cast<int32>(Movement->MovementMode) : -1,
		Character->IsPlayingRootMotion() ? 1 : 0,
		Movement ? Movement->BrakingDecelerationWalking : -1.0f,
		Movement ? Movement->GroundFriction : -1.0f,
		ActiveRollDeceleration,
		Controller ? Controller->GetInputAnalogKeyState(EKeys::Gamepad_LeftX) : 0.0f);

	VehicleExitTracePreviousLocation = CharacterLocation;
	VehicleExitTracePreviousVelocity = ObservedVelocity;
	VehicleExitTraceAccumulator = 0.0f;

	if (!OccupiedVehicle && !bControlledBailoutActive && !bRagdollActive &&
		bGrounded && CharacterSpeed <= 1.0f)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VIC][FullTrace] STOP elapsed=%.3f character_location=%s vehicle_location=%s distance=%.2f_cm"),
			VehicleExitTraceElapsed,
			*CharacterLocation.ToCompactString(),
			*Vehicle->GetActorLocation().ToCompactString(),
			FVector::Dist2D(CharacterLocation, Vehicle->GetActorLocation()));
		bVehicleExitTraceActive = false;
		VehicleExitTraceCharacter = nullptr;
		VehicleExitTraceVehicle = nullptr;
	}
}

APawn* UOWSVehicleInteractionComponent::FindEnterableVehicle(
	ACharacter& Character,
	UOWSStockVehicleInteractionComponent*& OutInteraction,
	FName& OutDoorId,
	FName& OutSeatId,
	const APawn* RequiredVehicle,
	const FName RequiredDoorId) const
{
	OutInteraction = nullptr;
	OutDoorId = NAME_None;
	OutSeatId = NAME_None;
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	if (RequiredVehicle && !RequiredDoorId.IsNone())
	{
		APawn* Candidate = const_cast<APawn*>(RequiredVehicle);
		UOWSStockVehicleInteractionComponent* CandidateInteraction =
			Candidate->FindComponentByClass<UOWSStockVehicleInteractionComponent>();
		AController* CandidateController = Candidate->GetController();
		FTransform DoorTransform;
		if (!CandidateInteraction ||
			(CandidateController && CandidateController->IsPlayerController()) ||
			Candidate == ReentryBlockedVehicle ||
			!CandidateInteraction->GetDoorWorldTransform(RequiredDoorId, DoorTransform) ||
			FVector::Dist(Character.GetActorLocation(), DoorTransform.GetLocation()) >
				MaximumEnterDistance)
		{
			return nullptr;
		}
		FName SeatId;
		if (!CandidateInteraction->SelectAvailableSeat(
			RequiredDoorId, true, &Character, SeatId) ||
			!CandidateInteraction->IsControlSeat(SeatId))
		{
			return nullptr;
		}
		OutInteraction = CandidateInteraction;
		OutDoorId = RequiredDoorId;
		OutSeatId = SeatId;
		return Candidate;
	}

	TArray<FOverlapResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(OWSVehicleEntry), false, &Character);
	World->OverlapMultiByObjectType(
		Hits, Character.GetActorLocation(), FQuat::Identity,
		FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllDynamicObjects),
		FCollisionShape::MakeSphere(MaximumEnterDistance), Params);

	APawn* BestVehicle = nullptr;
	float BestDistance = MaximumEnterDistance;
	for (const FOverlapResult& Hit : Hits)
	{
		APawn* Candidate = Cast<APawn>(Hit.GetActor());
		if (RequiredVehicle && Candidate != RequiredVehicle)
		{
			continue;
		}
		UOWSStockVehicleInteractionComponent* CandidateInteraction = Candidate
			? Candidate->FindComponentByClass<UOWSStockVehicleInteractionComponent>() : nullptr;
		// Reject a vehicle only if a *player* already controls it. Parked cars
		// spawn with an AIController (AutoPossessAI), and those should still be
		// enterable — possessing the vehicle auto-evicts the AIController.
		AController* CandidateController = Candidate ? Candidate->GetController() : nullptr;
		if (!Candidate || !CandidateInteraction ||
			(CandidateController && CandidateController->IsPlayerController()) ||
			Candidate == ReentryBlockedVehicle)
		{
			continue;
		}
		FName DoorId;
		float Distance = 0.0f;
		const bool bFaced = CandidateInteraction->FindFacedDoor(
			Character.GetActorLocation(), Character.GetActorForwardVector(),
			MaximumEnterDistance, DoorId, Distance);
		if (!bFaced || Distance >= BestDistance)
		{
			continue;
		}
		FName SeatId;
		const bool bSeat = CandidateInteraction->SelectAvailableSeat(DoorId, true, &Character, SeatId);
		if (!bSeat || !CandidateInteraction->IsControlSeat(SeatId))
		{
			continue;
		}
		BestVehicle = Candidate;
		BestDistance = Distance;
		OutInteraction = CandidateInteraction;
		OutDoorId = DoorId;
		OutSeatId = SeatId;
	}
	return BestVehicle;
}

bool UOWSVehicleInteractionComponent::PlaceCharacterAtDoorExit(
	ACharacter& Character,
	const APawn& Vehicle,
	const UOWSStockVehicleInteractionComponent& Interaction,
	const FName DoorId) const
{
	UWorld* World = GetWorld();
	UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
	FTransform ExitTransform;
	if (!World || !Capsule ||
		!Interaction.GetDoorExitWorldTransform(DoorId, ExitTransform))
	{
		return false;
	}

	const float CapsuleRadius = FMath::Max(1.0f, Capsule->GetScaledCapsuleRadius() - 2.0f);
	const float CapsuleHalfHeight = FMath::Max(
		CapsuleRadius, Capsule->GetScaledCapsuleHalfHeight() - 2.0f);
	const FVector CapsuleAxis = ExitTransform.GetRotation().GetAxisZ();
	const float VerticalCapsuleExtent = CapsuleRadius +
		(Capsule->GetScaledCapsuleHalfHeight() - CapsuleRadius) *
		FMath::Abs(CapsuleAxis.Z);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(OWSDoorExit), false, &Character);
	Params.AddIgnoredActor(&Vehicle);
	FHitResult SupportHit;
	const FVector SupportTraceStart =
		ExitTransform.GetLocation() + FVector(0.0f, 0.0f, 500.0f);
	const FVector SupportTraceEnd =
		ExitTransform.GetLocation() - FVector(0.0f, 0.0f, 1000.0f);
	const bool bHasWalkableSupport = World->LineTraceSingleByChannel(
		SupportHit, SupportTraceStart, SupportTraceEnd, ECC_Pawn, Params) &&
		SupportHit.ImpactNormal.Z >= 0.7f;
	if (!bHasWalkableSupport)
	{
		return false;
	}
	FHitResult VehicleSupportHit;
	const FVector VehicleSupportTraceStart =
		Vehicle.GetActorLocation() + FVector(0.0f, 0.0f, 500.0f);
	const FVector VehicleSupportTraceEnd =
		Vehicle.GetActorLocation() - FVector(0.0f, 0.0f, 1000.0f);
	const bool bVehicleHasWalkableSupport = World->LineTraceSingleByChannel(
		VehicleSupportHit, VehicleSupportTraceStart, VehicleSupportTraceEnd,
		ECC_Pawn, Params) && VehicleSupportHit.ImpactNormal.Z >= 0.7f;
	const float MaximumSupportHeightDelta =
		(Character.GetCharacterMovement()
			? Character.GetCharacterMovement()->MaxStepHeight
			: 45.0f) + 5.0f;
	if (!bVehicleHasWalkableSupport ||
		FMath::Abs(SupportHit.ImpactPoint.Z - VehicleSupportHit.ImpactPoint.Z) >
			MaximumSupportHeightDelta)
	{
		return false;
	}
	FVector GroundedExitLocation = ExitTransform.GetLocation();
	GroundedExitLocation.Z = SupportHit.ImpactPoint.Z + VerticalCapsuleExtent + 2.0f;
	ExitTransform.SetLocation(GroundedExitLocation);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByChannel(
		Overlaps, ExitTransform.GetLocation(), ExitTransform.GetRotation(), ECC_Pawn,
		FCollisionShape::MakeCapsule(CapsuleRadius, CapsuleHalfHeight), Params);
	const float CapsuleBottomZ = ExitTransform.GetLocation().Z - VerticalCapsuleExtent;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		const bool bValidSupportContact =
			Overlap.GetComponent() == SupportHit.GetComponent() &&
			CapsuleBottomZ >= SupportHit.ImpactPoint.Z - 2.0f;
		if (Overlap.bBlockingHit && !bValidSupportContact)
		{
			return false;
		}
	}

	Character.SetActorTransform(
		ExitTransform, false, nullptr, ETeleportType::TeleportPhysics);
	return true;
}

bool UOWSVehicleInteractionComponent::PlaceCharacterAtSafeDoorExit(
	ACharacter& Character,
	const APawn& Vehicle,
	const UOWSStockVehicleInteractionComponent& Interaction) const
{
	if (!OccupiedDoorId.IsNone() &&
		PlaceCharacterAtDoorExit(Character, Vehicle, Interaction, OccupiedDoorId))
	{
		return true;
	}

	TArray<FName> DoorIds;
	Interaction.GetDoorIds(DoorIds);
	for (const FName DoorId : DoorIds)
	{
		if (DoorId != OccupiedDoorId &&
			PlaceCharacterAtDoorExit(Character, Vehicle, Interaction, DoorId))
		{
			return true;
		}
	}

	// Every configured door exit is blocked. Reject the exit so possession and
	// occupancy remain unchanged instead of inventing an unsafe placement.
	return false;
}

void UOWSVehicleInteractionComponent::SetCameraTarget(
	AActor* Target,
	const float BlendSeconds) const
{
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Controller || !Target)
	{
		return;
	}
	Controller->SetViewTargetWithBlend(
		Target, BlendSeconds, EViewTargetBlendFunction::VTBlend_Cubic);
	// An instant switch usually follows a teleport (seat -> outside the door).
	// Without a cut the gameplay camera rig damps that jump as if it were real
	// motion and swings for a dozen frames, which reads as flicker.
	if (BlendSeconds <= 0.0f)
	{
		if (APlayerCameraManager* Manager = Controller->PlayerCameraManager)
		{
			Manager->SetGameCameraCutThisFrame();
		}
	}
}

void UOWSVehicleInteractionComponent::EjectCharacterWithPhysics(
	ACharacter& Character,
	const FVector& VehicleVelocity,
	const FVector& DoorDirection)
{
	USkeletalMeshComponent* Mesh = Character.GetMesh();
	if (!Mesh)
	{
		return;
	}

	// Remember how the mesh was set up so RecoverFromRagdoll can restore it.
	CachedMeshRelativeTransform = Mesh->GetRelativeTransform();
	CachedMeshCollisionProfile = Mesh->GetCollisionProfileName();
	RagdollCharacter = &Character;
	bRagdollActive = true;
	RagdollElapsed = 0.0f;
	// Level the capsule out. The camera anchors to the actor, so any residual
	// pitch/roll here would tip the whole view while the body tumbles.
	Character.SetActorRotation(
		FRotator(0.0f, Character.GetActorRotation().Yaw, 0.0f), ETeleportType::TeleportPhysics);

	// The capsule and movement component would fight the ragdoll, so shut them
	// down and hand control of the body to the physics asset.
	if (UCharacterMovementComponent* Movement = Character.GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}
	if (UCapsuleComponent* Capsule = Character.GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// The camera rig hangs off the skeletal mesh, so it would tumble with the
	// ragdoll. Park it on the capsule first, which trails the body smoothly.
	ParkCameraRigsOnCapsule(Character);
	// Free the mesh from the capsule so the body tumbles independently and the
	// capsule can trail it for the camera.
	Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	Mesh->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
	// Don't explode against the car we're bailing out of; still land on the world.
	Mesh->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Ignore);
	// The tumbling body sits on top of the camera boom's origin. If it blocks the
	// camera probe the arm collapses to minimum length and flickers, so hide the
	// ragdoll from the camera channel and keep the normal walking distance.
	Mesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetSimulatePhysics(true);
	Mesh->SetAllBodiesSimulatePhysics(true);
	Mesh->SetAllBodiesPhysicsBlendWeight(1.0f);
	Mesh->WakeAllRigidBodies();

	// Carry the car's momentum, shove sideways out of the door, and add an
	// upward kick so the driver clears the vehicle instead of scraping along it.
	const FVector LaunchVelocity =
		VehicleVelocity +
		DoorDirection * EjectionSidePush +
		FVector(0.0f, 0.0f, EjectionUpwardBoost);
	Mesh->SetAllPhysicsLinearVelocity(LaunchVelocity);
	// A little spin so the body tumbles rather than sliding rigidly.
	Mesh->SetAllPhysicsAngularVelocityInDegrees(DoorDirection * 180.0f);
}

void UOWSVehicleInteractionComponent::ParkCameraRigsOnCapsule(ACharacter& Character)
{
	USkeletalMeshComponent* Mesh = Character.GetMesh();
	UCapsuleComponent* Capsule = Character.GetCapsuleComponent();
	if (!Mesh || !Capsule)
	{
		return;
	}

	ParkedCameraRigs.Reset();
	ParkedCameraRigTransforms.Reset();
	// Copy the list first: re-attaching mutates the mesh's child array.
	TArray<USceneComponent*> Children = Mesh->GetAttachChildren();
	for (USceneComponent* Child : Children)
	{
		if (!Child || !Child->GetClass()->GetName().Contains(TEXT("Camera")))
		{
			continue;
		}
		const FTransform RelativeToMesh = Child->GetRelativeTransform();
		ParkedCameraRigs.Add(Child);
		ParkedCameraRigTransforms.Add(RelativeToMesh);
		Child->AttachToComponent(
			Capsule, FAttachmentTransformRules::KeepRelativeTransform);
		// Re-create the pose the rig would have if the mesh were still standing
		// in its default place, so framing matches the on-foot camera exactly.
		Child->SetRelativeTransform(RelativeToMesh * CachedMeshRelativeTransform);
	}
}

void UOWSVehicleInteractionComponent::RestoreCameraRigs(ACharacter& Character)
{
	USkeletalMeshComponent* Mesh = Character.GetMesh();
	if (Mesh)
	{
		for (int32 Index = 0; Index < ParkedCameraRigs.Num(); ++Index)
		{
			USceneComponent* Rig = ParkedCameraRigs[Index].Get();
			if (!Rig)
			{
				continue;
			}
			Rig->AttachToComponent(
				Mesh, FAttachmentTransformRules::KeepRelativeTransform);
			Rig->SetRelativeTransform(ParkedCameraRigTransforms[Index]);
		}
	}
	ParkedCameraRigs.Reset();
	ParkedCameraRigTransforms.Reset();
}

void UOWSVehicleInteractionComponent::UpdateRagdollRecovery(const float DeltaTime)
{
	ACharacter* Character = RagdollCharacter.Get();
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (!Mesh)
	{
		bRagdollActive = false;
		return;
	}

	// Trail the collision-free capsule behind the flying body so the camera,
	// which follows the character actor, actually watches the ragdoll. Track the
	// pelvis rather than the component origin (that spins with the root body),
	// and move no faster than a sprint so the camera rig keeps its normal
	// framing instead of lagging and clipping the way it did at car speed.
	const FVector BodyTarget = Mesh->DoesSocketExist(TEXT("pelvis"))
		? Mesh->GetSocketLocation(TEXT("pelvis"))
		: Mesh->GetComponentLocation();
	const FVector AnchorLocation = Character->GetActorLocation();
	const FVector ToBody = BodyTarget - AnchorLocation;
	const float BodyDistance = ToBody.Size();
	const float MaximumStep = RagdollAnchorMaxSpeed * DeltaTime;
	Character->SetActorLocation(
		(BodyDistance > MaximumStep)
			? AnchorLocation + ToBody / BodyDistance * MaximumStep
			: BodyTarget,
		false, nullptr, ETeleportType::None);

	RagdollElapsed += DeltaTime;
	if (RagdollElapsed < RagdollMinimumSeconds)
	{
		return;
	}
	// Wait for the body to stop AND for the camera anchor to have caught up,
	// so standing up never snaps the view across the gap.
	const bool bSettled =
		Mesh->GetPhysicsLinearVelocity().Size() <= RagdollSettleSpeed &&
		BodyDistance <= RagdollAnchorCatchUpDistance;
	if (bSettled || RagdollElapsed >= RagdollMaximumSeconds)
	{
		RecoverFromRagdoll();
	}
}

void UOWSVehicleInteractionComponent::RecoverFromRagdoll()
{
	ACharacter* Character = RagdollCharacter.Get();
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
	if (!Character || !Mesh || !Capsule)
	{
		bRagdollActive = false;
		return;
	}

	// Stand the capsule up where the body came to rest.
	const FVector BodyLocation = Mesh->DoesSocketExist(TEXT("pelvis"))
		? Mesh->GetSocketLocation(TEXT("pelvis"))
		: Mesh->GetComponentLocation();
	Mesh->SetSimulatePhysics(false);
	Mesh->SetAllBodiesSimulatePhysics(false);
	Mesh->SetCollisionProfileName(CachedMeshCollisionProfile);
	Mesh->AttachToComponent(Capsule, FAttachmentTransformRules::KeepRelativeTransform);
	Mesh->SetRelativeTransform(CachedMeshRelativeTransform);
	RestoreCameraRigs(*Character);

	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Character->SetActorLocation(
		BodyLocation + FVector(0.0f, 0.0f, Capsule->GetScaledCapsuleHalfHeight()),
		false, nullptr, ETeleportType::TeleportPhysics);
	if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
	{
		Movement->SetMovementMode(MOVE_Walking);
	}

	bRagdollActive = false;
	RagdollElapsed = 0.0f;
	RagdollCharacter = nullptr;
}

FVector UOWSVehicleInteractionComponent::GetVehicleVelocity() const
{
	const APawn* Vehicle = OccupiedVehicle.Get();
	const UPrimitiveComponent* Body = Vehicle
		? Cast<UPrimitiveComponent>(Vehicle->GetRootComponent()) : nullptr;
	if (Body && Body->IsSimulatingPhysics())
	{
		return Body->GetPhysicsLinearVelocity();
	}
	return Vehicle ? Vehicle->GetVelocity() : FVector::ZeroVector;
}

float UOWSVehicleInteractionComponent::GetVehicleSpeedMph() const
{
	return GetVehicleVelocity().Size() * 0.0223694f;
}

void UOWSVehicleInteractionComponent::ApplyVehicleInputContext(const bool bVehicleActive) const
{
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	ULocalPlayer* LocalPlayer = Controller ? Controller->GetLocalPlayer() : nullptr;
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LocalPlayer
		? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer) : nullptr;
	UInputMappingContext* VehicleContext = LoadObject<UInputMappingContext>(
		nullptr, TEXT("/KinetiForge/Template/Input/IMC_VehicleDefault.IMC_VehicleDefault"));
	if (!InputSubsystem || !VehicleContext)
	{
		return;
	}
	InputSubsystem->RemoveMappingContext(VehicleContext);
	if (bVehicleActive)
	{
		InputSubsystem->AddMappingContext(VehicleContext, 200);
	}
}
