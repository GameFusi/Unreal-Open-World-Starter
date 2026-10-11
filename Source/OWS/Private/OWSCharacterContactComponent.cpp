#include "OWSCharacterContactComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "OWSContactPolicy.h"
#include "OWSContactWorldSubsystem.h"
#include "OWSSelectorComponent.h"
#include "OWSVehicleContactComponent.h"
#include "OWSVehicleInteractionComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicalAnimationComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Sound/SoundAttenuation.h"

namespace
{
constexpr ECollisionChannel FallenBodyChannel = ECC_GameTraceChannel2;

FVector HeadLocation(const ACharacter& Character)
{
    const auto* Mesh = Character.GetMesh();
    return Mesh && Mesh->DoesSocketExist(TEXT("head"))
        ? Mesh->GetSocketLocation(TEXT("head")) : Character.GetPawnViewLocation();
}

double CharacterContactMass(const ACharacter& Character)
{
    const double Measured = Character.GetMesh() ? Character.GetMesh()->GetMass() : 0.;
    return Measured > 0. ? Measured : Character.GetCharacterMovement()->Mass;
}

bool DependsOnContactOrMovement(const FTickFunction& Root, const FTickFunction& Contact, const FTickFunction& Movement)
{
    TArray<const FTickFunction*> Pending { &Root };
    TSet<const FTickFunction*> Visited;
    while (!Pending.IsEmpty())
    {
        const FTickFunction* Tick = Pending.Pop(EAllowShrinking::No);
        if (!Tick || Visited.Contains(Tick)) continue;
        if (Tick == &Contact || Tick == &Movement) return true;
        Visited.Add(Tick);
        for (const FTickPrerequisite& Prerequisite : Tick->GetPrerequisites()) Pending.Add(Prerequisite.Get());
    }
    return false;
}
}

UOWSCharacterContactComponent::UOWSCharacterContactComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
    GetUpFront = TSoftObjectPtr<UAnimMontage>(FSoftObjectPath(TEXT(
        "/GASPALS/Characters/UEFN_Mannequin/Animations/GetUp/M_Neutral_GetUp_Front_Montage_Default.M_Neutral_GetUp_Front_Montage_Default")));
    GetUpBack = TSoftObjectPtr<UAnimMontage>(FSoftObjectPath(TEXT(
        "/GASPALS/Characters/UEFN_Mannequin/Animations/GetUp/M_Neutral_GetUp_Back_Montage_Default.M_Neutral_GetUp_Back_Montage_Default")));
}

void UOWSCharacterContactComponent::BeginPlay()
{
    Super::BeginPlay();
    Character = Cast<ACharacter>(GetOwner());
    if (!Character || UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(FallenBodyChannel) != TEXT("OWSCharacterBody"))
    {
        SetComponentTickEnabled(false); return;
    }
    Character->GetCapsuleComponent()->OnComponentHit.AddUniqueDynamic(this, &ThisClass::OnCapsuleHit);
    Character->GetCharacterMovement()->AddTickPrerequisiteComponent(this);
    // GASPALS can already tick its actor after movement. Contact must precede
    // movement, so depending on that actor would close a tick dependency cycle.
    if (!DependsOnContactOrMovement(Character->PrimaryActorTick, PrimaryComponentTick,
        Character->GetCharacterMovement()->PrimaryComponentTick)) AddTickPrerequisiteActor(Character);
    OrderMovementReflex();
}

void UOWSCharacterContactComponent::OrderMovementReflex()
{
    auto* Movement = Character->GetCharacterMovement();
    TInlineComponentArray<UActorComponent*> Components(Character);
    for (UActorComponent* Component : Components)
    {
        if (!Component || Component == this || Component == PhysicalAnimation || Cast<USceneComponent>(Component) ||
            !Component->PrimaryComponentTick.bCanEverTick || Component->PrimaryComponentTick.TickGroup != TG_PrePhysics) continue;
        // Immediate safety input runs after independent gameplay input producers
        // and before locomotion. Do not introduce cycles with post-movement jobs.
        if (!DependsOnContactOrMovement(Component->PrimaryComponentTick, PrimaryComponentTick,
            Movement->PrimaryComponentTick)) AddTickPrerequisiteComponent(Component);
    }
}

