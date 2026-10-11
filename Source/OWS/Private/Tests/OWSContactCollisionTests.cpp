#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "OWSCharacterContactComponent.h"
#include "OWSVehicleContactComponent.h"
#include "VehicleSuspensionSolver.h"
#include "VehicleWheelComponent.h"

namespace
{
struct FContactWorld
{
    UWorld* World = nullptr;
    AActor* Vehicle = nullptr;
    UBoxComponent* Body = nullptr;
    UOWSVehicleContactComponent* Contact = nullptr;
    FContactWorld()
    {
        UWorld::InitializationValues Values;
        Values.AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false)
            .CreateAISystem(false).ShouldSimulatePhysics(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
        if (!World) return;
        Vehicle = World->SpawnActor<AActor>();
        Body = NewObject<UBoxComponent>(Vehicle);
        Vehicle->AddInstanceComponent(Body); Vehicle->SetRootComponent(Body);
        Body->SetBoxExtent(FVector(100., 50., 40.));
        Body->SetCollisionProfileName(TEXT("PhysicsActor")); Body->RegisterComponent();
        Body->SetSimulatePhysics(true); Body->SetMassOverrideInKg(NAME_None, 1500.f);
        Contact = NewObject<UOWSVehicleContactComponent>(Vehicle);
        Vehicle->AddInstanceComponent(Contact); Contact->RegisterComponent();
        Vehicle->DispatchBeginPlay();
    }
    ~FContactWorld() { if (World) World->DestroyWorld(false); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSContactCollisionHullTest, "OWS.Contact.CollisionHullIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSContactCollisionHullTest::RunTest(const FString&)
{
    FContactWorld Fixture;
    if (!TestNotNull(TEXT("Isolated physics world"), Fixture.World)) return false;
    TestEqual(TEXT("Physical chassis ignores kinematic Pawn bodies"), Fixture.Body->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);
    TestEqual(TEXT("Finite fallen bodies still collide with the chassis"), Fixture.Body->GetCollisionResponseToChannel(ECC_GameTraceChannel2), ECR_Block);
    TInlineComponentArray<UPrimitiveComponent*> Primitives(Fixture.Vehicle);
    UPrimitiveComponent* Hull = nullptr;
    for (auto* Primitive : Primitives)
        if (Primitive != Fixture.Body && Primitive->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block) Hull = Primitive;
    if (TestNotNull(TEXT("Matching locomotion query hull installed"), Hull))
    {
        TestEqual(TEXT("Query hull never participates in physics resolution"), Hull->GetCollisionEnabled(), ECollisionEnabled::QueryOnly);
        TestFalse(TEXT("CMC cannot apply impulses to a simulated query hull"), Hull->IsSimulatingPhysics());
        TestFalse(TEXT("Query hull is not welded into the physical chassis"), Hull->BodyInstance.bAutoWeld);
        TestEqual(TEXT("Query hull does not duplicate Visibility targeting hits"), Hull->GetCollisionResponseToChannel(ECC_Visibility), ECR_Ignore);
        FHitResult Hit;
        TestTrue(TEXT("Pawn sweep still sees the car as a locomotion obstacle"), Fixture.World->SweepSingleByChannel(
            Hit, FVector(-300., 0., 0.), FVector(300., 0., 0.), FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(10.)));
        TestTrue(TEXT("Locomotion hits only the nonphysical proxy"), Hit.GetComponent() == Hull);
    }
    Fixture.Contact->DestroyComponent();
    TestEqual(TEXT("Removing the component restores the authored chassis response"), Fixture.Body->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSContactSlowPushTest, "OWS.Contact.SlowPushConservation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSContactSlowPushTest::RunTest(const FString&)
{
    FContactWorld Fixture;
    if (!TestNotNull(TEXT("Isolated physics world"), Fixture.World)) return false;
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ACharacter* Character = Fixture.World->SpawnActor<ACharacter>(FVector(150., 0., 96.), FRotator::ZeroRotator, Params);
    auto* Contact = NewObject<UOWSCharacterContactComponent>(Character);
    Character->AddInstanceComponent(Contact); Contact->RegisterComponent(); Character->DispatchBeginPlay();
    Character->GetCharacterMovement()->Mass = 100.f;
    Character->GetCharacterMovement()->Velocity = FVector::ZeroVector;
    Fixture.Body->SetPhysicsLinearVelocity(FVector(50., 0., 0.));
    FHitResult Hit;
    Hit.ImpactPoint = FVector::ZeroVector; // Center-of-mass case: no off-center torque.
    Hit.ImpactNormal = FVector(-1., 0., 0.);
    Contact->ReceiveVehicleContact(*Fixture.Body, Hit, .01f);
    const double AfterCar = Fixture.Body->GetPhysicsLinearVelocity().X;
    const double AfterPerson = Character->GetCharacterMovement()->Velocity.X;
    TestTrue(TEXT("Slow contact remains upright"), Contact->IsUpright());
    TestTrue(TEXT("Character is pushed, not an immovable wall"), AfterPerson > 0.);
    TestTrue(TEXT("Vehicle continues instead of bouncing or stopping"), AfterCar > 0.);
    TestTrue(TEXT("Production contact conserves momentum"), FMath::IsNearlyEqual(1500. * 50., 1500. * AfterCar + 100. * AfterPerson, .1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSContactWheelSurfaceTest, "OWS.Contact.WheelsExcludeCharacters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSContactWheelSurfaceTest::RunTest(const FString&)
{
    FContactWorld Fixture;
    if (!TestNotNull(TEXT("Isolated physics world"), Fixture.World)) return false;
    UVehicleWheelComponent* Wheel = NewObject<UVehicleWheelComponent>(Fixture.Vehicle);
    struct FTestSuspensionSolver : FVehicleSuspensionSolver
    {
        using FVehicleSuspensionSolver::ResponseParams;
    };
    FTestSuspensionSolver Solver;
    if (!TestTrue(TEXT("Initialize production suspension query configuration"), Solver.Initialize(Wheel))) return false;
    TestEqual(TEXT("Upright characters are not wheel support surfaces"), Solver.ResponseParams.CollisionResponse.GetResponse(ECC_Pawn), ECR_Ignore);
    TestEqual(TEXT("Fallen characters are not artificial suspension ramps"), Solver.ResponseParams.CollisionResponse.GetResponse(ECC_GameTraceChannel2), ECR_Ignore);
    TestEqual(TEXT("Static roads/terrain remain support surfaces"), Solver.ResponseParams.CollisionResponse.GetResponse(ECC_WorldStatic), ECR_Block);
    TestEqual(TEXT("Other dynamic surface responses are preserved"), Solver.ResponseParams.CollisionResponse.GetResponse(ECC_WorldDynamic), ECR_Block);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOWSContactTickOrderTest, "OWS.Contact.PostMovementActorTickOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOWSContactTickOrderTest::RunTest(const FString&)
{
    FContactWorld Fixture;
    if (!TestNotNull(TEXT("Isolated physics world"), Fixture.World)) return false;
    ACharacter* Character = Fixture.World->SpawnActor<ACharacter>();
    auto* Movement = Character->GetCharacterMovement();
    // Reproduce the authored GASPALS actor's post-movement tick dependency.
    Character->AddTickPrerequisiteComponent(Movement);
    auto* Contact = NewObject<UOWSCharacterContactComponent>(Character);
    Character->AddInstanceComponent(Contact); Contact->RegisterComponent(); Character->DispatchBeginPlay();
    bool bMovementAfterContact = false;
    for (const FTickPrerequisite& Prerequisite : Movement->PrimaryComponentTick.GetPrerequisites())
        bMovementAfterContact |= Prerequisite.Get() == &Contact->PrimaryComponentTick;
    TestTrue(TEXT("Contact still runs before locomotion"), bMovementAfterContact);
    for (const FTickPrerequisite& Prerequisite : Contact->PrimaryComponentTick.GetPrerequisites())
        TestTrue(TEXT("Contact never depends on a post-movement actor"), Prerequisite.Get() != &Character->PrimaryActorTick);
    bool bActorAfterMovement = false;
    for (const FTickPrerequisite& Prerequisite : Character->PrimaryActorTick.GetPrerequisites())
        bActorAfterMovement |= Prerequisite.Get() == &Movement->PrimaryComponentTick;
    TestTrue(TEXT("The actor's authored order remains intact"), bActorAfterMovement);
    return true;
}
#endif
