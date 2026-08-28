#include "Composite/MHCompositePlacementCompiler.h"

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

USceneComponent* PlanViewNew(AActor& Target, UClass* Class, const FString& Label,
    const FName Key, FMHCompositePlacementCompileResult& Result, USceneComponent* Parent = nullptr,
    const bool bAbsolute = true)
{
    USceneComponent* Component = NewObject<USceneComponent>(&Target, Class,
        MakeUniqueObjectName(&Target, Class, FName(*Label)), PlanViewFlags);
    Target.AddInstanceComponent(Component);
    Component->ComponentTags.Add(Key);
    Component->SetupAttachment(Parent != nullptr ? Parent : Target.GetRootComponent());
    // Componentwise FTransform multiplication can even lose representable
    // scale-axis permutations. Apply the admitted full world matrix instead.
    Component->SetAbsolute(bAbsolute, bAbsolute, bAbsolute);
    // Assign mesh, transform and derived display state before the first
    // registration, avoiding a second render/physics-state construction.
    Result.Components.Add(Component);
    return Component;
}

using FPlanViewComponentMap = TMap<FName, USceneComponent*>;

USceneComponent* PlanViewFind(const FPlanViewComponentMap& Previous, const FName Key, UClass* Class)
{
    USceneComponent* Component = Previous.FindRef(Key);
    return IsValid(Component) && Component->GetClass() == Class ? Component : nullptr;
}

void PlanViewAttach(USceneComponent& Component, USceneComponent* Parent)
{
    if (Component.GetAttachParent() != Parent)
        Component.AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform);
    Component.SetAbsolute(true, true, true);
}

void PlanViewRegister(AActor& Target, FMHCompositePlacementCompileResult& Result)
{
    for (UActorComponent* Component : Result.Components)
    {
        if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
        {
            // Drag previews are intentionally unpickable; a real placement
            // uses the engine's ordinary HActor proxy, never a custom raycast.
            const bool bSelectable = !Target.bIsEditorPreviewActor;
            if (Primitive->bSelectable != bSelectable)
            {
                Primitive->bSelectable = bSelectable;
                Primitive->MarkRenderStateDirty();
            }
            Primitive->SetVisibility(true);
            Primitive->SetHiddenInGame(false);
        }
        if (!Component->IsRegistered()) Component->RegisterComponent();
    }
}