bool UOWSCharacterContactComponent::IsContactAvailable() const
{
    if (!Character || !IsUpright() || Character->GetAttachParentActor() ||
        Character->ActorHasTag(FName(TEXT("OWS.Pose.Seated")))) return false;
    const auto* Movement = Character->GetCharacterMovement();
    const auto* Mesh = Character->GetMesh();
    if (!Movement || !Mesh || Mesh->IsSimulatingPhysics() ||
        (Movement->MovementMode != MOVE_Walking && Movement->MovementMode != MOVE_Falling)) return false;
    const AController* Controller = Character->GetController();
    const auto* Interaction = Controller ? Controller->FindComponentByClass<UOWSVehicleInteractionComponent>() : nullptr;
    return !Interaction || !Interaction->IsCharacterInBailout(Character);
}

void UOWSCharacterContactComponent::SetState(EOWSCharacterContactState Next)
{
    if (State == Next) return;
    if (bLogTransitions && Character)
        UE_LOG(LogTemp, Log, TEXT("[OWSContact] %s state=%d -> %d"),
            *Character->GetName(), static_cast<int32>(State), static_cast<int32>(Next));
    State = Next;
}

void UOWSCharacterContactComponent::ReceiveVehicleContact(UPrimitiveComponent& Body, const FHitResult& Hit, float Dt)
{
    if (!Character || !Character->HasAuthority() || !IsContactAvailable() || Dt <= 0.f) return;
    FVector Direction = (-Hit.ImpactNormal).GetSafeNormal2D();
    if (Direction.IsNearlyZero()) Direction = (Character->GetActorLocation() - Body.Bounds.Origin).GetSafeNormal2D();
    if (Direction.IsNearlyZero()) return;
    Threat = Body.GetOwner(); // Touch is awareness even when the car is behind the character.
    FVector Point = Hit.ImpactPoint;
    if (Hit.bStartPenetrating || Point.ContainsNaN())
        Body.GetClosestPointOnCollision(Character->GetActorLocation(), Point);
    PushFrom(&Body, nullptr, Point, Direction, Dt, Hit.bStartPenetrating ? Hit.PenetrationDepth : 0.f);
}

void UOWSCharacterContactComponent::PushFrom(UPrimitiveComponent* VehicleBody, ACharacter* OtherCharacter,
    const FVector& Point, const FVector& Direction, float Dt, float PenetrationDepth)
{
    auto* Movement = Character->GetCharacterMovement();
    auto* OtherMovement = OtherCharacter ? OtherCharacter->GetCharacterMovement() : nullptr;
    const double CharacterMass = CharacterContactMass(*Character);
    const double OtherMass = OtherCharacter ? CharacterContactMass(*OtherCharacter) : 0.;
    const FVector SourceVelocity = VehicleBody ? VehicleBody->GetPhysicsLinearVelocityAtPoint(Point) :
        (OtherMovement ? OtherMovement->Velocity : FVector::ZeroVector);
    const double ClosingSpeed = FMath::Max(0., FVector::DotProduct(SourceVelocity - Movement->Velocity, Direction));
    if (ClosingSpeed <= UE_SMALL_NUMBER && PenetrationDepth <= UE_SMALL_NUMBER) return;
    AActor* Source = VehicleBody ? VehicleBody->GetOwner() : OtherCharacter;
    const double Now = GetWorld()->GetTimeSeconds();
    if (PressureSource != Source || Now - LastPressureAt > 2. * FMath::Max(Dt, .01f)) PressureDistance = 0.f;
    PressureSource = Source; LastPressureAt = Now;
    // A small touch does not fall on a timer. Continued physical displacement
    // without clearing the contact accumulates a character-sized balance demand.
    PressureDistance += ClosingSpeed * Dt;
    if (bLogTransitions && Now >= NextSampleAt)
    {
        NextSampleAt = Now + FMath::Max(.01f, ContactSampleInterval);
        UE_LOG(LogTemp, Log, TEXT("[OWSContactSample] character=%s source=%s closing_cm_s=%.3f character_mass_kg=%.3f source_mass_kg=%.3f character_v=%s source_point_v=%s pressure_cm=%.3f penetration_cm=%.3f"),
            *Character->GetName(), *GetNameSafe(Source), ClosingSpeed, CharacterMass,
            VehicleBody ? VehicleBody->GetMass() : OtherMass, *Movement->Velocity.ToString(),
            *SourceVelocity.ToString(), PressureDistance, PenetrationDepth);
    }
    const float BalanceTravel = Character->GetCapsuleComponent()->GetScaledCapsuleRadius() * SustainedPushRadiusMultiplier;
    if (ClosingSpeed >= KnockdownClosingSpeed || PressureDistance >= BalanceTravel)
    {
        const FVector PreviousVelocity = Movement->Velocity;
        if (BeginFall())
        {
            // Vehicles now collide with the finite physics asset, not an infinite
            // capsule. Chaos computes the impact; DO NOT add a second launch impulse.
            if (OtherMovement)
            {
                // Upright characters remain kinematic; transfer their finite
                // locomotion momentum once instead of an unbounded CMC push force.
                const double BodyMass = Character->GetMesh()->GetMass();
                const double J = OWSContact::NormalImpulse(ClosingSpeed, OtherMass, BodyMass);
                Character->GetMesh()->SetAllPhysicsLinearVelocity(PreviousVelocity + Direction * (J / FMath::Max(1., BodyMass)));
                OtherMovement->Velocity -= Direction * (J / FMath::Max(1., OtherMass));
            }
            return;
        }
    }
    double SourceMass = OtherMass;
    if (VehicleBody)
        if (FBodyInstance* Body = VehicleBody->GetBodyInstance())
        {
            const FVector LeverCross = FVector::CrossProduct(Point - Body->GetCOMPosition(), Direction);
            SourceMass = OWSContact::EffectiveContactMass(Body->GetBodyMass(),
                Body->GetMassSpaceToWorldSpace().GetRotation().UnrotateVector(LeverCross), Body->GetBodyInertiaTensor());
        }
    const double J = OWSContact::NormalImpulse(ClosingSpeed, SourceMass, CharacterMass);
    if (J > 0.)
    {
        Movement->Velocity += Direction * (J / CharacterMass);
        if (VehicleBody) VehicleBody->AddImpulseAtLocation(-Direction * J, Point);
        else if (OtherMovement) OtherMovement->Velocity -= Direction * (J / OtherMass);
    }
    // Resolve only existing geometric penetration here. Normal displacement is
    // integrated by CharacterMovement; moving twice would invent extra travel.
    if (PenetrationDepth > 0.f)
    {
        FHitResult PushHit;
        Movement->SafeMoveUpdatedComponent(Direction * PenetrationDepth,
            Character->GetActorQuat(), true, PushHit);
    }
}

