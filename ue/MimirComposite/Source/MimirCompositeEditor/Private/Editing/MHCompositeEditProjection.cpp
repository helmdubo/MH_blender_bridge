#include "Editing/MHCompositeEditProjection.h"

#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Selection.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditProjection)

using namespace UE::MimirComposite;

namespace
{

void RetireProjectionComponent(USceneComponent* Component)
{
    if (!IsValid(Component)) return;
    if (GEditor != nullptr && GEditor->GetSelectedComponents()->IsSelected(Component))
    {
        GEditor->SelectComponent(Component, false, false, true);
    }
    Component->DestroyComponent();
}

/** Maps a resolved visual path to the authoring path in the current definition. */
FString AuthoringPathForVisual(const FString& Origin, const FString& DefinitionPrefix)
{
    if (!Origin.StartsWith(DefinitionPrefix, ESearchCase::CaseSensitive)) return FString();
    FString Path = Origin;
    const int32 DefinitionBoundary = Path.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, DefinitionPrefix.Len());
    if (DefinitionBoundary != INDEX_NONE) Path = Path.Left(DefinitionBoundary);
    const int32 OptionBoundary = Path.Find(TEXT("/options["), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (OptionBoundary != INDEX_NONE) Path = Path.Left(OptionBoundary);
    return Path;
}

} // namespace

UMHCompositeEditMeshComponent::UMHCompositeEditMeshComponent()
{
#if WITH_EDITOR
    SelectionOverrideDelegate.BindUObject(this, &UMHCompositeEditMeshComponent::IsEditIndividuallySelected);
#endif
}

void UMHCompositeEditMeshComponent::SetEditSelected(const bool bSelected)
{
    if (bEditSelected == bSelected) return;
    bEditSelected = bSelected;
#if WITH_EDITOR
    PushSelectionToProxy();
#endif
}

bool UMHCompositeEditMeshComponent::ShouldRenderSelected() const
{
    // UPrimitiveComponent normally inherits its owner's selection.  All edit
    // visuals share one selected projection actor, so that would outline every
    // sibling.  Logical node selection is the sole rendering authority here.
    return bEditSelected;
}

bool UMHCompositeEditMeshComponent::IsEditIndividuallySelected(const UPrimitiveComponent* Component) const
{
    return Component == this && bEditSelected;
}

AMHCompositeEditProjectionActor::AMHCompositeEditProjectionActor()
{
    // The actor must remain renderable in the editor's Game View. Lifetime is
    // kept editor-local by the transient/duplicate-transient spawn flags,
    // rather than render visibility flags.
    bListedInSceneOutliner = false;
    PrimaryActorTick.bCanEverTick = false;
    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("MH_EditProjectionRoot"));
    Root->SetMobility(EComponentMobility::Movable);
    SetRootComponent(Root);
}

