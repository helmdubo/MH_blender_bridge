#include "Composite/MHCompositePlacementCompiler.h"

#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Components/BoxComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite
{
namespace
{
constexpr EObjectFlags PlanViewFlags = RF_Transactional | RF_Transient | RF_DuplicateTransient | RF_TextExportTransient;

/** Instrumentation counter behind MHGetPlacementPreviousComponentProbes. */
uint64 GMHPlacementPreviousComponentProbes = 0;

FMatrix PlanViewTrsMatrix(const FMHRandomTrs& Trs)
{
    return FTransform(FQuat(Trs.RotationQuat), FVector(Trs.TranslationCm), FVector(Trs.Scale)).ToMatrixWithScale();
}

void PlanViewSetWorld(USceneComponent& Component, const FMatrix& Matrix)
{
    const FTransform Transform(Matrix);
    if (!Component.GetComponentTransform().Equals(Transform, 0.0))
        Component.SetWorldTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
}

/**
 * Canonical creation order: NewObject, AddInstanceComponent, SetupAttachment,
 * SetAbsolute, endpoint configuration, RegisterComponent. Nothing that decides
 * what the component renders may happen after registration.
 */
USceneComponent* PlanViewNew(AActor& Target, UClass* Class, const FString& Label,
    const FName Key, FMHCompositePlacementCompileResult& Result, USceneComponent* Parent = nullptr,
    const TFunction<void(USceneComponent&)>& Configure = TFunction<void(USceneComponent&)>())
{
    USceneComponent* Component = NewObject<USceneComponent>(&Target, Class,
        MakeUniqueObjectName(&Target, Class, FName(*Label)), PlanViewFlags);
    Target.AddInstanceComponent(Component);
    Component->ComponentTags.Add(Key);
    Component->SetupAttachment(Parent != nullptr ? Parent : Target.GetRootComponent());
    // Componentwise FTransform multiplication can even lose representable
    // scale-axis permutations. Apply the admitted full world matrix instead.
    Component->SetAbsolute(Parent == nullptr, Parent == nullptr, Parent == nullptr);
    if (Configure) Configure(*Component);
    {
        FMHPlacementStageScope Stage(EMHPlacementStage::RegisterComponents);
        Component->RegisterComponent();
    }
    Result.Components.Add(Component);
    return Component;
}

/** One tag/class row of the previous component view. */
struct FPlanViewPreviousKey
{
    FName Tag;
    const UClass* Class = nullptr;

    bool operator==(const FPlanViewPreviousKey& Other) const
    {
        return Tag == Other.Tag && Class == Other.Class;
    }
};

uint32 GetTypeHash(const FPlanViewPreviousKey& Key)
{
    return HashCombine(GetTypeHash(Key.Tag), GetTypeHash(Key.Class));
}

using FPlanViewPreviousIndex = TMap<FPlanViewPreviousKey, USceneComponent*>;

/**
 * Build the reuse index once instead of rescanning the previous view per leaf.
 * The former linear scan returned the first previous component matching both
 * tag and class, so insertion keeps the first occurrence and never overwrites.
 */
FPlanViewPreviousIndex PlanViewIndexPrevious(TConstArrayView<TObjectPtr<UActorComponent>> Previous)
{
    FPlanViewPreviousIndex Index;
    Index.Reserve(Previous.Num());
    for (UActorComponent* Component : Previous)
    {
        if (!IsValid(Component)) continue;
        USceneComponent* Scene = Cast<USceneComponent>(Component);
        for (const FName& Tag : Component->ComponentTags)
        {
            ++GMHPlacementPreviousComponentProbes;
            const FPlanViewPreviousKey Key{Tag, Component->GetClass()};
            if (!Index.Contains(Key)) Index.Add(Key, Scene);
        }
    }
    return Index;
}

USceneComponent* PlanViewFind(const FPlanViewPreviousIndex& Previous, const FName Key, UClass* Class)
{
    ++GMHPlacementPreviousComponentProbes;
    USceneComponent* const* Found = Previous.Find(FPlanViewPreviousKey{Key, Class});
    return Found != nullptr ? *Found : nullptr;
}

USceneComponent* PlanViewPlaceholder(AActor& Target, const FString& Label, const FName Key,
    FMHCompositePlacementCompileResult& Result)
{
    USceneComponent* Root = PlanViewNew(Target, USceneComponent::StaticClass(), TEXT("MH_Unresolved"), Key, Result);
    UBoxComponent* Box = CastChecked<UBoxComponent>(PlanViewNew(Target, UBoxComponent::StaticClass(), TEXT("MH_UnresolvedBox"), Key, Result, Root));
    Box->SetBoxExtent(FVector(50.0));
    Box->ShapeColor = FColor::Red;
    Box->bDrawOnlyIfSelected = false;
    Box->SetLineThickness(3.0f);
    Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Box->SetHiddenInGame(false);
    UTextRenderComponent* Text = CastChecked<UTextRenderComponent>(PlanViewNew(Target, UTextRenderComponent::StaticClass(), TEXT("MH_UnresolvedLabel"), Key, Result, Root));
    Text->SetText(FText::FromString(Label));
    Text->SetTextRenderColor(FColor::Red);
    Text->SetHorizontalAlignment(EHTA_Center);
    Text->SetWorldSize(24.0f);
    Text->SetRelativeLocation(FVector(0, 0, 60));
    Text->SetHiddenInGame(false);
    return Root;
}

bool PlanViewPreflight(const FMHResolvedCompositePlan& Plan, const FMHRandomComposite& Root,
    const FTransform& Placement, FString& Error)
{
    if (!MHValidateResolvedPlacementTransforms(Plan, Placement, Error)) return false;
    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
    {
        if (!Root.Nodes.IsValidIndex(Leaf.RootNodeIndex))
        {
            Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: resolved leaf has no authored root handle: ") + Leaf.Origin;
            return false;
        }
    }
    for (int32 Index = 0; Index < Root.Nodes.Num(); ++Index)
    {
        if (!MHIsRepresentableTransformMatrix(PlanViewTrsMatrix(Root.Nodes[Index].Transform) * Placement.ToMatrixWithScale()))
        {
            Error = FString::Printf(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: authored handle %s:nodes[%d]"), *Root.Name, Index);
            return false;
        }
    }
    return true;
}
} // namespace

uint64 MHGetPlacementPreviousComponentProbes()
{
    return GMHPlacementPreviousComponentProbes;
}

void MHResetPlacementPreviousComponentProbes()
{
    GMHPlacementPreviousComponentProbes = 0;
}

bool MHUpdateCompositePlacementBasis(AActor& Target, const FMHResolvedCompositePlan& Plan,
    const FMHRandomComposite& RootDefinition, TConstArrayView<TObjectPtr<USceneComponent>> Handles,
    TConstArrayView<TObjectPtr<USceneComponent>> Leaves, FString& OutError)
{
    // Fail-closed before any mutation: an intersection-length walk leaves the
    // surplus silently stale and still reports success. Require exact agreement
    // and let the caller rebuild the whole placement instead.
    if (Handles.Num() != RootDefinition.Nodes.Num() || Leaves.Num() != Plan.Leaves.Num())
    {
        OutError = FString::Printf(
            TEXT("MH_E_PLACEMENT_STATE_DESYNC: %s handles %d of %d, leaves %d of %d"),
            *RootDefinition.Name, Handles.Num(), RootDefinition.Nodes.Num(), Leaves.Num(), Plan.Leaves.Num());
        return false;
    }
    if (!PlanViewPreflight(Plan, RootDefinition, Target.GetActorTransform(), OutError)) return false;
    const FMatrix Basis = Target.GetActorTransform().ToMatrixWithScale();
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        if (IsValid(Handles[Index])) PlanViewSetWorld(*Handles[Index], PlanViewTrsMatrix(RootDefinition.Nodes[Index].Transform) * Basis);
    }
    for (int32 Index = 0; Index < Leaves.Num(); ++Index)
    {
        if (IsValid(Leaves[Index])) PlanViewSetWorld(*Leaves[Index], Plan.Leaves[Index].WorldMatrix * Basis);
    }
    return true;
}

FMHCompositePlacementCompileResult MHCompileCompositePlacementV5(AActor& Target,
    const FMHResolvedCompositePlan& Plan, const FMHRandomComposite& RootDefinition,
    const UMHCompositeSettings& Settings, TConstArrayView<TObjectPtr<UActorComponent>> PreviousComponents)
{
    FMHPlacementStageScope CompileStage(EMHPlacementStage::CompilePlacement);
    FMHCompositePlacementCompileResult Result;
    if (!PlanViewPreflight(Plan, RootDefinition, Target.GetActorTransform(), Result.Error)) return Result;
    struct FEndpoint { UStaticMesh* Mesh = nullptr; UClass* ActorClass = nullptr; };
    TArray<FEndpoint> Endpoints;
    {
        FMHPlacementStageScope LoadStage(EMHPlacementStage::LoadEndpoints);
        // All endpoint lookup and matrix checks precede component mutations.
        for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
        {
            FEndpoint& Endpoint = Endpoints.AddDefaulted_GetRef();
            if (Leaf.Kind == EMHRandomSemanticKind::Mesh)
            {
                FMHResourceKey Key;
                Key.Kind = EMHResourceKind::StaticMesh;
                Key.LogicalName = Leaf.Resource;
                MHRecordDefinitionEndpointResolve();
                Endpoint.Mesh = Cast<UStaticMesh>(MHLoadAppliedResource(Key, Result.Error));
                if (!Result.Error.IsEmpty()) return Result;
            }
            else if (Leaf.Kind == EMHRandomSemanticKind::Actor)
            {
                const FSoftClassPath* Path = Settings.ActorClassRegistry.Find(Leaf.Resource);
                Endpoint.ActorClass = Path != nullptr ? Path->TryLoadClass<AActor>() : nullptr;
                if (!MHIsSpawnableCompositeActorClass(Endpoint.ActorClass)) Endpoint.ActorClass = nullptr;
            }
            else
            {
                Result.Error = TEXT("MH_E_COMPOSITE_GRAMMAR: resolved leaf is neither mesh nor actor");
                return Result;
            }
        }
    }
    const FMatrix Basis = Target.GetActorTransform().ToMatrixWithScale();
    // Handles and leaves share one index; their tag prefixes already separate
    // MH.Handle: from MH.Leaf:, so no second pass over the previous view.
    const FPlanViewPreviousIndex PreviousIndex = PlanViewIndexPrevious(PreviousComponents);
    for (int32 Index = 0; Index < RootDefinition.Nodes.Num(); ++Index)
    {
        const FName Key(*FString::Printf(TEXT("MH.Handle:%d"), Index));
        USceneComponent* Handle = PlanViewFind(PreviousIndex, Key, USceneComponent::StaticClass());
        if (Handle == nullptr) Handle = PlanViewNew(Target, USceneComponent::StaticClass(), TEXT("MH_Node"), Key, Result);
        else Result.Components.Add(Handle);
        PlanViewSetWorld(*Handle, PlanViewTrsMatrix(RootDefinition.Nodes[Index].Transform) * Basis);
        Result.TopLevelComponents.Add(Handle);
    }
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        const FEndpoint& Endpoint = Endpoints[Index];
        const FString Label = Leaf.DisplayName.IsEmpty() ? Leaf.Resource : Leaf.DisplayName;
        const FName Key(*FString::Printf(TEXT("MH.Leaf:%s:%d:%s"), *Leaf.Origin, static_cast<int32>(Leaf.Kind), *Leaf.Resource));
        UClass* Class = Endpoint.Mesh != nullptr ? UStaticMeshComponent::StaticClass() :
            (Endpoint.ActorClass != nullptr ? UChildActorComponent::StaticClass() : nullptr);
        USceneComponent* Component = Class != nullptr ? PlanViewFind(PreviousIndex, Key, Class) : nullptr;
        if (Class == nullptr)
        {
            Component = PlanViewPlaceholder(Target, Label, Key, Result);
            Result.Warnings.Add(TEXT("MH_W_UNRESOLVED_PLACEMENT: ") + Leaf.Origin + TEXT(" -> ") + Leaf.Resource);
        }
        else if (Component == nullptr)
        {
            // A newly created leaf knows its endpoint before it registers; a
            // reused one is configured below, where it is already registered.
            Component = PlanViewNew(Target, Class, TEXT("MH_Leaf_") + Leaf.Resource, Key, Result, nullptr,
                [&Endpoint](USceneComponent& New)
                {
                    if (UStaticMeshComponent* NewMesh = Cast<UStaticMeshComponent>(&New)) NewMesh->SetStaticMesh(Endpoint.Mesh);
                    else if (UChildActorComponent* NewChild = Cast<UChildActorComponent>(&New))
                    {
                        NewChild->SetEditorTreeViewVisualizationMode(EChildActorComponentTreeViewVisualizationMode::Hidden);
                        NewChild->SetChildActorClass(Endpoint.ActorClass);
                    }
                });
        }
        else Result.Components.Add(Component);
        // Organizational ancestry only: keep the admitted full world matrix,
        // never recompose it through the author's handle using FTransform.
        // Apply this to reused leaves and placeholder roots as well.
        Component->SetAbsolute(true, true, true);
        Component->AttachToComponent(Result.TopLevelComponents[Leaf.RootNodeIndex], FAttachmentTransformRules::KeepWorldTransform);
        PlanViewSetWorld(*Component, Leaf.WorldMatrix * Basis);
        if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component)) Mesh->SetStaticMesh(Endpoint.Mesh);
        // S6.3.1: the resolved appearance channels reach the material as Custom
        // Primitive Data. Materialization side only - never part of a preimage.
        MHApplyLeafAppearanceCustomData(Component, Leaf, Settings.AppearanceCustomDataBaseIndex);
        if (UChildActorComponent* Child = Cast<UChildActorComponent>(Component))
        {
            Child->SetEditorTreeViewVisualizationMode(EChildActorComponentTreeViewVisualizationMode::Hidden);
            if (Child->GetChildActorClass() != Endpoint.ActorClass) Child->SetChildActorClass(Endpoint.ActorClass);
            if (AActor* Actor = Child->GetChildActor()) Actor->SetActorLabel(Label, false);
        }
        Result.LeafComponents.Add(Component);
    }
    return Result;
}

FMHCompositePlacementCompileResult MHBuildCompositeDiagnosticView(AActor& Target, const FString& Label, const FString& Diagnostic)
{
    FMHCompositePlacementCompileResult Result;
    USceneComponent* Placeholder = PlanViewPlaceholder(Target, Label, FName(TEXT("MH.Diagnostic")), Result);
    Placeholder->SetWorldTransform(Target.GetActorTransform());
    Result.Warnings.Add(TEXT("MH_W_UNRESOLVED_PLACEMENT: ") + Diagnostic);
    return Result;
}
} // namespace UE::MimirComposite
