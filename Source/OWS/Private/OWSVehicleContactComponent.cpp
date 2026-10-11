#include "OWSVehicleContactComponent.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "OWSCharacterContactComponent.h"

UOWSVehicleContactComponent::UOWSVehicleContactComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

namespace
{
UPrimitiveComponent* MakeContactQuery(AActor& Owner, UPrimitiveComponent& Source)
{
    UPrimitiveComponent* Query = nullptr;
    if (auto* Mesh = Cast<UStaticMeshComponent>(&Source))
    {
        auto* Copy = NewObject<UStaticMeshComponent>(&Owner, NAME_None, RF_Transient);
        Copy->SetStaticMesh(Mesh->GetStaticMesh()); Query = Copy;
    }
    else if (auto* SkeletalMesh = Cast<USkeletalMeshComponent>(&Source))
    {
        auto* Copy = NewObject<USkeletalMeshComponent>(&Owner, NAME_None, RF_Transient);
        Copy->SetSkeletalMeshAsset(SkeletalMesh->GetSkeletalMeshAsset());
        Copy->SetPhysicsAsset(SkeletalMesh->GetPhysicsAsset());
        Copy->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
        Copy->SetLeaderPoseComponent(SkeletalMesh); Query = Copy;
    }
    else if (auto* Box = Cast<UBoxComponent>(&Source))
    {
        auto* Copy = NewObject<UBoxComponent>(&Owner, NAME_None, RF_Transient);
        Copy->SetBoxExtent(Box->GetUnscaledBoxExtent()); Query = Copy;
    }
    else if (auto* Sphere = Cast<USphereComponent>(&Source))
    {
        auto* Copy = NewObject<USphereComponent>(&Owner, NAME_None, RF_Transient);
        Copy->SetSphereRadius(Sphere->GetUnscaledSphereRadius()); Query = Copy;
    }
    else if (auto* Capsule = Cast<UCapsuleComponent>(&Source))
    {
        auto* Copy = NewObject<UCapsuleComponent>(&Owner, NAME_None, RF_Transient);
        Copy->SetCapsuleSize(Capsule->GetUnscaledCapsuleRadius(), Capsule->GetUnscaledCapsuleHalfHeight()); Query = Copy;
    }
    if (!Query) return nullptr;
    Owner.AddInstanceComponent(Query);
    Query->BodyInstance.bAutoWeld = false;
    Query->SetupAttachment(&Source);
    Query->SetRelativeTransform(FTransform::Identity);
    Query->SetMobility(EComponentMobility::Movable);
    Query->SetHiddenInGame(true); Query->SetVisibility(false);
    Query->SetCanEverAffectNavigation(false);
    Query->SetCollisionObjectType(Source.GetCollisionObjectType());
    Query->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Query->SetCollisionResponseToAllChannels(ECR_Ignore);
    Query->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
    // Preserve authored traversal support, but do not duplicate physics or targeting geometry.
    Query->SetCollisionResponseToChannel(ECC_GameTraceChannel1,
        Source.GetCollisionResponseToChannel(ECC_GameTraceChannel1));
    Query->SetGenerateOverlapEvents(false);
    Query->RegisterComponent();
    Query->SetSimulatePhysics(false);
    if (auto* SkeletalQuery = Cast<USkeletalMeshComponent>(Query)) SkeletalQuery->SetAllBodiesSimulatePhysics(false);
    return Query;
}
}