bool UMHCompositeEditProjection::Open(UMHCompositeEditSession& InSession, FString& OutError)
{
    Close();
    Session = &InSession;
    AMHCompositeActor* Root = InSession.GetRootPlacement();
    const UMHCompositeAsset* Edited = InSession.GetEditedAsset();
    UWorld* World = Root != nullptr ? Root->GetWorld() : nullptr;
    if (Root == nullptr || Edited == nullptr || World == nullptr || Root->GetLevel() == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: an edit projection needs an open session on a placed root");
        return false;
    }
    // CE-3c: a root session's occurrence is the whole placement.
    OccurrencePrefix = InSession.IsNested() ? InSession.GetInvocationPath() + TEXT(">") : FString();
    DefinitionPrefix = OccurrencePrefix + Edited->LogicalName + TEXT(":");
    // The same spawn shape as the engine's Level Instance editing actor:
    // transient, no actor package, not in the outliner, in the root's level.
    FActorSpawnParameters Params;
    Params.OverrideLevel = Root->GetLevel();
    Params.bHideFromSceneOutliner = true;
    Params.bCreateActorPackage = false;
    Params.ObjectFlags = RF_Transient | RF_DuplicateTransient;
    Params.bNoFail = true;
    // CE-3d: the actor's pivot is the occurrence's transform (the gizmo of
    // the framed occurrence sits there, not at the placement's origin);
    // components are absolute, so the pivot carries no geometry.
    FTransform Pivot = Root->GetActorTransform();
    if (InSession.IsNested())
    {
        if (const UE::MimirComposite::FMHResolvedCompositePlan* RootPlan = Root->GetResolvedPlan())
        {
            FString PivotNodePath = InSession.GetInvocationPath();
            // A selected Composite option is an invocation occurrence but not
            // a separate resolved node. Its Random owner supplies the world
            // frame; the option path remains the occurrence prefix below.
            const int32 OptionBoundary = PivotNodePath.Find(
                TEXT("/options["), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            const FString OptionSelector = OptionBoundary != INDEX_NONE
                ? PivotNodePath.Mid(OptionBoundary)
                : FString();
            const FString OptionIndexText = OptionSelector.Len() >= 11 && OptionSelector.EndsWith(TEXT("]"))
                ? OptionSelector.Mid(9, OptionSelector.Len() - 10)
                : FString();
            const int32 OptionIndex = OptionIndexText.IsNumeric() ? FCString::Atoi(*OptionIndexText) : INDEX_NONE;
            if (OptionIndex >= 0 && FString::FromInt(OptionIndex) == OptionIndexText)
            {
                PivotNodePath.LeftInline(OptionBoundary, EAllowShrinking::No);
            }
            for (const UE::MimirComposite::FMHResolvedCompositeNode& Node : RootPlan->Nodes)
            {
                if (Node.NodePath != PivotNodePath) continue;
                Pivot = FTransform(Node.WorldMatrix * Root->GetActorTransform().ToMatrixWithScale());
                break;
            }
        }
    }
    AMHCompositeEditProjectionActor* Actor = World->SpawnActor<AMHCompositeEditProjectionActor>(Pivot.GetLocation(), Pivot.Rotator(), Params);
    if (Actor == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the edit projection actor could not be spawned");
        return false;
    }
    // SpawnActor(Location, Rotation) does not carry occurrence scale.  Keep the
    // projection pivot on the complete occurrence frame used by its nodes.
    Actor->SetActorTransform(Pivot, false, nullptr, ETeleportType::TeleportPhysics);
    Actor->SetActorLabel(TEXT("MH Edit: ") + Edited->LogicalName);
    ProjectionActor = Actor;
    if (!Refresh(OutError))
    {
        Close();
        return false;
    }
    // The projection is in place: only now does the occurrence's own rendering go away.
    AcquireLease();
    return true;
}

void UMHCompositeEditProjection::AcquireLease()
{
    const UMHCompositeEditSession* Owner = Session.Get();
    const AMHCompositeActor* Root = Owner != nullptr ? Owner->GetRootPlacement() : nullptr;
    UMHInstancePoolSubsystem* Pool = Root != nullptr ? UMHInstancePoolSubsystem::Get(Root->GetWorld()) : nullptr;
    if (Pool == nullptr) return;
    TArray<FMHInstanceHandle> Handles;
    for (const FMHCompositeLeafMaterialization& Row : Root->GetLeafMaterializations())
    {
        if (Row.Handle.IsSet() && UnderOccurrence(Row.NodePath)) Handles.Add(Row.Handle);
    }
    Lease = Pool->AcquireSuppression(Handles);
}

bool UMHCompositeEditProjection::UnderOccurrence(const FString& Path) const
{
    // A root session's occurrence is the whole placement (FString::StartsWith
    // of an empty prefix is false, so the empty prefix is explicit).
    return OccurrencePrefix.IsEmpty() || Path.StartsWith(OccurrencePrefix);
}

bool UMHCompositeEditProjection::BuildDraftGraph(FMHRandomSourceGraph& OutGraph, FString& OutError)
{
    UMHCompositeEditSession* Owner = Session.Get();
    UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get();
    const AMHCompositeActor* Root = Owner != nullptr ? Owner->GetRootPlacement() : nullptr;
    const UMHCompositeAsset* RootAsset = Root != nullptr ? Root->GetCompositeAsset() : nullptr;
    const UMHCompositeAsset* Edited = Owner != nullptr ? Owner->GetEditedAsset() : nullptr;
    UMHCompositeEditDocument* Draft = Owner != nullptr ? Owner->GetDraft() : nullptr;
    if (Recipes == nullptr || RootAsset == nullptr || Edited == nullptr || Draft == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the edit projection lost its session");
        return false;
    }
    // The placement's published graph ...
    const FMHCompiledRecipe* RootRecipe = Recipes->Compile(*RootAsset, OutError);
    if (RootRecipe == nullptr || !MHBuildRecipeGraph(*RootRecipe, OutGraph, OutError)) return false;
    // ... with the edited definition replaced by the draft, compiled by the
    // same compiler through a transient asset (no second document->graph path).
    FMHCompositeDocument DraftDocument;
    TArray<uint8> DraftBytes;
    if (!Draft->Extract(DraftDocument, OutError) || !MHWriteCanonicalCompositeV5(DraftDocument, DraftBytes, OutError)) return false;
    if (DraftAsset == nullptr)
    {
        DraftAsset = NewObject<UMHCompositeAsset>(this, NAME_None, RF_Transient);
        DraftAsset->LogicalName = Edited->LogicalName;
        DraftAsset->SourceRelativePath = Edited->SourceRelativePath;
    }
    if (!MHApplyCompositeV5(*DraftAsset, DraftDocument, Draft->GetInlinedProfiles(), OutError)) return false;
    DraftAsset->AppliedHash = MHRawPayloadHash(DraftBytes);
    DraftAsset->SourceHash = DraftAsset->AppliedHash;
    Recipes->Invalidate(*DraftAsset);
    const FMHCompiledRecipe* DraftRecipe = Recipes->Compile(*DraftAsset, OutError);
    FMHRandomSourceGraph DraftGraph;
    if (DraftRecipe == nullptr || !MHBuildRecipeGraph(*DraftRecipe, DraftGraph, OutError)) return false;
    for (const TPair<FString, FMHRandomComposite>& Pair : DraftGraph.Composites)
    {
        OutGraph.Composites.Add(Pair.Key, Pair.Value);
    }
    for (const TPair<FString, FMHRandomPlacementProfile>& Pair : DraftGraph.Profiles)
    {
        OutGraph.Profiles.Add(Pair.Key, Pair.Value);
    }
    for (const TPair<FString, TArray<FString>>& Pair : DraftGraph.ResourceDependencies)
    {
        OutGraph.ResourceDependencies.Add(Pair.Key, Pair.Value);
    }
    return true;
}

USceneComponent* UMHCompositeEditProjection::PlaceComponent(const FString& Origin, UClass* Class, const FMatrix& WorldMatrix, const TFunction<void(USceneComponent&)>& Configure)
{
    AMHCompositeEditProjectionActor* Actor = ProjectionActor.Get();
    if (Actor == nullptr) return nullptr;
    USceneComponent* Component = ComponentsByOrigin.FindRef(Origin).Get();
    if (!IsValid(Component) || Component->GetClass() != Class)
    {
        RetireProjectionComponent(Component);
        Component = NewObject<USceneComponent>(Actor, Class, MakeUniqueObjectName(Actor, Class, TEXT("MH_EditNode")),
            RF_Transient | RF_DuplicateTransient);
        Actor->AddInstanceComponent(Component);
        Component->ComponentTags.Add(FName(*(TEXT("MHEdit.Origin:") + Origin)));
        Component->SetupAttachment(Actor->GetRootComponent());
        Component->SetAbsolute(true, true, true);
        Component->SetMobility(EComponentMobility::Movable);
        if (Configure) Configure(*Component);
        Component->RegisterComponent();
        ComponentsByOrigin.Add(Origin, Component);
    }
    Component->SetWorldTransform(FTransform(WorldMatrix), false, nullptr, ETeleportType::TeleportPhysics);
    return Component;
}

bool UMHCompositeEditProjection::Refresh(FString& OutError)
{
    UMHCompositeEditSession* Owner = Session.Get();
    AMHCompositeActor* Root = Owner != nullptr ? Owner->GetRootPlacement() : nullptr;
    if (Root == nullptr || !ProjectionActor.IsValid())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the edit projection is closed");
        return false;
    }
    FMHRandomSourceGraph Graph;
    if (!BuildDraftGraph(Graph, OutError)) return false;
    TSharedRef<FMHResolvedCompositePlan> NextPlan = MakeShared<FMHResolvedCompositePlan>();
    // Frozen seeds and call context of the placement: the draft is shown
    // exactly as this placement would show it after a save.
    if (!MHResolvePreviewGraph(Graph, Owner->GetFrozenSeed(), Owner->GetFrozenAppearanceSeed(), Owner->GetCallContext().ToResolveContext(), *NextPlan, OutError)) return false;
    // Admission precedes every component mutation. In particular, a parent
    // gesture may make an otherwise valid descendant matrix sheared or
    // singular even when the directly edited node still decomposes cleanly.
    if (!MHValidateResolvedPlacementTransforms(*NextPlan, Root->GetActorTransform(), OutError)) return false;
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    UMHEndpointPrototypeRegistry* Endpoints = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHEndpointPrototypeRegistry>() : nullptr;
    const FMatrix Basis = Root->GetActorTransform().ToMatrixWithScale();
    TSet<FString> Live;
    TArray<TObjectPtr<USceneComponent>> NextComponents;
    // Mesh leaves: the mesh itself with the placement's appearance channels.
    for (const FMHResolvedCompositeLeaf& Leaf : NextPlan->Leaves)
    {
        if (!UnderOccurrence(Leaf.Origin)) continue;
        Live.Add(Leaf.Origin);
        if (Leaf.Kind == EMHRandomSemanticKind::Mesh && Endpoints != nullptr && Settings != nullptr)
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::StaticMesh;
            Key.LogicalName = Leaf.Resource;
            bool bPlaceholder = false;
            FString ResolveError;
            UStaticMesh* Mesh = Endpoints->ResolveMeshForPreview(Key, *Settings, bPlaceholder, ResolveError);
            USceneComponent* Component = PlaceComponent(Leaf.Origin, UMHCompositeEditMeshComponent::StaticClass(), Leaf.WorldMatrix * Basis,
                [](USceneComponent& New)
                {
                    UStaticMeshComponent& MeshComponent = static_cast<UStaticMeshComponent&>(New);
                    MeshComponent.SetCollisionEnabled(ECollisionEnabled::NoCollision);
                    MeshComponent.SetCanEverAffectNavigation(false);
                });
            if (Component != nullptr) NextComponents.AddUnique(Component);
            if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component))
            {
                if (MeshComponent->GetStaticMesh() != Mesh) MeshComponent->SetStaticMesh(Mesh);
                MHApplyLeafAppearanceCustomData(MeshComponent, Leaf, Settings->AppearanceCustomDataBaseIndex);
            }
        }
        else
        {
            if (USceneComponent* Component = PlaceComponent(Leaf.Origin, USceneComponent::StaticClass(), Leaf.WorldMatrix * Basis, nullptr))
            {
                NextComponents.AddUnique(Component);
            }
        }
    }
    // Structural nodes (group, random, nested reference, actor, empty): a handle at the node.
    for (const FMHResolvedCompositeNode& Node : NextPlan->Nodes)
    {
        if (!Node.NodePath.StartsWith(DefinitionPrefix) || Live.Contains(Node.NodePath)) continue;
        if (Node.SemanticKind == EMHRandomSemanticKind::Mesh) continue;
        // A nested reference is atomic (spec A04): its geometry is shown and
        // maps to the reference node, nothing below its '>' gets a handle.
        if (Node.NodePath.Mid(DefinitionPrefix.Len()).Contains(TEXT(">"))) continue;
        Live.Add(Node.NodePath);
        if (USceneComponent* Component = PlaceComponent(Node.NodePath, USceneComponent::StaticClass(), Node.WorldMatrix * Basis, nullptr))
        {
            NextComponents.AddUnique(Component);
        }
    }
    // Retire components whose origin is gone.
    for (auto It = ComponentsByOrigin.CreateIterator(); It; ++It)
    {
        if (Live.Contains(It.Key())) continue;
        RetireProjectionComponent(It.Value().Get());
        It.RemoveCurrent();
    }
    // Build the frame and visual-binding tables from this exact resolved plan
    // and the current session ids.  Queries never reinterpret an origin using
    // a later draft selector, which is essential when origins/components are
    // reused after reorder or deletion.
    TMap<FString, FGuid> IdByAuthoringPath;
    TMap<FGuid, FMHCompositeEditNodeFrame> NextFrames;
    TMap<FGuid, TWeakObjectPtr<USceneComponent>> NextFrameComponents;
    TMap<TWeakObjectPtr<USceneComponent>, FGuid> NextBindings;
    const UMHCompositeEditDocument* Draft = Owner->GetDraft();
    if (Draft != nullptr)
    {
        for (int32 DraftIndex = 0; DraftIndex < Draft->Num(); ++DraftIndex)
        {
            const FGuid NodeId = Draft->GetNodeId(DraftIndex);
            const FString NodePath = DefinitionPrefix + Draft->GetSelector(DraftIndex);
            if (!NodeId.IsValid() || NodePath == DefinitionPrefix) continue;
            IdByAuthoringPath.Add(NodePath, NodeId);
            const FMHResolvedCompositeNode* Resolved = NextPlan->Nodes.FindByPredicate(
                [&NodePath](const FMHResolvedCompositeNode& Candidate)
                {
                    return Candidate.NodePath == NodePath;
                });
            if (Resolved == nullptr) continue;
            FMHCompositeEditNodeFrame& Frame = NextFrames.Add(NodeId);
            Frame.NodeId = NodeId;
            Frame.ParentNodeId = Draft->GetParentId(NodeId);
            const FMHCompositeAssetNode& DraftNode = Draft->GetNodes()[DraftIndex];
            Frame.AuthoredLocal = DraftNode.Transform;
            Frame.WorldMatrix = Resolved->WorldMatrix * Basis;
            Frame.ParentWorldMatrix = NextPlan->Nodes.IsValidIndex(Resolved->ParentResolvedNodeIndex)
                ? NextPlan->Nodes[Resolved->ParentResolvedNodeIndex].WorldMatrix * Basis
                : Basis;
            Frame.bGeneratedTransform = !DraftNode.Profile.IsEmpty() || DraftNode.bHasInlinePlacement;
        }
        for (const TPair<FString, TWeakObjectPtr<USceneComponent>>& Pair : ComponentsByOrigin)
        {
            USceneComponent* Component = Pair.Value.Get();
            if (!IsValid(Component)) continue;
            const FGuid NodeId = IdByAuthoringPath.FindRef(AuthoringPathForVisual(Pair.Key, DefinitionPrefix));
            if (!NodeId.IsValid() || !NextFrames.Contains(NodeId)) continue;
            NextBindings.Add(Component, NodeId);
            const FString* ExactPath = IdByAuthoringPath.FindKey(NodeId);
            if (ExactPath != nullptr && Pair.Key == *ExactPath) NextFrameComponents.Add(NodeId, Component);
        }
    }
    Components = MoveTemp(NextComponents);
    NodeIdByComponent = MoveTemp(NextBindings);
    NodeFrames = MoveTemp(NextFrames);
    FrameComponentByNodeId = MoveTemp(NextFrameComponents);
    UpdateSelection(Owner->GetSelectedNodeIds());
    PushEditingTint();
    Plan = NextPlan;
    return true;
}

