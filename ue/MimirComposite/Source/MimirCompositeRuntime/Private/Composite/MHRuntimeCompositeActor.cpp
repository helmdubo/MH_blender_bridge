#include "Composite/MHRuntimeCompositeActor.h"

#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "Components/ChildActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Math/Transform.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHRuntimeCompositeActor)

DEFINE_LOG_CATEGORY_STATIC(LogMHRuntimeComposite, Log, All);

AMHRuntimeCompositeActor::AMHRuntimeCompositeActor()
{
    CompositeRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MHCompositeRoot"));
    SetRootComponent(CompositeRoot);
    PrimaryActorTick.bCanEverTick = false;
}

const UE::MimirComposite::FMHResolvedCompositePlan* AMHRuntimeCompositeActor::GetResolvedPlan() const
{
    return LastRuntimeError.IsEmpty() && ResolvedPlan.IsValid() && ResolvedPlan->Seed == Seed &&
        ResolvedPlan->Appearance.AppearanceSeed == AppearanceSeed ? ResolvedPlan.Get() : nullptr;
}

bool AMHRuntimeCompositeActor::BuildCandidate(const FMHRuntimeCompositeInput& Input, const int32 InSeed,
    const int32 InAppearanceSeed, UE::MimirComposite::FMHResolvedCompositePlan& OutPlan, FString& OutError) const
{
    using namespace UE::MimirComposite;
    FMHRandomSourceGraph Graph;
    // Admission uses all source options, never the selected subset.
    return MHDecodeRuntimeCompositeGraph(Input.GraphBytes, Graph, OutError) &&
        MHValidateRuntimeCompositeBindings(Graph, Input.Bindings, OutError) &&
        MHResolveCompositePlan(Graph, InSeed, InAppearanceSeed, OutPlan, OutError) &&
        MHValidateResolvedPlacementTransforms(OutPlan, GetActorTransform(), OutError);
}

bool AMHRuntimeCompositeActor::Configure(const FMHRuntimeCompositeInput& Input, const int32 InSeed,
    const int32 InAppearanceSeed, FString& OutError)
{
    OutError.Reset();
    if (bUpdating || IsActorBeingDestroyed())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: runtime placement is already updating or being destroyed");
        return false;
    }
    TGuardValue<bool> Guard(bUpdating, true);
    bBasisRejected = false;
    auto Candidate = MakeShared<UE::MimirComposite::FMHResolvedCompositePlan>();
    if (!BuildCandidate(Input, InSeed, InAppearanceSeed, *Candidate, OutError) || !Materialize(Input, *Candidate, OutError))
    {
        LastRuntimeError = OutError;
        ResolvedSignature.Reset();
        PlacementSignature.Reset();
        return false;
    }
    RuntimeInput = Input;
    Seed = InSeed;
    AppearanceSeed = InAppearanceSeed;
    ResolvedSignature = Candidate->ResolvedSignature;
    PlacementSignature = Candidate->PlacementSignature;
    ResolvedPlan = MoveTemp(Candidate);
    LastRuntimeError.Reset();
    AttachTransformHook();
    return true;
}

bool AMHRuntimeCompositeActor::RebuildRuntime(FString& OutError)
{
    return Configure(RuntimeInput, Seed, AppearanceSeed, OutError);
}

bool AMHRuntimeCompositeActor::Materialize(const FMHRuntimeCompositeInput& Input,
    const UE::MimirComposite::FMHResolvedCompositePlan& Plan, FString& OutError)
{
    using namespace UE::MimirComposite;
    if (!MHValidateResolvedPlacementTransforms(Plan, GetActorTransform(), OutError)) return false;
    struct FMHRuntimeLeafEndpoint { UStaticMesh* Mesh = nullptr; UClass* ActorClass = nullptr; };
    TArray<FMHRuntimeLeafEndpoint> Endpoints;
    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
    {
        const FString Key = (Leaf.Kind == EMHRandomSemanticKind::Mesh ? TEXT("static_mesh:") : TEXT("actor:")) + Leaf.Resource;
        const FMHRuntimeCompositeBinding* Binding = Input.Bindings.FindByPredicate(
            [&Key](const FMHRuntimeCompositeBinding& Entry) { return Entry.ResourceKey == Key; });
        FMHRuntimeLeafEndpoint& Endpoint = Endpoints.AddDefaulted_GetRef();
        if (Binding != nullptr && Leaf.Kind == EMHRandomSemanticKind::Mesh) Endpoint.Mesh = Cast<UStaticMesh>(Binding->Object);
        if (Binding != nullptr && Leaf.Kind == EMHRandomSemanticKind::Actor) Endpoint.ActorClass = Cast<UClass>(Binding->Object);
        if (Endpoint.Mesh == nullptr && Endpoint.ActorClass == nullptr)
        {
            OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: missing runtime endpoint ") + Key + TEXT(" at ") + Leaf.Origin;
            return false;
        }
    }

    // Everything above is read-only. The spawning loop consumes leaves only;
    // it neither walks a source graph nor makes random decisions.
    TArray<TObjectPtr<USceneComponent>> NewComponents;
    const auto DiscardStagedComponents = [&]()
    {
        for (int32 Index = NewComponents.Num() - 1; Index >= 0; --Index)
            if (IsValid(NewComponents[Index]))
            {
                RemoveInstanceComponent(NewComponents[Index]);
                NewComponents[Index]->DestroyComponent();
            }
    };
    const FMatrix Basis = GetActorTransform().ToMatrixWithScale();
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        const FMHRuntimeLeafEndpoint& Endpoint = Endpoints[Index];
        UClass* ComponentClass = Endpoint.Mesh != nullptr ? UStaticMeshComponent::StaticClass() : UChildActorComponent::StaticClass();
        USceneComponent* Component = NewObject<USceneComponent>(this, ComponentClass,
            MakeUniqueObjectName(this, ComponentClass, TEXT("MH_RuntimeLeaf")),
            RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);
        AddInstanceComponent(Component);
        Component->SetupAttachment(CompositeRoot);
        Component->SetAbsolute(true, true, true);
        Component->ComponentTags.Add(FName(*Leaf.Origin));
        Component->SetWorldTransform(FTransform(Leaf.WorldMatrix * Basis));
        if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component))
        {
            MeshComponent->SetStaticMesh(Endpoint.Mesh);
            // S6.3.1: the same appearance Custom Primitive Data as the editor
            // preview, so PIE and packaged tint byte-identically.
            UE::MimirComposite::MHApplyLeafAppearanceCustomData(
                MeshComponent, Leaf, AppearanceCustomDataBaseIndex);
        }
        else if (UChildActorComponent* ActorComponent = Cast<UChildActorComponent>(Component))
        {
            ActorComponent->SetChildActorClass(Endpoint.ActorClass);
        }
        NewComponents.Add(Component);
        Component->RegisterComponent();
        const UChildActorComponent* ChildComponent = Cast<UChildActorComponent>(Component);
        if (!IsValid(Component) || !Component->IsRegistered() ||
            (ChildComponent != nullptr && !IsValid(ChildComponent->GetChildActor())))
        {
            OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: runtime materialization failed at ") + Leaf.Origin;
            DiscardStagedComponents();
            return false;
        }
    }
    ClearMaterializedComponents();
    MaterializedComponents = MoveTemp(NewComponents);
    return true;
}