void UOWSCharacterContactComponent::OnCapsuleHit(UPrimitiveComponent*, AActor* Other,
    UPrimitiveComponent*, FVector, const FHitResult& Hit)
{
    ACharacter* OtherCharacter = Cast<ACharacter>(Other);
    if (!Character || !Character->HasAuthority() || !IsContactAvailable() || !OtherCharacter) return;
    auto* Target = OtherCharacter->FindComponentByClass<UOWSCharacterContactComponent>();
    if (!Target || !Target->IsContactAvailable()) return;
    const FVector TowardOther = (-Hit.ImpactNormal).GetSafeNormal2D();
    if (!TowardOther.IsNearlyZero())
        Target->PushFrom(nullptr, Character, Hit.ImpactPoint, TowardOther,
            GetWorld()->GetDeltaSeconds(), Hit.bStartPenetrating ? Hit.PenetrationDepth : 0.f);
}

bool UOWSCharacterContactComponent::BeginFall()
{
    auto* Mesh = Character->GetMesh();
    auto* Movement = Character->GetCharacterMovement();
    auto* Asset = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
    if (!Mesh || !Asset || Asset->FindBodyIndex(PelvisBody) == INDEX_NONE || !Mesh->GetAnimInstance() ||
        Mesh->LeaderPoseComponent.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("[OWSContact] Cannot take bodily control of %s: missing independent physics/animation mesh"), *Character->GetName());
        return false;
    }
    // Load actual floor get-up clips, never jump/land/stumble or a guessed substitute.
    UAnimMontage* Front = GetUpFront.LoadSynchronous();
    UAnimMontage* Back = GetUpBack.LoadSynchronous();
    if (!Front || !Back || !Front->GetSkeleton() || !Back->GetSkeleton() ||
        !Front->GetSkeleton()->IsCompatibleMesh(Mesh->GetSkeletalMeshAsset()) ||
        !Back->GetSkeleton()->IsCompatibleMesh(Mesh->GetSkeletalMeshAsset()))
    {
        UE_LOG(LogTemp, Error, TEXT("[OWSContact] Compatible floor get-up montages required for %s"), *Character->GetName());
        return false;
    }
    const FVector InheritedVelocity = Movement->Velocity;
    MeshParent = Mesh->GetAttachParent(); MeshSocket = Mesh->GetAttachSocketName();
    MeshRelativeTransform = Mesh->GetRelativeTransform();
    MeshProfile = Mesh->GetCollisionProfileName(); MeshObjectType = Mesh->GetCollisionObjectType();
    MeshResponses = Mesh->GetCollisionResponseToChannels(); MeshCollision = Mesh->GetCollisionEnabled();
    CapsuleCollision = Character->GetCapsuleComponent()->GetCollisionEnabled();
    bSavedPauseAnims = Mesh->bPauseAnims;
    // A noncolliding locomotion mesh may not have runtime bodies yet. The asset
    // validates the pelvis; enabling the contact profile creates those bodies
    // before their original per-body settings are cached.
    Mesh->SetCollisionProfileName(TEXT("OWSContactBody"));
    SavedBodies.Reset();
    for (const USkeletalBodySetup* Setup : Asset->SkeletalBodySetups)
    {
        FBodyInstance* Body = Setup ? Mesh->GetBodyInstance(Setup->BoneName) : nullptr;
        if (!Body) continue;
        FBodySettings& Saved = SavedBodies.AddDefaulted_GetRef();
        Saved.Bone = Setup->BoneName; Saved.AngularDamping = Body->AngularDamping;
        Saved.DepenetrationSpeed = Body->GetMaxDepenetrationVelocity();
        Saved.bOverrideDepenetration = Body->GetOverrideMaxDepenetrationVelocity(); Saved.bCCD = Body->bUseCCD;
    }
    ParkedCameras.Reset(); CameraTransforms.Reset();
    const TArray<USceneComponent*> Children = Mesh->GetAttachChildren();
    for (USceneComponent* Child : Children)
    {
        if (!Child || !Child->GetClass()->GetName().Contains(TEXT("Camera"))) continue;
        ParkedCameras.Add(Child); CameraTransforms.Add(Child->GetRelativeTransform());
        Child->AttachToComponent(Character->GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
    }
    Movement->StopMovementImmediately(); Movement->DisableMovement();
    Character->ConsumeMovementInputVector();
    Character->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
    Mesh->SetCollisionProfileName(TEXT("OWSContactBody"));
    Mesh->bPauseAnims = true;
    Mesh->SetSimulatePhysics(true); Mesh->SetAllBodiesSimulatePhysics(true);
    Mesh->SetAllBodiesPhysicsBlendWeight(1.f);
    Mesh->SetAllPhysicsLinearVelocity(InheritedVelocity);
    for (const FBodySettings& Saved : SavedBodies)
    {
        if (FBodyInstance* Body = Mesh->GetBodyInstance(Saved.Bone))
        {
            Body->AngularDamping = FMath::Max(Saved.AngularDamping, LimbAngularDamping);
            Body->UpdateDampingProperties(); Body->SetUseCCD(true);
            Body->SetMaxDepenetrationVelocity(InitialOverlapDepenetrationSpeed);
        }
    }
    if (!PhysicalAnimation)
    {
        PhysicalAnimation = NewObject<UPhysicalAnimationComponent>(Character, NAME_None, RF_Transient);
        Character->AddInstanceComponent(PhysicalAnimation); PhysicalAnimation->RegisterComponent();
    }
    PhysicalAnimation->SetSkeletalMeshComponent(Mesh);
    PhysicalAnimation->SetStrengthMultiplyer(1.f);
    FPhysicalAnimationData Drive;
    Drive.bIsLocalSimulation = true; Drive.OrientationStrength = LimbOrientationStrength;
    Drive.AngularVelocityStrength = LimbAngularDamping; Drive.MaxAngularForce = 1000.f;
    PhysicalAnimation->ApplyPhysicalAnimationSettingsBelow(PelvisBody, Drive, false);
    Mesh->WakeAllRigidBodies(); bEscaping = false; RecoveryMontage = nullptr;
    SetState(EOWSCharacterContactState::Fallen);
    return true;
}

