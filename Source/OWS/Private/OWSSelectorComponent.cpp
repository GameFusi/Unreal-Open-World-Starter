#include "OWSSelectorComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/OverlapResult.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "OWSInteractionTargetComponent.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	TArray<TEnumAsByte<ECollisionChannel>> MakeDiagnosticObjectTypes()
	{
		return {
			ECC_WorldStatic,
			ECC_WorldDynamic,
			ECC_Pawn,
			ECC_PhysicsBody,
			ECC_Vehicle,
			ECC_GameTraceChannel2 // OWSCharacterBody: fallen characters retain targeting.
		};
	}

	FOWSRangeSelector MakeSphereSelector(const FName Name, const float Radius)
	{
		FOWSRangeSelector Selector;
		Selector.Name = Name;
		Selector.Shape = EOWSRangeSelectorShape::Sphere;
		Selector.Radius = Radius;
		Selector.bRequiresFacing = false;
		Selector.DetectableObjectTypes = MakeDiagnosticObjectTypes();
		return Selector;
	}

	FOWSRangeSelector MakeConeSelector(
		const FName Name,
		const float Length,
		const float HalfAngleDegrees)
	{
		FOWSRangeSelector Selector;
		Selector.Name = Name;
		Selector.Shape = EOWSRangeSelectorShape::Cone;
		Selector.Length = Length;
		Selector.HalfAngleDegrees = HalfAngleDegrees;
		Selector.bRequiresFacing = true;
		Selector.DetectableObjectTypes = MakeDiagnosticObjectTypes();
		return Selector;
	}

	void GetCharacterHeadTransform(const AActor& Owner, FVector& OutLocation, FVector& OutForward)
	{
		OutLocation = Owner.GetActorLocation();
		OutForward = Owner.GetActorForwardVector();
		if (const ACharacter* Character = Cast<ACharacter>(&Owner))
		{
			if (const USkeletalMeshComponent* Mesh = Character->GetMesh())
			{
				if (Mesh->DoesSocketExist(TEXT("head")))
				{
					OutLocation = Mesh->GetSocketLocation(TEXT("head"));
				}
			}
		}
		OutForward = OutForward.GetSafeNormal();
	}
}

UOWSSelectorComponent::UOWSSelectorComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	bTickInEditor = true;
	PrecisionRayObjectTypes = MakeDiagnosticObjectTypes();
	EnsureDefaultConfiguration();
}

void UOWSSelectorComponent::EnsureDefaultConfiguration()
{
	if (!SelectorFunctions.IsEmpty())
	{
		return;
	}

	FOWSSelectorFunction Activate;
	Activate.Name = TEXT("Activate");
	Activate.ActivationKeys = { EKeys::F, EKeys::Gamepad_FaceButton_Left };
	Activate.SelectorStack = {
		MakeSphereSelector(TEXT("Reach Orb"), 125.0f),
		MakeConeSelector(TEXT("Short Wide Cone"), 1500.0f, 45.0f),
		MakeConeSelector(TEXT("Long Narrow Cone"), 4000.0f, 25.0f)
	};
	SelectorFunctions.Add(MoveTemp(Activate));
}

void UOWSSelectorComponent::BeginPlay()
{
	Super::BeginPlay();
	// Extend only the former stock category list for the new bodily contact
	// channel. Developer-authored filters remain unchanged.
	auto IncludeStockContactBody = [](TArray<TEnumAsByte<ECollisionChannel>>& Types)
	{
		if (Types.Num() == 5 && Types.Contains(ECC_WorldStatic) && Types.Contains(ECC_WorldDynamic) &&
			Types.Contains(ECC_Pawn) && Types.Contains(ECC_PhysicsBody) && Types.Contains(ECC_Vehicle))
			Types.Add(ECC_GameTraceChannel2);
	};
	IncludeStockContactBody(PrecisionRayObjectTypes);
	for (FOWSSelectorFunction& Function : SelectorFunctions)
		for (FOWSRangeSelector& Entry : Function.SelectorStack) IncludeStockContactBody(Entry.DetectableObjectTypes);
	EnsureDefaultConfiguration();
	UE_LOG(LogTemp, Display, TEXT("[OWSSelector] BeginPlay owner=%s class=%s world=%d"),
		GetOwner() ? *GetOwner()->GetName() : TEXT("none"),
		GetOwner() ? *GetOwner()->GetClass()->GetName() : TEXT("none"),
		GetWorld() ? static_cast<int32>(GetWorld()->WorldType) : -1);
}

void UOWSSelectorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (DebugReadoutContainer.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(DebugReadoutContainer.ToSharedRef());
	}
	DebugReadoutText.Reset();
	DebugReadoutContainer.Reset();
	DetectedActor = nullptr;
	DetectedInteractionTarget = nullptr;
	AwarenessActors.Reset();
	LastLoggedDetectedActor = nullptr;
	LastLoggedDetectedInteractionTarget = nullptr;
	bActivationKeyWasDown = false;
	Super::EndPlay(EndPlayReason);
}

void UOWSSelectorComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (World->WorldType == EWorldType::Editor)
	{
		DrawEditorVisualization();
		return;
	}

	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	APlayerController* Controller = Character
		? Cast<APlayerController>(Character->GetController())
		: nullptr;
	if (!Controller || !Controller->IsLocalController())
	{
		return;
	}

	EnsureDebugWidget(*Controller);
	UpdateDetectedActor();
	UpdateActivation(*Controller);
	SetDebugReadoutText(GetDetectedActorName());
}

FText UOWSSelectorComponent::GetDetectedActorName() const
{
	if (DetectedInteractionTarget)
	{
		return DetectedInteractionTarget->GetInteractionDisplayName();
	}
	return DetectedActor
		? FText::FromString(DetectedActor->GetActorNameOrLabel())
		: FText::GetEmpty();
}

bool UOWSSelectorComponent::HasValidSelectorStacks() const
{
	if (SelectorFunctions.IsEmpty())
	{
		return false;
	}
	for (const FOWSSelectorFunction& Function : SelectorFunctions)
	{
		if (!Function.SelectorStack.ContainsByPredicate(
			[](const FOWSRangeSelector& Selector) { return Selector.bEnabled; }))
		{
			return false;
		}
	}
	return true;
}

TArray<AActor*> UOWSSelectorComponent::GetAwarenessActors() const
{
	TArray<AActor*> Result;
	Result.Reserve(AwarenessActors.Num());
	for (AActor* Actor : AwarenessActors)
	{
		if (Actor)
		{
			Result.Add(Actor);
		}
	}
	return Result;
}

bool UOWSSelectorComponent::IsDebugReadoutMounted() const
{
	return DebugReadoutContainer.IsValid() &&
		DebugReadoutContainer->GetCachedGeometry().GetLocalSize().SizeSquared() > 1.0f;
}

void UOWSSelectorComponent::EnsureDebugWidget(APlayerController&)
{
	if (DebugReadoutContainer.IsValid())
	{
		return;
	}
	if (!GEngine || !GEngine->GameViewport)
	{
		UE_LOG(LogTemp, Error, TEXT("[OWSSelector] No game viewport for debug readout on %s"),
			GetOwner() ? *GetOwner()->GetName() : TEXT("none"));
		return;
	}

	SAssignNew(DebugReadoutText, STextBlock)
		.Text(FText::FromString(TEXT("Detected: None")))
		.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 18))
		.ColorAndOpacity(FLinearColor(0.92f, 0.97f, 1.0f, 1.0f))
		.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f))
		.ShadowOffset(FVector2D(1.5f, 1.5f));

	SAssignNew(DebugReadoutContainer, SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(32.0f))
		[
			DebugReadoutText.ToSharedRef()
		];

	GEngine->GameViewport->AddViewportWidgetContent(DebugReadoutContainer.ToSharedRef(), 25);
	UE_LOG(LogTemp, Display, TEXT("[OWSSelector] Debug readout added to viewport for %s"),
		GetOwner() ? *GetOwner()->GetName() : TEXT("none"));
}