void AMHRuntimeCompositeActor::ClearMaterializedComponents()
{
    for (int32 Index = MaterializedComponents.Num() - 1; Index >= 0; --Index)
    {
        if (IsValid(MaterializedComponents[Index]))
        {
            RemoveInstanceComponent(MaterializedComponents[Index]);
            MaterializedComponents[Index]->DestroyComponent();
        }
    }
    MaterializedComponents.Reset();
}

void AMHRuntimeCompositeActor::AttachTransformHook()
{
    if (CompositeRoot != nullptr && !IsTemplate())
    {
        CompositeRoot->TransformUpdated.RemoveAll(this);
        CompositeRoot->TransformUpdated.AddUObject(this, &AMHRuntimeCompositeActor::UpdatePlacementBasis);
    }
}

void AMHRuntimeCompositeActor::UpdatePlacementBasis(USceneComponent*, EUpdateTransformFlags, ETeleportType)
{
    if (bUpdating || !ResolvedPlan.IsValid() || IsActorBeingDestroyed()) return;
    if (!LastRuntimeError.IsEmpty() && !bBasisRejected) return;
    TGuardValue<bool> Guard(bUpdating, true);
    FString Error;
    if (!UE::MimirComposite::MHValidateResolvedPlacementTransforms(*ResolvedPlan, GetActorTransform(), Error))
    {
        LastRuntimeError = MoveTemp(Error);
        ResolvedSignature.Reset();
        PlacementSignature.Reset();
        bBasisRejected = true;
        return;
    }
    LastRuntimeError.Reset();
    ResolvedSignature = ResolvedPlan->ResolvedSignature;
    PlacementSignature = ResolvedPlan->PlacementSignature;
    bBasisRejected = false;
    const FMatrix Basis = GetActorTransform().ToMatrixWithScale();
    for (int32 Index = 0; Index < MaterializedComponents.Num(); ++Index)
    {
        if (IsValid(MaterializedComponents[Index]))
            MaterializedComponents[Index]->SetWorldTransform(FTransform(ResolvedPlan->Leaves[Index].WorldMatrix * Basis),
                false, nullptr, ETeleportType::TeleportPhysics);
    }
}

void AMHRuntimeCompositeActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    AttachTransformHook();
    if (!RuntimeInput.GraphBytes.IsEmpty() && !ResolvedPlan.IsValid())
    {
        FString Error;
        RebuildRuntime(Error);
    }
}

void AMHRuntimeCompositeActor::PostLoad()
{
    Super::PostLoad();
    ResolvedPlan.Reset();
    LastRuntimeError.Reset();
    ResolvedSignature.Reset();
    PlacementSignature.Reset();
}

void AMHRuntimeCompositeActor::BeginPlay()
{
    FString Error;
    if (!RebuildRuntime(Error)) UE_LOG(LogMHRuntimeComposite, Error, TEXT("%s: %s"), *GetPathName(), *Error);
    Super::BeginPlay();
}

void AMHRuntimeCompositeActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (CompositeRoot != nullptr) CompositeRoot->TransformUpdated.RemoveAll(this);
    ClearMaterializedComponents();
    Super::EndPlay(EndPlayReason);
}

void AMHRuntimeCompositeActor::Destroyed()
{
    if (CompositeRoot != nullptr) CompositeRoot->TransformUpdated.RemoveAll(this);
    ClearMaterializedComponents();
    Super::Destroyed();
}