bool UOWSCharacterContactComponent::RecoverySpace(FVector& OutLocation, bool& OutSupported) const
{
    const auto* Capsule = Character->GetCapsuleComponent();
    const auto* Mesh = Character->GetMesh();
    const FVector Pelvis = Mesh->GetSocketLocation(PelvisBody);
    const float Radius = Capsule->GetScaledCapsuleRadius(), HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    FCollisionQueryParams Params(TEXT("OWSContactRecovery"), false, Character);
    FHitResult Support;
    const bool bHit = GetWorld()->LineTraceSingleByChannel(Support, Pelvis,
        Pelvis - FVector(0., 0., HalfHeight * 2.), ECC_Pawn, Params);
    // Not grounded on another character or a car: those can pin the body, not
    // authorize standing on top of it as a way to escape underneath it.
    OutSupported = bHit && Support.ImpactNormal.Z >= Character->GetCharacterMovement()->GetWalkableFloorZ() &&
        !Cast<ACharacter>(Support.GetActor()) &&
        !(Support.GetActor() && Support.GetActor()->FindComponentByClass<UOWSVehicleContactComponent>()) &&
        Pelvis.Z - Support.ImpactPoint.Z <= Radius * 2.;
    if (!OutSupported) return false;
    OutLocation = FVector(Pelvis.X, Pelvis.Y, Support.ImpactPoint.Z + HalfHeight + .5f);
    return !GetWorld()->OverlapBlockingTestByChannel(OutLocation, FQuat::Identity, ECC_Pawn,
        FCollisionShape::MakeCapsule(Radius, HalfHeight), Params);
}

