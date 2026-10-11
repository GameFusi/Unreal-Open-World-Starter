#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "OWSStockVehicleInteractionComponent.h"
#include "../OWSVehicleCapacityPolicy.h"
#include "OWSVehicleInteractionComponent.h"

// Issue #144: exercise the actual fallback/entry/exit methods in a disposable
// world. No live map, game instance, private AI, saves or editor play session.
struct FOWSMissingRollFixture
{
    UWorld* World = nullptr;
    APlayerController* Controller = nullptr;
    ACharacter* Character = nullptr;
    APawn* Vehicle = nullptr;
    UBoxComponent* Body = nullptr;
    UOWSStockVehicleInteractionComponent* Seats = nullptr;
    UOWSVehicleInteractionComponent* Control = nullptr;
    UAnimSequenceBase* UnrelatedSequence = nullptr;

    FOWSMissingRollFixture()
    {
        UWorld::InitializationValues Values;
        Values.AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
        if (!World) return;
        Controller = World->SpawnActor<APlayerController>();
        Vehicle = World->SpawnActor<APawn>();
        Body = NewObject<UBoxComponent>(Vehicle);
        Vehicle->AddInstanceComponent(Body); Vehicle->SetRootComponent(Body);
        Body->SetBoxExtent(FVector(100., 50., 40.));
        Body->SetCollisionProfileName(TEXT("PhysicsActor")); Body->RegisterComponent();
        Vehicle->SetActorLocation(FVector(0., 0., 40.));
        Body->SetSimulatePhysics(true); Body->SetMassOverrideInKg(NAME_None, 1500.f);
        Seats = NewObject<UOWSStockVehicleInteractionComponent>(Vehicle);
        Vehicle->AddInstanceComponent(Seats); Seats->RegisterComponent();
        Vehicle->DispatchBeginPlay();
        // OWS currently boards drivers only. Reserve its passenger slot so both
        // authored doors select the driver without importing downstream seating APIs.
        Seats->OccupySeat(TEXT("FrontRight"), World->SpawnActor<AActor>());
        Control = NewObject<UOWSVehicleInteractionComponent>(Controller);
        Controller->AddInstanceComponent(Control); Control->RegisterComponent();
        Control->bLogBailoutTelemetry = false;

        AActor* Ground = World->SpawnActor<AActor>();
        auto* Floor = NewObject<UBoxComponent>(Ground); Ground->SetRootComponent(Floor);
        Floor->SetBoxExtent(FVector(2000., 2000., 10.));
        Floor->SetCollisionProfileName(TEXT("BlockAll")); Floor->RegisterComponent();
        Ground->SetActorLocation(FVector(0., 0., -10.));
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Character = World->SpawnActor<ACharacter>(FVector(600., 600., 100.), FRotator::ZeroRotator, Params);
        auto* MeshAsset = LoadObject<USkeletalMesh>(nullptr, TEXT(
            "/GASPALS/Characters/UEFN_Mannequin/Meshes/SK_UEFN_Mannequin.SK_UEFN_Mannequin"));
        UnrelatedSequence = LoadObject<UAnimSequenceBase>(nullptr, TEXT(
            "/GASPALS/Characters/UEFN_Mannequin/Animations/GetUp/M_Neutral_GetUp_Back.M_Neutral_GetUp_Back"));
        Character->GetMesh()->SetSkeletalMeshAsset(MeshAsset);
        Character->GetMesh()->SetAnimInstanceClass(UAnimInstance::StaticClass());
        Controller->Possess(Character);
        Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    }
    ~FOWSMissingRollFixture() { if (World) World->DestroyWorld(false); }