void UOWSSelectorComponent::SetDebugReadoutText(const FText& ActorName)
{
	if (!DebugReadoutText.IsValid())
	{
		return;
	}
	DebugReadoutText->SetText(ActorName.IsEmpty()
		? FText::FromString(TEXT("Detected: None"))
		: FText::Format(NSLOCTEXT("OWSSelector", "DetectedActor", "Detected: {0}"), ActorName));
}

bool UOWSSelectorComponent::IsInsideRange(
	const AActor& Candidate,
	const FOWSRangeSelector& Selector,
	const FVector& Origin,
	const FVector& Forward) const
{
	FVector BoundsOrigin;
	FVector BoundsExtent;
	Candidate.GetActorBounds(false, BoundsOrigin, BoundsExtent, true);
	return IsLocationInsideRange(
		BoundsOrigin, BoundsExtent.Size(), Selector, Origin, Forward);
}

bool UOWSSelectorComponent::IsLocationInsideRange(
	const FVector& Location,
	const float Radius,
	const FOWSRangeSelector& Selector,
	const FVector& Origin,
	const FVector& Forward)
{
	const FVector Offset = Location - Origin;
	if (Selector.Shape == EOWSRangeSelectorShape::Sphere)
	{
		return Offset.SizeSquared() <= FMath::Square(Selector.Radius + Radius);
	}

	const float Along = FVector::DotProduct(Offset, Forward);
	if (Along < -Radius || Along > Selector.Length + Radius)
	{
		return false;
	}
	const float Lateral = (Offset - Forward * Along).Size();
	const float ConeRadius = FMath::Max(0.0f, Along) *
		FMath::Tan(FMath::DegreesToRadians(Selector.HalfAngleDegrees));
	return Lateral <= ConeRadius + Radius;
}

bool UOWSSelectorComponent::IsActorOrOwnedBy(const AActor* HitActor, const AActor& Candidate)
{
	for (const AActor* Actor = HitActor; Actor; Actor = Actor->GetOwner())
	{
		if (Actor == &Candidate)
		{
			return true;
		}
	}
	return false;
}

bool UOWSSelectorComponent::ResolvePrecisionRay(
	APlayerController& Controller,
	FVector& OutOrigin,
	FVector& OutDirection,
	float& OutLength) const
{
	OutLength = PrecisionRayLength;
	if (OutLength <= 0.0f || !GetOwner())
	{
		return false;
	}

	if (Controller.IsInputKeyDown(EKeys::Gamepad_LeftTrigger) && Controller.PlayerCameraManager)
	{
		OutOrigin = Controller.PlayerCameraManager->GetCameraLocation();
		OutDirection = Controller.PlayerCameraManager->GetCameraRotation().Vector();
	}
	else
	{
		GetCharacterHeadTransform(*GetOwner(), OutOrigin, OutDirection);
		OutDirection = Controller.GetControlRotation().Vector();
	}
	return true;
}

void UOWSSelectorComponent::UpdateAwarenessActors(
	const ACharacter& Character,
	const FVector& HeadOrigin,
	const FVector& HeadForward)
{
	AwarenessActors.Reset();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector CharacterOrigin = Character.GetActorLocation();
	for (const FOWSSelectorFunction& Function : SelectorFunctions)
	{
		for (const FOWSRangeSelector& Selector : Function.SelectorStack)
		{
			if (!Selector.bEnabled || Selector.DetectableObjectTypes.IsEmpty())
			{
				continue;
			}
			FCollisionObjectQueryParams ObjectParams;
			for (const ECollisionChannel Channel : Selector.DetectableObjectTypes)
			{
				ObjectParams.AddObjectTypesToQuery(Channel);
			}
			const FVector RangeOrigin = Selector.Shape == EOWSRangeSelectorShape::Sphere
				? CharacterOrigin
				: HeadOrigin;
			const float GatherRadius = Selector.Shape == EOWSRangeSelectorShape::Sphere
				? Selector.Radius
				: Selector.Length;
			TArray<FOverlapResult> Overlaps;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(OWSSelectorAwareness), false, &Character);
			World->OverlapMultiByObjectType(
				Overlaps, RangeOrigin, FQuat::Identity, ObjectParams,
				FCollisionShape::MakeSphere(GatherRadius), Params);
			for (const FOverlapResult& Overlap : Overlaps)
			{
				AActor* Candidate = Overlap.GetActor();
				if (!Candidate || Candidate == &Character ||
					!IsInsideRange(*Candidate, Selector, RangeOrigin, HeadForward))
				{
					continue;
				}
				TArray<UOWSInteractionTargetComponent*> Targets;
				Candidate->GetComponents(UOWSInteractionTargetComponent::StaticClass(), Targets);
				if (Targets.ContainsByPredicate(
					[](const UOWSInteractionTargetComponent* Target)
					{
						return Target && Target->bInteractionEnabled;
					}))
				{
					AwarenessActors.Add(Candidate);
				}
			}
		}
	}
}