void UOWSCharacterContactComponent::UpdateRecovery()
{
    auto* Mesh = Character->GetMesh();
    const FVector Pelvis = Mesh->GetSocketLocation(PelvisBody);
    if (State != EOWSCharacterContactState::Recovering)
    {
        // The detached physics mesh remains independent; move only its camera/
        // controller anchor, never drag the body or teleport it out of danger.
        Character->SetActorLocation(FVector(Pelvis.X, Pelvis.Y, Pelvis.Z), false);
        FVector StandLocation; bool bSupported = false;
        const bool bClear = RecoverySpace(StandLocation, bSupported);
        bool bSettled = true;
        for (const FBodySettings& Saved : SavedBodies)
        {
            bSettled &= Mesh->GetPhysicsLinearVelocity(Saved.Bone).SizeSquared() <= FMath::Square(RecoveryLinearSpeed) &&
                Mesh->GetPhysicsAngularVelocityInDegrees(Saved.Bone).SizeSquared() <= FMath::Square(RecoveryAngularSpeedDegrees);
        }
        if (!bClear && bSupported) SetState(EOWSCharacterContactState::Pinned);
        else SetState(EOWSCharacterContactState::Fallen);
        const double Now = GetWorld()->GetTimeSeconds();
        if (bLogTransitions && Now >= NextSampleAt)
        {
            NextSampleAt = Now + FMath::Max(.01f, ContactSampleInterval);
            UE_LOG(LogTemp, Log, TEXT("[OWSContactBody] character=%s mass_kg=%.3f pelvis=%s v_cm_s=%s angular_deg_s=%s gravity_cm_s2=%.3f supported=%d clear=%d settled=%d"),
                *Character->GetName(), Mesh->GetMass(), *Pelvis.ToString(),
                *Mesh->GetPhysicsLinearVelocity(PelvisBody).ToString(),
                *Mesh->GetPhysicsAngularVelocityInDegrees(PelvisBody).ToString(),
                GetWorld()->GetGravityZ(), bSupported, bClear, bSettled);
        }
        if (!OWSContact::CanRecover(bSupported, bClear, bSettled)) return;
        // Determine face-up/down from the current chest orientation relative to
        // the mesh's authored standing forward; preserve horizontal body facing.
        const FQuat PelvisRotation = Mesh->GetSocketQuaternion(PelvisBody);
        const FVector LocalForward = MeshRelativeTransform.GetRotation().Inverse().RotateVector(FVector::ForwardVector);
        const FVector ChestForward = PelvisRotation.RotateVector(LocalForward);
        RecoveryMontage = ChestForward.Z > 0.f ? GetUpBack.Get() : GetUpFront.Get();
        if (!RecoveryMontage || !Mesh->GetAnimInstance()) return;
        FVector Facing = PelvisRotation.RotateVector(FVector::UpVector).GetSafeNormal2D();
        if (Facing.IsNearlyZero()) Facing = Character->GetActorForwardVector();
        Character->SetActorLocationAndRotation(StandLocation, Facing.Rotation(), false);
        if (PhysicalAnimation) PhysicalAnimation->SetStrengthMultiplyer(0.f);
        Mesh->SetAllBodiesSimulatePhysics(false); Mesh->SetSimulatePhysics(false);
        Mesh->SetAllBodiesPhysicsBlendWeight(0.f);
        Mesh->AttachToComponent(MeshParent.Get(), FAttachmentTransformRules::KeepRelativeTransform, MeshSocket);
        Mesh->SetRelativeTransform(MeshRelativeTransform);
        Mesh->bPauseAnims = false;
        // Keep physical Pawn collision disabled until the authored rise ends.
        // Recovery space is checked on EVERY tick, including new pinning contacts.
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        if (Mesh->GetAnimInstance()->Montage_Play(RecoveryMontage, 1.f) <= 0.f)
        {
            UE_LOG(LogTemp, Error, TEXT("[OWSContact] Get-up montage rejected on %s"), *Character->GetName());
            RestoreMesh(); SetState(EOWSCharacterContactState::Upright);
            Character->GetCapsuleComponent()->SetCollisionEnabled(CapsuleCollision);
            Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
            return;
        }
        SetState(EOWSCharacterContactState::Recovering);
    }
    else
    {
        FCollisionQueryParams Params(TEXT("OWSContactGetUp"), false, Character);
        const auto* Capsule = Character->GetCapsuleComponent();
        const bool bBlocked = GetWorld()->OverlapBlockingTestByChannel(Character->GetActorLocation(),
            FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),
                Capsule->GetScaledCapsuleHalfHeight()), Params);
        FHitResult Support;
        const bool bSupported = GetWorld()->LineTraceSingleByChannel(Support, Character->GetActorLocation(),
            Character->GetActorLocation() - FVector(0., 0., Capsule->GetScaledCapsuleHalfHeight() + 2.), ECC_Pawn, Params) &&
            Support.ImpactNormal.Z >= Character->GetCharacterMovement()->GetWalkableFloorZ() &&
            !Cast<ACharacter>(Support.GetActor()) && !(Support.GetActor() &&
                Support.GetActor()->FindComponentByClass<UOWSVehicleContactComponent>());
        if (bBlocked || !bSupported)
        {
            Mesh->GetAnimInstance()->Montage_Stop(0.f, RecoveryMontage);
            const auto OriginalCapsuleCollision = CapsuleCollision;
            RestoreMesh(); SetState(EOWSCharacterContactState::Upright);
            // Return to finite-body simulation where the character already is,
            // not to a timeout escape or an upright collision capsule under a car.
            if (BeginFall())
            {
                // Get-up still had capsule collision disabled. A nested fall
                // must retain the pre-contact setting, not cache NoCollision.
                CapsuleCollision = OriginalCapsuleCollision;
                if (bBlocked) SetState(EOWSCharacterContactState::Pinned);
            }
            return;
        }
        if (Mesh->GetAnimInstance()->Montage_IsPlaying(RecoveryMontage)) return;
        RestoreMesh(); Character->GetCapsuleComponent()->SetCollisionEnabled(CapsuleCollision);
        Character->ConsumeMovementInputVector();
        Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        PressureDistance = 0.f; PressureSource.Reset();
        SetState(EOWSCharacterContactState::Upright);
    }
}