void UOWSVehicleContactComponent::BeginPlay()
{
    Super::BeginPlay();
    if (UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(ECC_GameTraceChannel2) != TEXT("OWSCharacterBody"))
    {
        UE_LOG(LogTemp, Error, TEXT("[OWSContact] Missing OWSCharacterBody collision channel; leaving %s unchanged"), *GetOwner()->GetName());
        SetComponentTickEnabled(false); return;
    }
    TInlineComponentArray<UPrimitiveComponent*> Primitives(GetOwner());
    // Prepare every shape before changing the original physical hulls.
    for (UPrimitiveComponent* Body : Primitives)
    {
        if (!Body || !Body->IsSimulatingPhysics() ||
            Body->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block) continue;
        if (Body->GetCollisionObjectType() == ECC_Pawn)
        {
            UE_LOG(LogTemp, Error, TEXT("[OWSContact] Chassis %s uses the character Pawn object category; leaving vehicle responses unchanged"), *Body->GetPathName());
            for (const FProxy& Existing : Bodies) if (Existing.Query.IsValid()) Existing.Query->DestroyComponent();
            Bodies.Reset(); SetComponentTickEnabled(false); return;
        }
        UPrimitiveComponent* Query = MakeContactQuery(*GetOwner(), *Body);
        if (!Query)
        {
            UE_LOG(LogTemp, Error, TEXT("[OWSContact] Unsupported chassis query shape: %s"), *Body->GetPathName());
            for (const FProxy& Existing : Bodies) if (Existing.Query.IsValid()) Existing.Query->DestroyComponent();
            Bodies.Reset(); SetComponentTickEnabled(false); return;
        }
        FProxy& Entry = Bodies.AddDefaulted_GetRef();
        Entry.Body = Body; Entry.Query = Query;
        Entry.PreviousPawnResponse = Body->GetCollisionResponseToChannel(ECC_Pawn);
        Entry.PreviousLocation = Query->GetComponentLocation();
    }
    for (const FProxy& Entry : Bodies) Entry.Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
}

UPrimitiveComponent* UOWSVehicleContactComponent::GetPrimaryBody() const
{
    for (const FProxy& Entry : Bodies) if (Entry.Body.IsValid()) return Entry.Body.Get();
    return nullptr;
}

void UOWSVehicleContactComponent::TickComponent(float Dt, ELevelTick Type, FActorComponentTickFunction* Tick)
{
    Super::TickComponent(Dt, Type, Tick);
    if (!GetOwner()->HasAuthority() || Dt <= 0.f) return;
    TSet<ACharacter*> Delivered;
    for (FProxy& Entry : Bodies)
    {
        UPrimitiveComponent* Body = Entry.Body.Get();
        UPrimitiveComponent* Query = Entry.Query.Get();
        if (!Body || !Query) continue;
        const FVector Start = Query->GetComponentLocation();
        const FVector Velocity = Body->GetPhysicsLinearVelocityAtPoint(Start);
        TArray<FHitResult> Hits, PreviousHits;
        FComponentQueryParams Params(TEXT("OWSContactChassis"), GetOwner());
        Params.bFindInitialOverlaps = true;
        // Include actual travel since the previous frame as well as the imminent physics step.
        GetWorld()->ComponentSweepMultiByChannel(Hits, Query, Start, Start + Velocity * Dt,
            Query->GetComponentQuat(), ECC_Pawn, Params);
        if (!Start.Equals(Entry.PreviousLocation))
        {
            GetWorld()->ComponentSweepMultiByChannel(PreviousHits, Query, Entry.PreviousLocation, Start,
                Query->GetComponentQuat(), ECC_Pawn, Params);
            Hits.Append(PreviousHits);
        }
        Entry.PreviousLocation = Start;
        for (const FHitResult& Hit : Hits)
        {
            ACharacter* Character = Cast<ACharacter>(Hit.GetActor());
            auto* Contact = Character ? Character->FindComponentByClass<UOWSCharacterContactComponent>() : nullptr;
            if (!Contact || !Contact->IsContactAvailable() || Delivered.Contains(Character) ||
                Character->GetAttachParentActor() == GetOwner()) continue;
            const auto* Movement = Character->GetCharacterMovement();
            if (Movement && Movement->GetMovementBase() && Movement->GetMovementBase()->GetOwner() == GetOwner()) continue;
            // Roof support is not a vehicle strike. Existing traversal and bailout own their states.
            if (-Hit.ImpactNormal.Z > .5f) continue;
            Delivered.Add(Character);
            Contact->ReceiveVehicleContact(*Body, Hit, Dt);
        }
    }
}

void UOWSVehicleContactComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    for (const FProxy& Entry : Bodies)
    {
        if (Entry.Body.IsValid() && Entry.Body->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Ignore)
            Entry.Body->SetCollisionResponseToChannel(ECC_Pawn, Entry.PreviousPawnResponse);
        if (Entry.Query.IsValid()) Entry.Query->DestroyComponent();
    }
    Bodies.Reset(); Super::EndPlay(Reason);
}