void UMHCompositeEditProjection::Close()
{
    const UMHCompositeEditSession* Owner = Session.Get();
    const AMHCompositeActor* Root = Owner != nullptr ? Owner->GetRootPlacement() : nullptr;
    if (Lease.IsSet())
    {
        if (UMHInstancePoolSubsystem* Pool = Root != nullptr ? UMHInstancePoolSubsystem::Get(Root->GetWorld()) : nullptr) Pool->ReleaseSuppression(Lease);
        Lease = FMHPoolSuppressionLease();
    }
    if (AMHCompositeEditProjectionActor* Actor = ProjectionActor.Get())
    {
        // CE-6a: leave the selection before the actor is garbage — the engine
        // cannot deselect a pending-kill actor ("invalid flags") and the
        // selection set would keep dangling element references.
        if (GEditor != nullptr)
        {
            TArray<UActorComponent*> Selected;
            GEditor->GetSelectedComponents()->GetSelectedObjects(Selected);
            // Clear the unified set atomically: a restored Undo snapshot can
            // contain retired components whose owner is no longer selected.
            if (Actor->IsSelected() || Selected.ContainsByPredicate([Actor](const UActorComponent* Component) { return Component->GetOwner() == Actor; }))
                GEditor->SelectNone(true, true, false);
        }
        if (UWorld* World = Actor->GetWorld()) World->DestroyActor(Actor);
        else Actor->Destroy();
    }
    ProjectionActor.Reset();
    ComponentsByOrigin.Reset();
    NodeIdByComponent.Reset();
    NodeFrames.Reset();
    FrameComponentByNodeId.Reset();
    Components.Reset();
    Plan.Reset();
}