void UOWSCharacterContactComponent::RestoreMesh()
{
    auto* Mesh = Character ? Character->GetMesh() : nullptr;
    if (!Mesh) return;
    if (PhysicalAnimation)
    {
        PhysicalAnimation->SetStrengthMultiplyer(0.f);
        PhysicalAnimation->SetSkeletalMeshComponent(nullptr);
    }
    Mesh->SetAllBodiesSimulatePhysics(false); Mesh->SetSimulatePhysics(false);
    Mesh->SetAllBodiesPhysicsBlendWeight(0.f);
    for (const FBodySettings& Saved : SavedBodies)
    {
        if (FBodyInstance* Body = Mesh->GetBodyInstance(Saved.Bone))
        {
            Body->AngularDamping = Saved.AngularDamping; Body->UpdateDampingProperties();
            Body->SetMaxDepenetrationVelocity(Saved.DepenetrationSpeed);
            Body->SetOverrideMaxDepenetrationVelocity(Saved.bOverrideDepenetration); Body->SetUseCCD(Saved.bCCD);
        }
    }
    Mesh->AttachToComponent(MeshParent.Get(), FAttachmentTransformRules::KeepRelativeTransform, MeshSocket);
    Mesh->SetRelativeTransform(MeshRelativeTransform); Mesh->SetCollisionProfileName(MeshProfile);
    Mesh->SetCollisionObjectType(MeshObjectType); Mesh->SetCollisionResponseToChannels(MeshResponses);
    Mesh->SetCollisionEnabled(MeshCollision); Mesh->bPauseAnims = bSavedPauseAnims;
    for (int32 Index = 0; Index < ParkedCameras.Num(); ++Index)
    {
        if (USceneComponent* Camera = ParkedCameras[Index].Get())
        {
            Camera->AttachToComponent(Mesh, FAttachmentTransformRules::KeepRelativeTransform);
            Camera->SetRelativeTransform(CameraTransforms[Index]);
        }
    }
    ParkedCameras.Reset(); CameraTransforms.Reset(); SavedBodies.Reset();
}

