#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "OWSCharacterContactComponent.h"
#include "PhysicsEngine/PhysicalAnimationComponent.h"

// Re-pinning changes physics ownership, not the character's authored collision
// settings. Invoke the production interruption path without touching a live map.
struct FOWSContactRecoveryFixture
{
    UWorld* World = nullptr;
    ACharacter* Character = nullptr;
    UOWSCharacterContactComponent* Contact = nullptr;
    AActor* Obstruction = nullptr;
    ECollisionEnabled::Type OriginalCollision = ECollisionEnabled::NoCollision;

    FOWSContactRecoveryFixture()
    {
        UWorld::InitializationValues Values;
        Values.AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
        if (!World) return;
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Character = World->SpawnActor<ACharacter>(FVector(0., 0., 96.), FRotator::ZeroRotator, Params);
        Character->GetMesh()->SetSkeletalMeshAsset(LoadObject<USkeletalMesh>(nullptr, TEXT(
            "/GASPALS/Characters/UEFN_Mannequin/Meshes/SK_UEFN_Mannequin.SK_UEFN_Mannequin")));
        Character->GetMesh()->SetRelativeLocation(FVector(0., 0., -96.));
        Character->GetMesh()->SetAnimInstanceClass(UAnimInstance::StaticClass());
        Contact = NewObject<UOWSCharacterContactComponent>(Character);
        Character->AddInstanceComponent(Contact); Contact->RegisterComponent();
        Contact->bLogTransitions = false;
        Character->DispatchBeginPlay();
        OriginalCollision = Character->GetCapsuleComponent()->GetCollisionEnabled();
    }
    ~FOWSContactRecoveryFixture() { if (World) World->DestroyWorld(false); }

    bool BeginFall() { return Contact && Contact->BeginFall(); }
    bool PrepareBlockedGetUp()
    {
        auto* Mesh = Character->GetMesh();
        Contact->RecoveryMontage = Contact->GetUpFront.Get();
        if (!Mesh->GetAnimInstance() || !Contact->RecoveryMontage) return false;
        if (Contact->PhysicalAnimation) Contact->PhysicalAnimation->SetStrengthMultiplyer(0.f);
        Mesh->SetAllBodiesSimulatePhysics(false); Mesh->SetSimulatePhysics(false);
        Mesh->SetAllBodiesPhysicsBlendWeight(0.f);
        Mesh->AttachToComponent(Character->GetCapsuleComponent(), FAttachmentTransformRules::KeepRelativeTransform);
        Mesh->SetRelativeTransform(Contact->MeshRelativeTransform);
        Mesh->bPauseAnims = false; Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        if (Mesh->GetAnimInstance()->Montage_Play(Contact->RecoveryMontage, 1.f) <= 0.f) return false;
        Contact->SetState(EOWSCharacterContactState::Recovering);
        if (!Obstruction)
        {
            Obstruction = World->SpawnActor<AActor>();
            auto* Box = NewObject<UBoxComponent>(Obstruction);
            Obstruction->SetRootComponent(Box); Box->SetBoxExtent(FVector(50., 50., 50.));
            Box->SetCollisionProfileName(TEXT("BlockAll")); Box->RegisterComponent();
            Obstruction->SetActorLocation(Character->GetActorLocation());
        }
        return true;
    }
    void InterruptGetUp() { Contact->UpdateRecovery(); }
    bool RetainsOriginalCollision() const { return Contact->CapsuleCollision == OriginalCollision; }
    void RestoreForInspection()
    {
        Contact->RestoreMesh();
        Character->GetCapsuleComponent()->SetCollisionEnabled(Contact->CapsuleCollision);
        Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        Contact->SetState(EOWSCharacterContactState::Upright);
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSContactReblockedRecoveryTest, "OWS.Contact.ReblockedGetUpOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSContactReblockedRecoveryTest::RunTest(const FString&)
{
    FOWSContactRecoveryFixture F;
    if (!TestNotNull(TEXT("Disposable real-rig physics world"), F.World)) return false;
    TestTrue(TEXT("Fixture starts with an enabled collision capsule"), F.OriginalCollision != ECollisionEnabled::NoCollision);
    if (!TestTrue(TEXT("Production fall acquires the actual skeletal physics asset"), F.BeginFall())) return false;
    for (int32 Repeat = 0; Repeat < 3; ++Repeat)
    {
        if (!TestTrue(TEXT("Real floor get-up montage starts before obstruction"), F.PrepareBlockedGetUp())) return false;
        F.InterruptGetUp();
        TestTrue(TEXT("New obstruction returns the character to pinned finite-body physics"), F.Contact->State == EOWSCharacterContactState::Pinned);
        TestTrue(TEXT("Nested fall retains the originally authored capsule collision"), F.RetainsOriginalCollision());
        TestEqual(TEXT("Pinned capsule stays disabled until genuine recovery"), F.Character->GetCapsuleComponent()->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
        TestTrue(TEXT("Pinned main mesh resumes physics instead of becoming an upright wall"), F.Character->GetMesh()->IsSimulatingPhysics(TEXT("pelvis")));
    }
    F.RestoreForInspection();
    TestEqual(TEXT("The saved handoff setting is still the original, not NoCollision"),
        F.Character->GetCapsuleComponent()->GetCollisionEnabled(), F.OriginalCollision);
    return !HasAnyErrors();
}

#endif