FString UMHCompositeEditProjection::GetOriginForComponent(const USceneComponent* Component) const
{
    if (Component == nullptr) return FString();
    for (const TPair<FString, TWeakObjectPtr<USceneComponent>>& Pair : ComponentsByOrigin)
    {
        if (Pair.Value.Get() == Component) return Pair.Key;
    }
    return FString();
}

FGuid UMHCompositeEditProjection::GetNodeIdForComponent(const USceneComponent* Component) const
{
    if (Component == nullptr) return FGuid();
    for (const TPair<TWeakObjectPtr<USceneComponent>, FGuid>& Pair : NodeIdByComponent)
    {
        if (Pair.Key.Get() == Component) return Pair.Value;
    }
    return FGuid();
}

void UMHCompositeEditProjection::PushEditingTint()
{
    // The engine's own path for actors of an edited Level Instance: the actor
    // flag behind it is private to the level streaming classes, so the state
    // goes straight to each proxy and follows proxy re-creation.
    for (auto It = TintedProxies.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid()) It.RemoveCurrent();
    }
    for (const TObjectPtr<USceneComponent>& Component : Components)
    {
        UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component.Get());
        if (Primitive == nullptr || Primitive->GetSceneProxy() == nullptr) continue;
        const FPrimitiveSceneProxy* Proxy = Primitive->GetSceneProxy();
        if (TintedProxies.FindRef(Primitive) == Proxy) continue;
        Primitive->PushLevelInstanceEditingStateToProxy(true);
        TintedProxies.Add(Primitive, Proxy);
    }
}