void UOWSSelectorComponent::UpdateDetectedActor()
{
	DetectedActor = nullptr;
	DetectedInteractionTarget = nullptr;
	ACharacter* Character = Cast<ACharacter>(GetOwner());
	APlayerController* Controller = Character
		? Cast<APlayerController>(Character->GetController())
		: nullptr;
	UWorld* World = GetWorld();
	if (!Character || !Controller || !World)
	{
		return;
	}

	FVector HeadOrigin;
	FVector HeadForward;
	GetCharacterHeadTransform(*Character, HeadOrigin, HeadForward);
	HeadForward = Controller->GetControlRotation().Vector();
	UpdateAwarenessActors(*Character, HeadOrigin, HeadForward);

	FCollisionObjectQueryParams PrecisionObjectParams;
	for (const ECollisionChannel Channel : PrecisionRayObjectTypes)
	{
		PrecisionObjectParams.AddObjectTypesToQuery(Channel);
	}

	FVector PrecisionOrigin;
	FVector PrecisionDirection;
	float PrecisionLength = 0.0f;
	if (ResolvePrecisionRay(*Controller, PrecisionOrigin, PrecisionDirection, PrecisionLength))
	{
		const FVector PrecisionEnd = PrecisionOrigin + PrecisionDirection * PrecisionLength;
		TArray<FHitResult> Hits;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(OWSSelectorPrecision), true, Character);
		World->LineTraceMultiByObjectType(
			Hits, PrecisionOrigin, PrecisionEnd, PrecisionObjectParams, Params);
		for (const FHitResult& Hit : Hits)
		{
			AActor* Candidate = Hit.GetActor();
			if (!Candidate || Candidate == Character)
			{
				continue;
			}
			DetectedActor = Candidate;
			TArray<UOWSInteractionTargetComponent*> InteractionTargets;
			Candidate->GetComponents(
				UOWSInteractionTargetComponent::StaticClass(), InteractionTargets);
			float ClosestTargetDistanceSquared = TNumericLimits<float>::Max();
			for (UOWSInteractionTargetComponent* InteractionTarget : InteractionTargets)
			{
				if (InteractionTarget && InteractionTarget->bInteractionEnabled)
				{
					const float DistanceSquared = FVector::DistSquared(
						InteractionTarget->GetComponentLocation(), Hit.ImpactPoint);
					if (DistanceSquared < ClosestTargetDistanceSquared)
					{
						ClosestTargetDistanceSquared = DistanceSquared;
						DetectedInteractionTarget = InteractionTarget;
					}
				}
			}
			if (DetectedActor != LastLoggedDetectedActor ||
				DetectedInteractionTarget != LastLoggedDetectedInteractionTarget)
			{
				UE_LOG(LogTemp, Display, TEXT("[OWSSelector] Detected=%s interaction=%s"),
					*DetectedActor->GetActorNameOrLabel(),
					DetectedInteractionTarget
						? *DetectedInteractionTarget->InteractionId.ToString()
						: TEXT("none"));
				LastLoggedDetectedActor = DetectedActor;
				LastLoggedDetectedInteractionTarget = DetectedInteractionTarget;
			}
			return;
		}
	}
	if (DetectedActor != LastLoggedDetectedActor ||
		DetectedInteractionTarget != LastLoggedDetectedInteractionTarget)
	{
		UE_LOG(LogTemp, Display, TEXT("[OWSSelector] Detected=%s"),
			DetectedActor ? *DetectedActor->GetActorNameOrLabel() : TEXT("None"));
		LastLoggedDetectedActor = DetectedActor;
		LastLoggedDetectedInteractionTarget = DetectedInteractionTarget;
	}
}

