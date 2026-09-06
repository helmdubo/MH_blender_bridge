#include "Editing/MHCompositeEditProjection.h"

#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditProjection)

using namespace UE::MimirComposite;

AMHCompositeEditProjectionActor::AMHCompositeEditProjectionActor()
{
    // Editor-only and transient: never saved with the map, never duplicated
    // into PIE, never cooked (spec §5.5, contract §2 "Временный контейнер").
    bIsEditorOnlyActor = true;
    bListedInSceneOutliner = false;
    SetActorHiddenInGame(true);
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
    if (Root == nullptr || Edited == nullptr || World == nullptr || Root->GetLevel() == nullptr || !InSession.IsNested())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: an edit projection needs a nested session on a placed root");
        return false;
    }
    OccurrencePrefix = InSession.GetInvocationPath() + TEXT(">");
    DefinitionPrefix = OccurrencePrefix + Edited->LogicalName + TEXT(":");
    // The same spawn shape as the engine's Level Instance editing actor:
    // transient, no actor package, not in the outliner, in the root's level.
    FActorSpawnParameters Params;
    Params.OverrideLevel = Root->GetLevel();
    Params.bHideFromSceneOutliner = true;
    Params.bCreateActorPackage = false;
    Params.ObjectFlags = RF_Transient;
    Params.bNoFail = true;
    AMHCompositeEditProjectionActor* Actor = World->SpawnActor<AMHCompositeEditProjectionActor>(Root->GetActorLocation(), Root->GetActorRotation(), Params);
    if (Actor == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the edit projection actor could not be spawned");
        return false;
    }
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
        if (Row.Handle.IsSet() && Row.NodePath.StartsWith(OccurrencePrefix)) Handles.Add(Row.Handle);
    }
    Lease = Pool->AcquireSuppression(Handles);
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
        if (IsValid(Component)) Component->DestroyComponent();
        Component = NewObject<USceneComponent>(Actor, Class, MakeUniqueObjectName(Actor, Class, TEXT("MH_EditNode")), RF_Transient);
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
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    UMHEndpointPrototypeRegistry* Endpoints = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHEndpointPrototypeRegistry>() : nullptr;
    const FMatrix Basis = Root->GetActorTransform().ToMatrixWithScale();
    TSet<FString> Live;
    // Mesh leaves: the mesh itself with the placement's appearance channels.
    for (const FMHResolvedCompositeLeaf& Leaf : NextPlan->Leaves)
    {
        if (!Leaf.Origin.StartsWith(OccurrencePrefix)) continue;
        Live.Add(Leaf.Origin);
        if (Leaf.Kind == EMHRandomSemanticKind::Mesh && Endpoints != nullptr && Settings != nullptr)
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::StaticMesh;
            Key.LogicalName = Leaf.Resource;
            bool bPlaceholder = false;
            FString ResolveError;
            UStaticMesh* Mesh = Endpoints->ResolveMeshForPreview(Key, *Settings, bPlaceholder, ResolveError);
            USceneComponent* Component = PlaceComponent(Leaf.Origin, UStaticMeshComponent::StaticClass(), Leaf.WorldMatrix * Basis,
                [](USceneComponent& New)
                {
                    UStaticMeshComponent& MeshComponent = static_cast<UStaticMeshComponent&>(New);
                    MeshComponent.SetCollisionEnabled(ECollisionEnabled::NoCollision);
                    MeshComponent.SetCanEverAffectNavigation(false);
                    MeshComponent.SetHiddenInGame(true);
                });
            if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component))
            {
                if (MeshComponent->GetStaticMesh() != Mesh) MeshComponent->SetStaticMesh(Mesh);
                MHApplyLeafAppearanceCustomData(MeshComponent, Leaf, Settings->AppearanceCustomDataBaseIndex);
            }
        }
        else
        {
            PlaceComponent(Leaf.Origin, USceneComponent::StaticClass(), Leaf.WorldMatrix * Basis, nullptr);
        }
    }
    // Structural nodes (group, random, nested reference, actor, empty): a handle at the node.
    for (const FMHResolvedCompositeNode& Node : NextPlan->Nodes)
    {
        if (!Node.NodePath.StartsWith(DefinitionPrefix) || Live.Contains(Node.NodePath)) continue;
        if (Node.SemanticKind == EMHRandomSemanticKind::Mesh) continue;
        Live.Add(Node.NodePath);
        PlaceComponent(Node.NodePath, USceneComponent::StaticClass(), Node.WorldMatrix * Basis, nullptr);
    }
    // Retire components whose origin is gone.
    for (auto It = ComponentsByOrigin.CreateIterator(); It; ++It)
    {
        if (Live.Contains(It.Key())) continue;
        if (USceneComponent* Stale = It.Value().Get()) Stale->DestroyComponent();
        It.RemoveCurrent();
    }
    Components.Reset();
    for (const TPair<FString, TWeakObjectPtr<USceneComponent>>& Pair : ComponentsByOrigin)
    {
        if (USceneComponent* Component = Pair.Value.Get()) Components.Add(Component);
    }
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
        if (UWorld* World = Actor->GetWorld()) World->DestroyActor(Actor);
        else Actor->Destroy();
    }
    ProjectionActor.Reset();
    ComponentsByOrigin.Reset();
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
    const UMHCompositeEditSession* Owner = Session.Get();
    const UMHCompositeEditDocument* Draft = Owner != nullptr ? Owner->GetDraft() : nullptr;
    const FString Origin = GetOriginForComponent(Component);
    if (Draft == nullptr || !Origin.StartsWith(DefinitionPrefix)) return FGuid();
    // Inside the edited definition: "nodes[1]/children[0]" or, for a leaf
    // picked by a random node, ".../options[k]" (the random node owns it).
    // Anything below a nested reference (">") belongs to that reference node.
    FString Selector = Origin.Mid(DefinitionPrefix.Len());
    int32 Nested = INDEX_NONE;
    if (Selector.FindChar(TEXT('>'), Nested)) Selector = Selector.Left(Nested);
    int32 Options = INDEX_NONE;
    if (Selector.FindLastChar(TEXT('/'), Options) && Selector.Mid(Options + 1).StartsWith(TEXT("options["))) Selector = Selector.Left(Options);
    const int32 Index = Draft->FindNodeIndexBySelector(Selector);
    return Index != INDEX_NONE ? Draft->GetNodeId(Index) : FGuid();
}

USceneComponent* UMHCompositeEditProjection::FindComponentForOrigin(const FString& Origin) const
{
    static_cast<void>(Origin);
    return nullptr;
}

USceneComponent* UMHCompositeEditProjection::FindComponentForNodeId(const FGuid& NodeId) const
{
    for (const TObjectPtr<USceneComponent>& Component : Components)
    {
        if (IsValid(Component) && GetNodeIdForComponent(Component) == NodeId) return Component;
    }
    return nullptr;
}