bool UMHCompositeEditProjection::GetParentWorldForComponent(const USceneComponent* Component, FTransform& OutParentWorld) const
{
    FMHCompositeEditNodeFrame Frame;
    if (!GetNodeFrame(GetNodeIdForComponent(Component), Frame)) return false;
    OutParentWorld = FTransform(Frame.ParentWorldMatrix);
    return true;
}

USceneComponent* UMHCompositeEditProjection::FindComponentForOrigin(const FString& Origin) const
{
    USceneComponent* Component = ComponentsByOrigin.FindRef(Origin).Get();
    return IsValid(Component) ? Component : nullptr;
}

USceneComponent* UMHCompositeEditProjection::FindComponentForNodeId(const FGuid& NodeId) const
{
    USceneComponent* Component = FrameComponentByNodeId.FindRef(NodeId).Get();
    return IsValid(Component) ? Component : nullptr;
}

bool UMHCompositeEditProjection::GetNodeFrame(const FGuid& NodeId, FMHCompositeEditNodeFrame& OutFrame) const
{
    const FMHCompositeEditNodeFrame* Frame = NodeFrames.Find(NodeId);
    if (Frame == nullptr) return false;
    OutFrame = *Frame;
    return true;
}

TArray<USceneComponent*> UMHCompositeEditProjection::GetComponentsForNodeId(const FGuid& NodeId, const bool bIncludeDescendants) const
{
    TArray<USceneComponent*> Result;
    if (!NodeFrames.Contains(NodeId)) return Result;
    auto IsRequestedNode = [this, &NodeId, bIncludeDescendants](FGuid Candidate)
    {
        if (Candidate == NodeId) return true;
        if (!bIncludeDescendants) return false;
        TSet<FGuid> Visited;
        while (Candidate.IsValid() && !Visited.Contains(Candidate))
        {
            Visited.Add(Candidate);
            const FMHCompositeEditNodeFrame* Frame = NodeFrames.Find(Candidate);
            if (Frame == nullptr) return false;
            Candidate = Frame->ParentNodeId;
            if (Candidate == NodeId) return true;
        }
        return false;
    };
    for (const TObjectPtr<USceneComponent>& Component : Components)
    {
        if (!IsValid(Component)) continue;
        const FGuid BoundId = GetNodeIdForComponent(Component);
        if (IsRequestedNode(BoundId)) Result.Add(Component);
    }
    return Result;
}

bool UMHCompositeEditProjection::GetNodeBounds(const FGuid& NodeId, FBox& OutBounds) const
{
    if (!NodeFrames.Contains(NodeId)) return false;
    FBox Bounds(ForceInit);
    for (USceneComponent* Component : GetComponentsForNodeId(NodeId, true))
    {
        const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
        if (Primitive != nullptr && Primitive->IsRegistered()) Bounds += Primitive->Bounds.GetBox();
    }
    if (!Bounds.IsValid) return false;
    OutBounds = Bounds;
    return true;
}

void UMHCompositeEditProjection::UpdateSelection(const TArray<FGuid>& NodeIds)
{
    TSet<USceneComponent*> SelectedVisuals;
    for (const FGuid& NodeId : NodeIds)
    {
        for (USceneComponent* Component : GetComponentsForNodeId(NodeId, true)) SelectedVisuals.Add(Component);
    }
    for (const TObjectPtr<USceneComponent>& Component : Components)
    {
        if (UMHCompositeEditMeshComponent* Mesh = Cast<UMHCompositeEditMeshComponent>(Component.Get()))
        {
            Mesh->SetEditSelected(SelectedVisuals.Contains(Mesh));
        }
    }
}