bool UOWSCharacterContactComponent::CanSeeVehicle(const AActor& Vehicle) const
{
    const auto* Selector = Character->FindComponentByClass<UOWSSelectorComponent>();
    if (!Selector) return false;
    const auto* Contact = Vehicle.FindComponentByClass<UOWSVehicleContactComponent>();
    const auto* Body = Contact ? Contact->GetPrimaryBody() : nullptr;
    if (!Body) return false;
    const FVector Eye = HeadLocation(*Character), Forward = Character->GetActorForwardVector();
    const FVector Target = Body->Bounds.Origin;
    const FVector Offset = Target - Eye;
    const double Along = FVector::DotProduct(Offset, Forward);
    bool bInCone = false;
    for (const FOWSSelectorFunction& Function : Selector->SelectorFunctions)
        for (const FOWSRangeSelector& Entry : Function.SelectorStack)
            if (Entry.bEnabled && Entry.Shape == EOWSRangeSelectorShape::Cone &&
                Entry.DetectableObjectTypes.Contains(Body->GetCollisionObjectType()) && Along >= 0. && Along <= Entry.Length &&
                (Offset - Forward * Along).Size() <= Along * FMath::Tan(FMath::DegreesToRadians(Entry.HalfAngleDegrees)))
                bInCone = true;
    if (!bInCone) return false;
    FCollisionQueryParams Params(TEXT("OWSContactVision"), false, Character);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByChannel(Hit, Eye, Target, ECC_Visibility, Params) || Hit.GetActor() == &Vehicle;
}

bool UOWSCharacterContactComponent::CanHearVehicle(const AActor& Vehicle) const
{
    // Listening is a separate sphere, not the interaction reach orb and not
    // omniscient knowledge that a silent vehicle is approaching from behind.
    TInlineComponentArray<UAudioComponent*> Sources(&Vehicle);
    const FVector Ear = HeadLocation(*Character);
    for (const UAudioComponent* Source : Sources)
    {
        if (!Source || !Source->IsPlaying() || Source->VolumeMultiplier <= 0.f) continue;
        float Radius = HearingRadius;
        if (const FSoundAttenuationSettings* Attenuation = Source->GetAttenuationSettingsToApply())
            if (Attenuation->bAttenuate) Radius = FMath::Min(Radius, Attenuation->GetMaxDimension());
        if (FVector::DistSquared(Ear, Source->GetComponentLocation()) <= FMath::Square(Radius))
        {
            FCollisionQueryParams Params(TEXT("OWSContactHearing"), false, Character);
            Params.AddIgnoredActor(&Vehicle);
            FHitResult Occluder;
            if (!GetWorld()->LineTraceSingleByChannel(Occluder, Ear, Source->GetComponentLocation(), ECC_Visibility, Params)) return true;
        }
    }
    return false;
}

bool UOWSCharacterContactComponent::FindEscape(const AActor& Vehicle, FVector& OutDestination) const
{
    auto* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!Nav) return false;
    auto* Capsule = Character->GetCapsuleComponent();
    const FVector Course = Vehicle.GetVelocity().GetSafeNormal2D();
    if (Course.IsNearlyZero()) return false;
    const FVector Side = FVector::CrossProduct(FVector::UpVector, Course);
    const FVector Start = Character->GetActorLocation();
    const double Offset = Vehicle.GetComponentsBoundingBox(true).GetExtent().Size2D() + Capsule->GetScaledCapsuleRadius() * 2.;
    double BestDistance = TNumericLimits<double>::Max();
    for (double Sign : {-1., 1.})
    {
        FNavLocation Projected;
        if (!Nav->ProjectPointToNavigation(Start + Side * Offset * Sign, Projected,
            FVector(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight() * 2.))) continue;
        UNavigationPath* Path = Nav->FindPathToLocationSynchronously(GetWorld(), Start, Projected.Location, Character);
        if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.Num() < 2) continue;
        // Validate every route segment, not just a free endpoint. Keep the capsule
        // off the floor with the same half-height used by CharacterMovement.
        FCollisionQueryParams Params(TEXT("OWSContactEscape"), false, Character);
        bool bClear = true;
        const FVector Lift(0., 0., Capsule->GetScaledCapsuleHalfHeight() + 1.);
        for (int32 Index = 1; Index < Path->PathPoints.Num(); ++Index)
        {
            FHitResult Hit;
            const FVector From = Index == 1 ? Start : Path->PathPoints[Index - 1] + Lift;
            bClear &= !GetWorld()->SweepSingleByChannel(Hit, From, Path->PathPoints[Index] + Lift,
                FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),
                    Capsule->GetScaledCapsuleHalfHeight()), Params);
        }
        const double Distance = Path->GetPathLength();
        if (bClear && Distance < BestDistance)
        {
            BestDistance = Distance;
            // Follow the actual navigable route's next point, not a straight-line
            // shortcut through scenery. Replan on the next awareness interval.
            OutDestination = Path->PathPoints[1];
        }
    }
    return BestDistance < TNumericLimits<double>::Max();
}