USceneComponent* PlanViewPlaceholder(AActor& Target, const FString& Label, const FName Key,
    FMHCompositePlacementCompileResult& Result, USceneComponent* Parent = nullptr)
{
    USceneComponent* Root = PlanViewNew(Target, USceneComponent::StaticClass(), TEXT("MH_Unresolved"), Key, Result, Parent);
    UBoxComponent* Box = CastChecked<UBoxComponent>(PlanViewNew(Target, UBoxComponent::StaticClass(), TEXT("MH_UnresolvedBox"), Key, Result, Root, false));
    Box->SetBoxExtent(FVector(50.0));
    Box->ShapeColor = FColor::Red;
    Box->bDrawOnlyIfSelected = false;
    Box->SetLineThickness(3.0f);
    Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Box->SetHiddenInGame(false);
    UTextRenderComponent* Text = CastChecked<UTextRenderComponent>(PlanViewNew(Target, UTextRenderComponent::StaticClass(), TEXT("MH_UnresolvedLabel"), Key, Result, Root, false));
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
    for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
    {
        const FMHResolvedCompositeNode& Node = Plan.Nodes[Index];
        if (!Root.Nodes.IsValidIndex(Node.RootNodeIndex) ||
            (Node.ParentNodeIndex != INDEX_NONE && (Node.ParentNodeIndex < 0 || Node.ParentNodeIndex >= Index)))
        {
            Error = TEXT("MH_E_COMPOSITE_GRAMMAR: invalid resolved node hierarchy");
            return false;
        }
    }
    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
        if (!Plan.Nodes.IsValidIndex(Leaf.NodeIndex))
        {
            Error = TEXT("MH_E_COMPOSITE_GRAMMAR: resolved leaf has no owning node");
            return false;
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

bool MHUpdateCompositePlacementBasis(AActor& Target, const FMHResolvedCompositePlan& Plan,
    const FMHRandomComposite& RootDefinition, TConstArrayView<TObjectPtr<USceneComponent>> Handles,
    TConstArrayView<TObjectPtr<USceneComponent>> Nodes,
    TConstArrayView<TObjectPtr<USceneComponent>> Leaves, FString& OutError)
{
    if (!PlanViewPreflight(Plan, RootDefinition, Target.GetActorTransform(), OutError)) return false;
    const FMatrix Basis = Target.GetActorTransform().ToMatrixWithScale();
    for (int32 Index = 0; Index < Handles.Num() && Index < RootDefinition.Nodes.Num(); ++Index)
    {
        if (IsValid(Handles[Index])) PlanViewSetWorld(*Handles[Index], PlanViewTrsMatrix(RootDefinition.Nodes[Index].Transform) * Basis);
    }
    for (int32 Index = 0; Index < Leaves.Num() && Index < Plan.Leaves.Num(); ++Index)
    {
        if (IsValid(Leaves[Index])) PlanViewSetWorld(*Leaves[Index], Plan.Leaves[Index].WorldMatrix * Basis);
    }
    for (int32 Index = 0; Index < Nodes.Num() && Index < Plan.Nodes.Num(); ++Index)
    {
        if (IsValid(Nodes[Index])) PlanViewSetWorld(*Nodes[Index], Plan.Nodes[Index].WorldMatrix * Basis);
    }
    return true;
}

FMHCompositePlacementCompileResult MHCompileCompositePlacementV5(AActor& Target,
    const FMHResolvedCompositePlan& Plan, const FMHRandomComposite& RootDefinition,
    const UMHCompositeSettings& Settings, TConstArrayView<TObjectPtr<UActorComponent>> PreviousComponents)
{
    FMHCompositePlacementCompileResult Result;
    if (!PlanViewPreflight(Plan, RootDefinition, Target.GetActorTransform(), Result.Error)) return Result;
    struct FEndpoint { UStaticMesh* Mesh = nullptr; UClass* ActorClass = nullptr; };
    TArray<FEndpoint> Endpoints;
    FMHAppliedResourceLookup ResourceLookup;
    // All endpoint lookup and matrix checks precede component mutations.
    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
    {
        FEndpoint& Endpoint = Endpoints.AddDefaulted_GetRef();
        if (Leaf.Kind == EMHRandomSemanticKind::Mesh)
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::StaticMesh;
            Key.LogicalName = Leaf.Resource;
            Endpoint.Mesh = Cast<UStaticMesh>(ResourceLookup.Load(Key, Result.Error));
            if (!Result.Error.IsEmpty()) return Result;
            // A cache hit can cold-load a mesh again after GC, starting a new
            // async build. This second boundary must also remain nonblocking.
            if (Endpoint.Mesh != nullptr && Endpoint.Mesh->IsCompiling())
            {
                Result.PendingMesh = Endpoint.Mesh;
                return Result;
            }
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
    FPlanViewComponentMap PreviousByKey;
    for (UActorComponent* Component : PreviousComponents)
        if (USceneComponent* Scene = Cast<USceneComponent>(Component); IsValid(Scene))
            for (const FName Tag : Scene->ComponentTags)
                if (!PreviousByKey.Contains(Tag)) PreviousByKey.Add(Tag, Scene);
    const FMatrix Basis = Target.GetActorTransform().ToMatrixWithScale();
    for (int32 Index = 0; Index < RootDefinition.Nodes.Num(); ++Index)
    {
        const FName Key(*FString::Printf(TEXT("MH.Handle:%d"), Index));
        USceneComponent* Handle = PlanViewFind(PreviousByKey, Key, USceneComponent::StaticClass());
        if (Handle == nullptr) Handle = PlanViewNew(Target, USceneComponent::StaticClass(), TEXT("MH_Node"), Key, Result);
        else Result.Components.Add(Handle);
        PlanViewSetWorld(*Handle, PlanViewTrsMatrix(RootDefinition.Nodes[Index].Transform) * Basis);
        Result.TopLevelComponents.Add(Handle);
    }
    for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
    {
        const FMHResolvedCompositeNode& Node = Plan.Nodes[Index];
        USceneComponent* Parent = Node.ParentNodeIndex == INDEX_NONE
            ? Result.TopLevelComponents[Node.RootNodeIndex].Get() : Result.NodeComponents[Node.ParentNodeIndex].Get();
        if (Node.ParentNodeIndex == INDEX_NONE && !Node.bHasProfile)
        {
            // A top-level unsampled node is already its own authoring handle.
            // Profile nodes need a separate sampled child beneath that handle.
            Result.NodeComponents.Add(Parent);
            continue;
        }
        const FName Key(*(TEXT("MH.Node:") + Node.NodePath));
        USceneComponent* Component = PlanViewFind(PreviousByKey, Key, USceneComponent::StaticClass());
        if (Component == nullptr)
            Component = PlanViewNew(Target, USceneComponent::StaticClass(), TEXT("MH_Node"), Key, Result, Parent);
        else
        {
            Result.Components.Add(Component);
            PlanViewAttach(*Component, Parent);
        }
        PlanViewSetWorld(*Component, Node.WorldMatrix * Basis);
        Result.NodeComponents.Add(Component);
    }
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        const FEndpoint& Endpoint = Endpoints[Index];
        const FString Label = Leaf.DisplayName.IsEmpty() ? Leaf.Resource : Leaf.DisplayName;
        const FName Key(*FString::Printf(TEXT("MH.Leaf:%s:%d:%s"), *Leaf.Origin, static_cast<int32>(Leaf.Kind), *Leaf.Resource));
        UClass* Class = Endpoint.Mesh != nullptr ? UStaticMeshComponent::StaticClass() :
            (Endpoint.ActorClass != nullptr ? UChildActorComponent::StaticClass() : nullptr);
        USceneComponent* Parent = Result.NodeComponents[Leaf.NodeIndex];
        USceneComponent* Component = Class != nullptr ? PlanViewFind(PreviousByKey, Key, Class) : nullptr;
        if (Class == nullptr)
        {
            Component = PlanViewPlaceholder(Target, Label, Key, Result, Parent);
            Result.Warnings.Add(TEXT("MH_W_UNRESOLVED_PLACEMENT: ") + Leaf.Origin + TEXT(" -> ") + Leaf.Resource);
        }
        else if (Component == nullptr) Component = PlanViewNew(Target, Class, TEXT("MH_Leaf_") + Leaf.Resource, Key, Result, Parent);
        else
        {
            Result.Components.Add(Component);
            PlanViewAttach(*Component, Parent);
        }
        PlanViewSetWorld(*Component, Leaf.WorldMatrix * Basis);
        if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component)) Mesh->SetStaticMesh(Endpoint.Mesh);
        if (UChildActorComponent* Child = Cast<UChildActorComponent>(Component))
        {
            Child->SetEditorTreeViewVisualizationMode(EChildActorComponentTreeViewVisualizationMode::Hidden);
            if (Child->GetChildActorClass() != Endpoint.ActorClass) Child->SetChildActorClass(Endpoint.ActorClass);
            if (AActor* Actor = Child->GetChildActor()) Actor->SetActorLabel(Label, false);
        }
        Result.LeafComponents.Add(Component);
    }
    PlanViewRegister(Target, Result);
    // ChildActorComponent creates its actor during registration. Label it only
    // after that boundary (already-existing children follow the same path).
    for (int32 Index = 0; Index < Result.LeafComponents.Num(); ++Index)
        if (UChildActorComponent* Child = Cast<UChildActorComponent>(Result.LeafComponents[Index]))
            if (AActor* Actor = Child->GetChildActor())
            {
                const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
                Actor->SetActorLabel(Leaf.DisplayName.IsEmpty() ? Leaf.Resource : Leaf.DisplayName, false);
            }
    return Result;
}

FMHCompositePlacementCompileResult MHBuildCompositeDiagnosticView(AActor& Target, const FString& Label, const FString& Diagnostic)
{
    FMHCompositePlacementCompileResult Result;
    USceneComponent* Placeholder = PlanViewPlaceholder(Target, Label, FName(TEXT("MH.Diagnostic")), Result);
    Placeholder->SetWorldTransform(Target.GetActorTransform());
    PlanViewRegister(Target, Result);
    Result.Warnings.Add(TEXT("MH_W_UNRESOLVED_PLACEMENT: ") + Diagnostic);
    return Result;
}
} // namespace UE::MimirComposite