AActor* UOWSSelectorComponent::RefreshSelection()
{
	UpdateDetectedActor();
	SetDebugReadoutText(GetDetectedActorName());
	return DetectedActor.Get();
}

bool UOWSSelectorComponent::ActivateCurrentTarget()
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	APlayerController* Controller = Character
		? Cast<APlayerController>(Character->GetController())
		: nullptr;
	FText FailureReason;
	if (!Controller || !DetectedActor || !DetectedInteractionTarget)
	{
		FailureReason = NSLOCTEXT(
			"OWSSelector", "NoInteractionTarget",
			"The detected object has no enabled OWS interaction target.");
		OnActivateTargetFailed.Broadcast(
			DetectedActor.Get(), DetectedInteractionTarget.Get(), FailureReason);
		return false;
	}
	if (!DetectedInteractionTarget->TryActivate(Controller, FailureReason))
	{
		if (FailureReason.IsEmpty())
		{
			FailureReason = NSLOCTEXT(
				"OWSSelector", "ActivationRejected", "Activation was rejected.");
		}
		UE_LOG(LogTemp, Display, TEXT("[OWSSelector] Activate rejected target=%s interaction=%s reason=%s"),
			*DetectedActor->GetActorNameOrLabel(),
			*DetectedInteractionTarget->InteractionId.ToString(),
			*FailureReason.ToString());
		OnActivateTargetFailed.Broadcast(
			DetectedActor.Get(), DetectedInteractionTarget.Get(), FailureReason);
		return false;
	}
	UE_LOG(LogTemp, Display, TEXT("[OWSSelector] Activate target=%s interaction=%s"),
		*DetectedActor->GetActorNameOrLabel(),
		*DetectedInteractionTarget->InteractionId.ToString());
	OnActivateTarget.Broadcast(DetectedActor.Get(), DetectedInteractionTarget.Get());
	return true;
}

void UOWSSelectorComponent::UpdateActivation(APlayerController& Controller)
{
	bool bActivationDown = false;
	for (const FOWSSelectorFunction& Function : SelectorFunctions)
	{
		for (const FKey& Key : Function.ActivationKeys)
		{
			bActivationDown |= Controller.IsInputKeyDown(Key);
		}
	}

	if (bActivationDown && !bActivationKeyWasDown && DetectedActor && GetOwner())
	{
		ActivateCurrentTarget();
	}
	bActivationKeyWasDown = bActivationDown;
}

void UOWSSelectorComponent::DrawEditorVisualization() const
{
#if WITH_EDITOR
	if (!bShowEditorVisualization || !GetOwner() || !GetWorld())
	{
		return;
	}
	FVector HeadOrigin;
	FVector HeadForward;
	GetCharacterHeadTransform(*GetOwner(), HeadOrigin, HeadForward);
	for (const FOWSSelectorFunction& Function : SelectorFunctions)
	{
		for (const FOWSRangeSelector& Selector : Function.SelectorStack)
		{
			if (!Selector.bEnabled)
			{
				continue;
			}
			if (Selector.Shape == EOWSRangeSelectorShape::Sphere)
			{
				DrawDebugSphere(GetWorld(), GetOwner()->GetActorLocation(), Selector.Radius,
					24, EditorVisualizationColor, false, -1.0f, 0, 1.5f);
			}
			else
			{
				const float AngleRadians = FMath::DegreesToRadians(Selector.HalfAngleDegrees);
				DrawDebugCone(GetWorld(), HeadOrigin, HeadForward, Selector.Length,
					AngleRadians, AngleRadians, 24, EditorVisualizationColor,
					false, -1.0f, 0, 1.5f);
			}
		}
	}
#endif
}