    bool IsValid() const
    {
        return World && Controller && Character && Vehicle && Body && Seats && Control && UnrelatedSequence &&
            Character->GetMesh()->GetSkeletalMeshAsset() && Character->GetMesh()->GetAnimInstance();
    }
    void RemoveRoll(bool bStaleReference)
    {
        const TSoftObjectPtr<UAnimSequenceBase> Missing = bStaleReference
            ? TSoftObjectPtr<UAnimSequenceBase>(FSoftObjectPath(TEXT(
                "/Game/OWS/Tests/DeliberatelyMissingRoll.DeliberatelyMissingRoll")))
            : TSoftObjectPtr<UAnimSequenceBase>();
        Control->RollLeftAnimation = Missing; Control->RollRightAnimation = Missing;
    }
    void Begin(bool bRightSide)
    {
        Control->OccupiedVehicle = Vehicle;
        Control->ReentryBlockedVehicle = Vehicle;
        Control->ReentryBlockOrigin = Character->GetActorLocation();
        Control->BeginControlledBailout(*Character, FVector(1000., 0., 0.), FVector(0., bRightSide ? 1. : -1., 0.));
        Control->OccupiedVehicle = nullptr;
    }
    void Finish() { Control->FinishControlledBailout(); }
    bool OwnsValidRoll() const
    {
        return Control->bControlledBailoutActive && Control->bControlledBailoutUsesSingleNode &&
            Control->bControlledBailoutOwnsMeshAnimation && Control->bControlledRollTickAfterMovement &&
            Control->ControlledBailoutCharacter == Character && Control->ActiveControlledAnimation;
    }
    bool ReleasedOwnership() const
    {
        return !Control->bControlledBailoutActive && !Control->bControlledBailoutAddedMoveIgnore &&
            !Control->bControlledBailoutUsesSingleNode && !Control->bControlledBailoutOwnsMeshAnimation &&
            !Control->bControlledRollTickAfterMovement && !Control->ControlledBailoutCharacter &&
            !Control->ControlledBailoutSourceVehicle && !Control->ActiveControlledAnimation &&
            !Control->CachedControlledAnimClass && !Control->ReentryBlockedVehicle &&
            Control->ReentryBlockOrigin.IsZero() && Control->ControlledRollKineticEnergyJoules == 0.;
    }
    bool HasMovementPrerequisite() const
    {
        for (const auto& Prerequisite : Control->PrimaryComponentTick.GetPrerequisites())
            if (Prerequisite.Get() == &Character->GetCharacterMovement()->PrimaryComponentTick) return true;
        return false;
    }
    bool Enter(FName DoorId)
    {
        FTransform Door;
        if (!Seats->GetDoorWorldTransform(DoorId, Door)) return false;
        Character->SetActorLocation(Door.GetLocation());
        FText Failure;
        return Control->TryEnterVehicleThroughDoor(Vehicle, DoorId, Failure);
    }
    bool ExitMoving()
    {
        Body->SetPhysicsLinearVelocity(FVector(1788.16, 0., 0.)); // 40 mph, above the roll threshold.
        return Control->TryExitVehicle();
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSMissingRollRestorationTest, "OWS.Vehicle.Bailout.MissingRollRestoration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSMissingRollRestorationTest::RunTest(const FString&)
{
    // Asset-loader messages are expected only for this deliberately absent path.
    AddExpectedError(TEXT("/Game/OWS/Tests/DeliberatelyMissingRoll"), EAutomationExpectedErrorFlags::Contains, 0);
    for (bool bStaleReference : {false, true})
        for (bool bRightSide : {false, true})
            for (bool bExistingInputIgnore : {false, true})
            {
                FOWSMissingRollFixture F;
                if (!TestTrue(TEXT("Isolated fixture and real compatible animation assets"), F.IsValid())) return false;
                F.RemoveRoll(bStaleReference);
                auto* Movement = F.Character->GetCharacterMovement();
                auto* Mesh = F.Character->GetMesh();
                auto* OriginalAnimation = Mesh->GetAnimInstance();
                const auto OriginalMode = Mesh->GetAnimationMode();
                const auto OriginalClass = Mesh->GetAnimClass();
                Movement->BrakingDecelerationWalking = 1234.f; Movement->GroundFriction = 3.5f;
                auto* Montage = OriginalAnimation->PlaySlotAnimationAsDynamicMontage(
                    F.UnrelatedSequence, TEXT("DefaultSlot"), 0.f, .1f);
                if (!TestNotNull(TEXT("Unrelated dynamic montage was playing before the missing roll"), Montage)) return false;
                if (bExistingInputIgnore)
                {
                    F.Controller->SetIgnoreMoveInput(true);
                    F.Controller->SetIgnoreMoveInput(true); // Preserve stacked ownership, not just the boolean.
                }
                const FVector Start = F.Character->GetActorLocation();
                F.Begin(bRightSide);
                TestTrue(TEXT("Unavailable roll releases recovery/animation/reentry ownership immediately"), F.ReleasedOwnership());
                TestFalse(TEXT("No orphaned movement tick prerequisite"), F.HasMovementPrerequisite());
                TestTrue(TEXT("Existing animation instance remains the same"), Mesh->GetAnimInstance() == OriginalAnimation);
                TestTrue(TEXT("Original animation mode and class remain unchanged"), Mesh->GetAnimationMode() == OriginalMode && Mesh->GetAnimClass() == OriginalClass);
                TestTrue(TEXT("Fallback never stops an unrelated dynamic montage"), OriginalAnimation->Montage_IsActive(Montage));
                TestEqual(TEXT("Original braking retained"), Movement->BrakingDecelerationWalking, 1234.f);
                TestEqual(TEXT("Original ground friction retained"), Movement->GroundFriction, 3.5f);
                TestTrue(TEXT("Ordinary locomotion resumes without a teleport or synthetic kick"),
                    Movement->IsMovingOnGround() && Movement->Velocity.Equals(FVector(1000., 0., 0.)) && F.Character->GetActorLocation().Equals(Start));
                TestEqual(TEXT("Movement-input ignore ownership preserved"), F.Controller->IsMoveInputIgnored(), bExistingInputIgnore);
                F.Finish();
                TestTrue(TEXT("Repeated cleanup remains idempotent"), F.ReleasedOwnership() && OriginalAnimation->Montage_IsActive(Montage));
                if (bExistingInputIgnore)
                {
                    F.Controller->SetIgnoreMoveInput(false);
                    TestTrue(TEXT("First pre-existing ignore owner is still present"), F.Controller->IsMoveInputIgnored());
                    F.Controller->SetIgnoreMoveInput(false);
                }
                TestFalse(TEXT("No leaked or over-released movement-input ignore"), F.Controller->IsMoveInputIgnored());
                F.Character->AddMovementInput(FVector::ForwardVector);
                TestTrue(TEXT("Movement input accepted after fallback"), F.Character->GetPendingMovementInputVector().X > 0.);
            }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSMissingRollReentryTest, "OWS.Vehicle.Bailout.MissingRollReentry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSMissingRollReentryTest::RunTest(const FString&)
{
    for (FName DoorId : {FName(TEXT("LeftDoor")), FName(TEXT("RightDoor"))})
    {
        FOWSMissingRollFixture F;
        if (!TestTrue(TEXT("Isolated driver/vehicle/ground fixture"), F.IsValid())) return false;
        F.RemoveRoll(false);
        for (int32 Repeat = 0; Repeat < 3; ++Repeat)
        {
            if (!TestTrue(TEXT("Occupant enters an actual seat through the requested door"), F.Enter(DoorId))) return false;
            if (!TestTrue(TEXT("Moving exit completes despite the unavailable roll"), F.ExitMoving())) return false;
            TestTrue(TEXT("No active bailout or nearby-vehicle blacklist"), F.ReleasedOwnership());
            TestTrue(TEXT("Occupant owns the on-foot pawn and can move"), F.Controller->GetPawn() == F.Character &&
                !F.Controller->IsMoveInputIgnored() && F.Character->GetCharacterMovement()->IsMovingOnGround());
            TestTrue(TEXT("Seat, attachment and visibility restored for reentry"), F.Seats->GetSeatForOccupant(F.Character).IsNone() &&
                !F.Character->GetAttachParentActor() && !F.Character->IsHidden() && F.Character->GetActorEnableCollision());
            FText Failure;
            TestTrue(TEXT("Same vehicle door admits immediate reentry after fallback"),
                F.Control->CanEnterVehicleThroughDoor(F.Vehicle, DoorId, Failure));
        }
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSValidRollOwnershipTest, "OWS.Vehicle.Bailout.ValidRollOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSValidRollOwnershipTest::RunTest(const FString&)
{
    for (bool bRightSide : {false, true})
    {
        FOWSMissingRollFixture F;
        if (!TestTrue(TEXT("Isolated real animation fixture"), F.IsValid())) return false;
        auto* Mesh = F.Character->GetMesh();
        auto* Movement = F.Character->GetCharacterMovement();
        const auto OriginalMode = Mesh->GetAnimationMode();
        const auto OriginalClass = Mesh->GetAnimClass();
        Movement->BrakingDecelerationWalking = 1234.f; Movement->GroundFriction = 3.5f;
        F.Begin(bRightSide);
        TestTrue(TEXT("Available roll still acquires its existing motion and animation ownership"), F.OwnsValidRoll());
        TestTrue(TEXT("Roll still ticks after locomotion"), F.HasMovementPrerequisite());
        TestTrue(TEXT("Valid roll still blocks movement input during its controlled phase"), F.Controller->IsMoveInputIgnored());
        auto* SingleNode = Mesh->GetSingleNodeInstance();
        if (TestNotNull(TEXT("Available roll uses the existing single-node animation path"), SingleNode))
        {
            auto* RuntimeSequence = Cast<UAnimSequence>(SingleNode->GetCurrentAsset());
            TestTrue(TEXT("Runtime roll remains root-locked, with no animation displacement"),
                RuntimeSequence && RuntimeSequence->bForceRootLock && !RuntimeSequence->bEnableRootMotion);
        }
        F.Finish();
        TestTrue(TEXT("Valid recovery releases its ownership and tick prerequisite"), F.ReleasedOwnership() && !F.HasMovementPrerequisite());
        TestFalse(TEXT("Valid recovery returns movement input"), F.Controller->IsMoveInputIgnored());
        TestTrue(TEXT("Valid recovery restores the original animation mode/class"),
            Mesh->GetAnimationMode() == OriginalMode && Mesh->GetAnimClass() == OriginalClass);
        TestEqual(TEXT("Valid recovery restores original braking"), Movement->BrakingDecelerationWalking, 1234.f);
        TestEqual(TEXT("Valid recovery restores original friction"), Movement->GroundFriction, 3.5f);
    }
    return !HasAnyErrors();
}

#endif