void UOWSCharacterContactComponent::UpdateAwareness(float Dt)
{
    auto* Movement = Character->GetCharacterMovement();
    if (!bNpcEvasion || !Character->GetController() ||
        !OWSContact::CanEvade(Character->IsPlayerControlled(), IsContactAvailable(), Movement->IsMovingOnGround()))
    {
        bEscaping = false; Threat.Reset(); ThreatAge = 0.f; return;
    }
    AwarenessElapsed += Dt;
    if (AwarenessElapsed >= AwarenessInterval)
    {
        AwarenessElapsed = 0.f;
        OrderMovementReflex(); // Include gameplay components attached after BeginPlay.
        auto* Registry = GetWorld()->GetSubsystem<UOWSContactWorldSubsystem>();
        AActor* Best = nullptr; double Soonest = ThreatHorizonSeconds;
        if (Registry)
            for (const auto& Entry : Registry->GetVehicles())
            {
                UOWSVehicleContactComponent* Contact = Entry.Get();
                UPrimitiveComponent* Body = Contact ? Contact->GetPrimaryBody() : nullptr;
                if (!Body) continue;
                AActor* Vehicle = Contact->GetOwner();
                double Time = 0.;
                const bool bTouched = PressureSource == Vehicle && GetWorld()->GetTimeSeconds() - LastPressureAt <= AwarenessInterval * 2.;
                if (!bTouched && !CanSeeVehicle(*Vehicle) && !CanHearVehicle(*Vehicle)) continue;
                const double Radius = Body->Bounds.BoxExtent.Size2D() + Character->GetCapsuleComponent()->GetScaledCapsuleRadius();
                if ((bTouched || OWSContact::ThreatTime(Body->Bounds.Origin - Character->GetActorLocation(),
                    Body->GetPhysicsLinearVelocity() - Movement->Velocity, Radius, ThreatHorizonSeconds, Time)) && Time <= Soonest)
                {
                    Best = Vehicle; Soonest = Time;
                }
            }
        if (Threat != Best) { Threat = Best; ThreatAge = 0.f; }
        bEscaping = Best && ThreatAge >= ReactionSeconds && FindEscape(*Best, EscapeDestination);
    }
    if (Threat.IsValid()) ThreatAge += Dt;
    if (!bEscaping || !Threat.IsValid()) return;
    // Only NPC input is replaced. Players can ignore the threat; physical
    // pushing/knockdown still applies, but no agent moves their stick for them.
    Character->ConsumeMovementInputVector();
    Character->AddMovementInput((EscapeDestination - Character->GetActorLocation()).GetSafeNormal2D(), 1.f);
}

void UOWSCharacterContactComponent::TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Tick)
{
    Super::TickComponent(Dt, Type, Tick);
    if (!Character || !Character->HasAuthority() || Dt <= 0.f) return;
    if (IsUpright()) UpdateAwareness(Dt);
    else UpdateRecovery();
}

void UOWSCharacterContactComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Character)
    {
        Character->GetCapsuleComponent()->OnComponentHit.RemoveDynamic(this, &ThisClass::OnCapsuleHit);
        Character->GetCharacterMovement()->RemoveTickPrerequisiteComponent(this);
        if (!IsUpright()) RestoreMesh();
    }
    if (PhysicalAnimation) PhysicalAnimation->DestroyComponent();
    Super::EndPlay(Reason);
}
