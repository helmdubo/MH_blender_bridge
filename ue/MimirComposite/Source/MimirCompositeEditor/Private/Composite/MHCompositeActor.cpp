#include "Composite/MHCompositeActor.h"

#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHInstancePool.h"
#include "Composite/MHMaterializeLayout.h"
#include "Performance/MHPerformanceTrace.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHCompositeRuntimeBridge.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/CollisionProfile.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "LevelEditor.h"
#include "Logging/MessageLog.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Serialization/Archive.h"
#include "Serialization/ArchiveSavePackageData.h"
#include "Settings/MHCompositeSettings.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeActor)

DEFINE_LOG_CATEGORY_STATIC(LogMHCompositeActor, Display, All);

namespace
{
void BroadcastMHCompositeComponentsEdited()
{
    if (!IsRunningCommandlet() && FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
        FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor")).BroadcastComponentsEdited();
}

void DestroyMHRetiredComponents(const TArray<TObjectPtr<UActorComponent>>& Previous,
    const TArray<TObjectPtr<UActorComponent>>& Current)
{
    UE::MimirComposite::FMHPlacementStageScope Stage(
        UE::MimirComposite::EMHPlacementStage::DestroyRetiredComponents);
    if (Previous.IsEmpty()) return;
    // One membership set instead of a linear Contains per retired candidate.
    // Retirement order and the set of destroyed components are unchanged.
    const TSet<TObjectPtr<UActorComponent>> Kept(Current);
    for (int32 Index = Previous.Num() - 1; Index >= 0; --Index)
    {
        if (IsValid(Previous[Index]) && !Kept.Contains(Previous[Index]))
        {
            UE::MimirComposite::MHRecordPlacementComponentDestroyed();
            Previous[Index]->DestroyComponent();
        }
    }
}

void RecordMHPlacementReseedComparison(
    const UE::MimirComposite::FMHResolvedCompositePlan& Previous,
    const UE::MimirComposite::FMHResolvedCompositePlan& Candidate)
{
    using namespace UE::MimirComposite;
    TMap<FString, const FMHResolvedCompositeLeaf*> PreviousByPath;
    PreviousByPath.Reserve(Previous.Leaves.Num());
    for (const FMHResolvedCompositeLeaf& Leaf : Previous.Leaves) PreviousByPath.Add(Leaf.Origin, &Leaf);
    TSet<FString> CandidatePaths;
    CandidatePaths.Reserve(Candidate.Leaves.Num());
    uint64 Stable = 0;
    uint64 Changed = 0;
    uint64 Added = 0;
    for (const FMHResolvedCompositeLeaf& Leaf : Candidate.Leaves)
    {
        CandidatePaths.Add(Leaf.Origin);
        const FMHResolvedCompositeLeaf* const* Found = PreviousByPath.Find(Leaf.Origin);
        if (Found == nullptr)
        {
            ++Added;
        }
        else if ((*Found)->Kind == Leaf.Kind && (*Found)->Resource == Leaf.Resource)
        {
            ++Stable;
        }
        else
        {
            ++Changed;
        }
    }
    uint64 Removed = 0;
    for (const TPair<FString, const FMHResolvedCompositeLeaf*>& Pair : PreviousByPath)
        if (!CandidatePaths.Contains(Pair.Key)) ++Removed;
    MHRecordPlacementReseedComparison(
        Previous.Leaves.Num(), Candidate.Leaves.Num(), Stable, Changed, Added, Removed);
}
}

AMHCompositeActor::AMHCompositeActor()
{
    CompositeRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MHCompositeRoot"));
    SetRootComponent(CompositeRoot);
    PrimaryActorTick.bCanEverTick = false;
}

void AMHCompositeActor::Serialize(FArchive& Archive)
{
    Super::Serialize(Archive);
    const FArchiveSavePackageData* SaveData = Archive.GetSavePackageData();
    const FObjectSavePackageSerializeContext* SaveContext = SaveData != nullptr ? &SaveData->SavePackageContext : nullptr;
    if (Archive.IsCooking() && !IsTemplate() && SaveContext != nullptr &&
        SaveContext->GetPhase() != EObjectSaveContextPhase::CookDependencyHarvest &&
        !UE::MimirComposite::MHIsRuntimeCompositeCookPrepared(*this))
    {
        Archive.SetError();
        UE_LOG(LogMHCompositeActor, Error,
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s has no admitted runtime cook handoff"), *GetPathName());
    }
}

int32 AMHCompositeActor::GenerateAutoSeed(const int32 DifferentFrom)
{
    int32 Result = 0;
    do
    {
        const uint32 Bits = FGuid::NewGuid().A;
        FMemory::Memcpy(&Result, &Bits, sizeof(Result));
    } while (Result == 0 || Result == DifferentFrom);
    return Result;
}

void AMHCompositeActor::SetCallContext(const FMHCompositeCallContext& NewContext)
{
    if (CallContext.StreamNamespace == NewContext.StreamNamespace &&
        CallContext.AppearanceBoundary == NewContext.AppearanceBoundary &&
        CallContext.Version == NewContext.Version)
    {
        return;
    }
    Modify();
    CallContext = NewContext;
    RebuildComposite();
}

void AMHCompositeActor::SetSeed(const int32 NewSeed)
{
    if (Seed != NewSeed)
    {
        Modify();
        Seed = NewSeed;
    }
    RebuildPlacement(true);
}

void AMHCompositeActor::Reseed()
{
    SetSeed(GenerateAutoSeed(Seed));
}

void AMHCompositeActor::SetAutoSeed(const bool bEnabled)
{
    if (bAutoSeed != bEnabled)
    {
        Modify();
        bAutoSeed = bEnabled;
    }
}

void AMHCompositeActor::SetAppearanceSeed(const int32 NewSeed)
{
    if (AppearanceSeed != NewSeed || !bAppearanceSeedStored)
    {
        Modify();
        AppearanceSeed = NewSeed;
        bAppearanceSeedStored = true;
    }
    RebuildPlacement(true);
}

void AMHCompositeActor::ReseedAppearance()
{
    SetAppearanceSeed(GenerateAutoSeed(AppearanceSeed));
}

void AMHCompositeActor::SetAutoAppearanceSeed(const bool bEnabled)
{
    if (bAutoAppearanceSeed != bEnabled)
    {
        Modify();
        bAutoAppearanceSeed = bEnabled;
    }
}

const UE::MimirComposite::FMHResolvedCompositePlan* AMHCompositeActor::GetResolvedPlan() const
{
    using namespace UE::MimirComposite;
    if (IsPreviewLoading() || !bPlanAvailable || !ResidentPlan.IsValid() ||
        ResidentPlan->Seed != Seed ||
        ResidentPlan->Appearance.AppearanceSeed != AppearanceSeed ||
        !LastPlacementError.IsEmpty()) return nullptr;
    // R2b-2: the preview plan is resident; nothing is re-resolved on read.
    return ResidentPlan.Get();
}

void AMHCompositeActor::SetCompositeAsset(UMHCompositeAsset* Asset)
{
    Modify();
    if (CompositeAsset.Get() != Asset)
    {
        ObservedRecipeGraph.Reset();
        // Keep the committed graph/plan with its visible rows until the new
        // asset's selected mesh batch is ready. GetResolvedPlan hides it while
        // the candidate is pending; commit replaces it atomically.
        bPlanAvailable = false;
    }
    CompositeAsset = Asset;
    RebuildComposite();
}

UMHCompositeAsset* AMHCompositeActor::GetCompositeAsset() const
{
    return CompositeAsset.LoadSynchronous();
}

const UE::MimirComposite::FMHCompositeLeafMaterialization*
AMHCompositeActor::FindLeafMaterialization(
    const USceneComponent* Component, const int32 InstanceIndex) const
{
    // A pooled ISM address belongs to whichever owner the pool says (16 §2.8);
    // only this actor's own rows answer.
    if (const UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Component);
        Bucket != nullptr && InstanceIndex != INDEX_NONE)
    {
        if (const UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld()))
        {
            AActor* InstanceOwner = nullptr;
            FString NodePath;
            if (Pool->ReverseLookup(Bucket, InstanceIndex, InstanceOwner, NodePath))
                return InstanceOwner == this ? FindLeafMaterializationByNodePath(NodePath) : nullptr;
        }
    }
    RefreshPooledRows();
    return LeafMaterializations.FindByPredicate(
        [Component, InstanceIndex](const UE::MimirComposite::FMHCompositeLeafMaterialization& Row)
        {
            return Row.Component == Component && Row.InstanceIndex == InstanceIndex;
        });
}

const UE::MimirComposite::FMHCompositeLeafMaterialization*
AMHCompositeActor::FindLeafMaterializationByNodePath(const FString& NodePath) const
{
    RefreshPooledRows();
    return LeafMaterializations.FindByPredicate(
        [&NodePath](const UE::MimirComposite::FMHCompositeLeafMaterialization& Row)
        {
            return Row.NodePath == NodePath;
        });
}

void AMHCompositeActor::RefreshPooledRows() const
{
    const UMHInstancePoolSubsystem* Pool = nullptr;
    for (UE::MimirComposite::FMHCompositeLeafMaterialization& Row : LeafMaterializations)
    {
        if (!Row.Handle.IsSet()) continue;
        if (Pool == nullptr) Pool = UMHInstancePoolSubsystem::Get(GetWorld());
        if (Pool == nullptr) return;
        UInstancedStaticMeshComponent* Bucket = nullptr;
        int32 InstanceIndex = INDEX_NONE;
        if (!Pool->GetInstance(Row.Handle, Bucket, InstanceIndex)) continue;
        Row.Component = Bucket;
        Row.InstanceIndex = InstanceIndex;
    }
}

void AMHCompositeActor::SyncPoolVisibility()
{
    if (IsTemplate()) return;
    if (UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld()))
        Pool->SetOwnerEditorVisibility(*this, !IsHiddenEd());
}

bool AMHCompositeActor::SelectPlacementLeaf(
    const USceneComponent* Component, const int32 InstanceIndex)
{
    const UE::MimirComposite::FMHCompositeLeafMaterialization* Row =
        FindLeafMaterialization(Component, InstanceIndex);
    if (Row == nullptr) return false;
    return SelectPlacementLeafByNodePath(Row->NodePath);
}

bool AMHCompositeActor::SelectPlacementLeafByNodePath(const FString& NodePath)
{
    FString OccurrencePath;
    if (!FindPlacementOccurrenceForLeafPath(NodePath, OccurrencePath)) return false;
    SelectedPlacementLeafPath = NodePath;
    SelectedPlacementOccurrencePath = MoveTemp(OccurrencePath);
    if (UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld());
        Pool != nullptr && Pool->IsOwnerSelected(*this))
    {
        Pool->SetOwnerSelected(*this, true);
    }
    return true;
}

bool AMHCompositeActor::FindPlacementOccurrenceForLeafPath(
    const FString& LeafPath, FString& OutOccurrencePath) const
{
    OutOccurrencePath.Reset();
    const UE::MimirComposite::FMHCompositeLeafMaterialization* Row =
        FindLeafMaterializationByNodePath(LeafPath);
    if (Row == nullptr || !ResidentPlan.IsValid() ||
        !ResidentPlan->Nodes.IsValidIndex(Row->ResolvedNodeIndex)) return false;

    int32 NodeIndex = Row->ResolvedNodeIndex;
    for (int32 Depth = 0; Depth < ResidentPlan->Nodes.Num(); ++Depth)
    {
        if (!ResidentPlan->Nodes.IsValidIndex(NodeIndex)) return false;
        const UE::MimirComposite::FMHResolvedCompositeNode& Node = ResidentPlan->Nodes[NodeIndex];
        if (Node.SemanticKind == UE::MimirComposite::EMHRandomSemanticKind::Composite)
        {
            OutOccurrencePath = Node.NodePath;
            return true;
        }
        if (Node.SemanticKind == UE::MimirComposite::EMHRandomSemanticKind::Random &&
            Node.SelectedOptionIndex >= 0)
        {
            const FString OptionPath = FString::Printf(
                TEXT("%s/options[%d]"), *Node.NodePath, Node.SelectedOptionIndex);
            // A direct mesh option ends at /options[k]. Only descendants below
            // the option prove that the selected option is a composite occurrence.
            if (LeafPath.StartsWith(OptionPath + TEXT(">")))
            {
                OutOccurrencePath = OptionPath;
                return true;
            }
        }
        const int32 ParentIndex = Node.ParentResolvedNodeIndex;
        if (ParentIndex == INDEX_NONE) return true;
        if (ParentIndex < 0 || ParentIndex >= NodeIndex) return false;
        NodeIndex = ParentIndex;
    }
    return false;
}

void AMHCompositeActor::ClearPlacementLeafSelection()
{
    if (SelectedPlacementLeafPath.IsEmpty() && SelectedPlacementOccurrencePath.IsEmpty()) return;
    SelectedPlacementLeafPath.Reset();
    SelectedPlacementOccurrencePath.Reset();
    if (UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld());
        Pool != nullptr && Pool->IsOwnerSelected(*this))
    {
        Pool->SetOwnerSelected(*this, true);
    }
}

bool AMHCompositeActor::ShouldHighlightPlacementLeafPath(const FString& NodePath) const
{
    return SelectedPlacementLeafPath.IsEmpty() || NodePath == SelectedPlacementLeafPath;
}

void AMHCompositeActor::PrunePlacementLeafSelection()
{
    if (SelectedPlacementLeafPath.IsEmpty()) return;
    FString OccurrencePath;
    if (!FindPlacementOccurrenceForLeafPath(SelectedPlacementLeafPath, OccurrencePath))
    {
        SelectedPlacementLeafPath.Reset();
        SelectedPlacementOccurrencePath.Reset();
    }
    else
    {
        SelectedPlacementOccurrencePath = MoveTemp(OccurrencePath);
    }
    // New/reused pool slots were created before the retained logical path was
    // revalidated. Reapply the final scope to all of this owner's instances.
    if (UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld());
        Pool != nullptr && Pool->IsOwnerSelected(*this))
    {
        Pool->SetOwnerSelected(*this, true);
    }
}

bool AMHCompositeActor::DependsOnResource(const UE::MimirComposite::FMHResourceKey& Key) const
{
    using namespace UE::MimirComposite;
    // R2b-3: no dependency list on the actor. The root asset and the resident
    // recipe graph (composites, meshes, profiles) are what a notification can
    // hit; materials and textures reconcile per bucket (docs/16 §4, R4).
    if (Key.Kind == EMHResourceKind::Composite)
    {
        // The root is observed by identity, alive or not: a dead or replaced
        // root asset is exactly the notification this placement must react to.
        if (const UMHCompositeAsset* Asset = CompositeAsset.Get(); Asset != nullptr && Asset->LogicalName == Key.LogicalName) return true;
        if (!CompositeAsset.IsNull() && CompositeAsset.ToSoftObjectPath().GetAssetName() == Key.LogicalName) return true;
    }
    const FMHRandomSourceGraph* Graph = ObservedRecipeGraph.IsValid() ? ObservedRecipeGraph.Get() : AppliedGraph.Get();
    if (Graph == nullptr) return false;
    TSet<FMHResourceKey> Observed;
    MHCollectRecipeGraphDependencies(*Graph, Observed);
    return Observed.Contains(Key);
}

void AMHCompositeActor::AttachRootTransformHook()
{
    if (CompositeRoot != nullptr && !IsTemplate())
    {
        CompositeRoot->TransformUpdated.RemoveAll(this);
        CompositeRoot->TransformUpdated.AddUObject(this, &AMHCompositeActor::UpdatePlacementBasis);
    }
}

TArray<TObjectPtr<UActorComponent>> AMHCompositeActor::CollectPreviousDerivedComponents() const
{
    // The tracking arrays are transient: a duplicated, pasted or reloaded actor
    // can carry MH-tagged instance components the arrays no longer know about.
    // Feed those into the reuse index and the retirement set alike, so a stale
    // twin is either adopted by its tag or destroyed - never accumulated.
    TArray<TObjectPtr<UActorComponent>> Previous = DerivedComponents;
    TSet<const UActorComponent*> Tracked;
    Tracked.Reserve(Previous.Num());
    for (const UActorComponent* Component : Previous) Tracked.Add(Component);
    for (UActorComponent* Component : GetInstanceComponents())
    {
        if (!IsValid(Component) || Tracked.Contains(Component)) continue;
        for (const FName& Tag : Component->ComponentTags)
        {
            if (Tag.ToString().StartsWith(TEXT("MH.")))
            {
                Previous.Add(Component);
                break;
            }
        }
    }
    return Previous;
}

void AMHCompositeActor::ClearDerivedComponents()
{
    CancelPendingPlacement();
    // Pooled leaves are released with the owner (16 §2.8); Undo rebuilds them
    // from the actor's record afterwards (OPEN-R-1).
    if (UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld())) Pool->RemoveOwner(*this);
    // Undo can restore transaction-era plan-view components after the transient
    // tracking arrays were cleared. Include every MH-tagged instance component
    // so rebuilding from the actor record cannot leave an untracked twin.
    const TArray<TObjectPtr<UActorComponent>> Previous = CollectPreviousDerivedComponents();
    DerivedComponents.Reset();
    DestroyMHRetiredComponents(Previous, DerivedComponents);
    TopLevelPlacementComponents.Reset();
    LeafPlacementComponents.Reset();
    LeafMaterializations.Reset();
    LastPlacementWarnings.Reset();
    LastPlacementError.Reset();
    ResidentPlan.Reset();
    AppliedGraph.Reset();
    ObservedRecipeGraph.Reset();
    bPlanAvailable = false;
    bBasisRejected = false;
    BroadcastMHCompositeComponentsEdited();
}

void AMHCompositeActor::ReportPlacementError()
{
    if (LastPlacementError.IsEmpty()) return;
    const FString Diagnostic = CompositeAsset.ToSoftObjectPath().ToString() + TEXT(": ") + LastPlacementError;
    // Missing applied dependencies are a visible, recoverable placement state,
    // not an engine error. The machine-readable code remains in the message.
    UE_LOG(LogMHCompositeActor, Warning, TEXT("%s"), *Diagnostic);
    if (!IsRunningCommandlet()) FMessageLog(TEXT("Mimir")).Warning(FText::FromString(Diagnostic));
}

void AMHCompositeActor::RebuildComposite()
{
    RebuildPlacement(false);
}

void AMHCompositeActor::ReconcileEndpoint(const UE::MimirComposite::FMHResourceKey& Key,
    const UE::MimirComposite::FMHEndpointInterfaceDelta& Delta)
{
    using namespace UE::MimirComposite;
    if (!Delta.Any() || bRebuildInProgress || IsTemplate() || IsActorBeingDestroyed() ||
        IsRunningCookCommandlet() || (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::PIE)) return;
    // The contract explicitly retains the missing-endpoint recovery path until R4.
    if (Delta.bFirstAdmission)
    {
        if (!bPlanAvailable || !ResidentPlan.IsValid() || ResidentPlan->Leaves.ContainsByPredicate(
            [&](const FMHResolvedCompositeLeaf& Leaf)
            { return Leaf.Kind == EMHRandomSemanticKind::Mesh && Leaf.Resource == Key.LogicalName; }))
            RebuildComposite();
        return;
    }
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (Registry == nullptr) return;
    UStaticMesh* Mesh = Cast<UStaticMesh>(Registry->Resolve(Key).Object.Get());
    if (Mesh == nullptr) return;
    TGuardValue<bool> Guard(bRebuildInProgress, true);
    // Pooled leaves were reconciled by the level pool before this call (R5b-0,
    // one pass per bucket): adopt the current bucket components behind the handles.
    bool bMigrated = false;
    if (Delta.bBucketDescriptor)
    {
        // R5-F: one pass, each row resolves its own handle and both derived
        // views follow it together. A getter that refreshed every row at once
        // hid the migration from the rows after the first one and left the
        // compatibility array stale (audit 2026-09-05 §2.1).
        if (const UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld()))
        {
            for (int32 Index = 0; Index < LeafMaterializations.Num(); ++Index)
            {
                FMHCompositeLeafMaterialization& Row = LeafMaterializations[Index];
                if (!Row.Handle.IsSet()) continue;
                UInstancedStaticMeshComponent* Bucket = nullptr;
                int32 InstanceIndex = INDEX_NONE;
                if (!Pool->GetInstance(Row.Handle, Bucket, InstanceIndex) || Bucket == nullptr) continue;
                bMigrated |= Row.Component != Bucket;
                Row.Component = Bucket;
                Row.InstanceIndex = InstanceIndex;
                if (LeafPlacementComponents.IsValidIndex(Index) && LeafPlacementComponents[Index] != Bucket)
                {
                    LeafPlacementComponents[Index] = Bucket;
                    bMigrated = true;
                }
            }
        }
    }
    // Static mesh components this actor materializes itself (the leaf extracted
    // for Placement Edit Mode, or every static leaf without a pool).
    for (TObjectPtr<UActorComponent>& Entry : DerivedComponents)
    {
        UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(Entry);
        if (!IsValid(Component) || Component->GetStaticMesh() != Mesh) continue;
        if (Delta.bMaterialBinding) Component->EmptyOverrideMaterials();
        if (Delta.bCollisionInterface) Component->RecreatePhysicsState();
        Component->MarkRenderStateDirty();
        Component->UpdateBounds();
        MHRecordReimportBucket(false);
    }
    // Bounds are derived directly from components; the actor has no separate cache.
    // Payload/bounds-only refresh deliberately emits no Outliner tree invalidation.
    if (bMigrated)
    {
        ++PreviewRevision;
        BroadcastMHCompositeComponentsEdited();
    }
}

void AMHCompositeActor::ReconcileRecipe(const UE::MimirComposite::FMHResourceKey& Key)
{
    if (Key.Kind != EMHResourceKind::Composite || bRebuildInProgress ||
        IsTemplate() || IsActorBeingDestroyed() || IsRunningCookCommandlet() ||
        (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::PIE)) return;
    const UInstancedStaticMeshComponent* Defaults = GetDefault<UInstancedStaticMeshComponent>();
    for (UActorComponent* Entry : DerivedComponents)
    {
        UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Entry);
        if (Bucket == nullptr || Bucket->GetStaticMesh() == nullptr || Bucket->GetStaticMesh()->GetBodySetup() == nullptr) continue;
        // DECIDED-R3B-2: the existing full compiler assigns these same values
        // through setters that discard the profile name. Canonicalize only an
        // exactly equivalent default policy, without changing collision behavior.
        if (Bucket->GetCollisionProfileName() == UCollisionProfile::CustomCollisionProfileName &&
            Bucket->GetCollisionEnabled() == Defaults->GetCollisionEnabled() &&
            Bucket->GetCollisionObjectType() == Defaults->GetCollisionObjectType() &&
            Bucket->GetCollisionResponseToChannels() == Defaults->GetCollisionResponseToChannels())
            Bucket->SetCollisionProfileName(Defaults->GetCollisionProfileName());
    }
    RebuildPlacement(false, true);
}

void AMHCompositeActor::RebuildPlacement(const bool bSeedOnly, const bool bRecipeChanged)
{
    using namespace UE::MimirComposite;
    // Play/cook consumes a fresh applied-input snapshot through the runtime
    // wrapper. Never manufacture an editor preview in either handoff world.
    if (IsRunningCookCommandlet() ||
        (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::PIE)) return;
    // Undo of creation marks garbage without running Destroyed(). Such an
    // actor must never recreate preview geometry in the shared level pool.
    if (!IsValid(this) || bRebuildInProgress || IsTemplate() || IsActorBeingDestroyed()) return;
    TGuardValue<bool> Guard(bRebuildInProgress, true);
    CancelPendingPlacement();
    ++PlacementRebuildCount;
    // Instrumentation for the S6.2 lifecycle guard: placement components may
    // only be created once this actor's own components are already registered.
    if (!HasActorRegisteredAllComponents()) ++PlacementUnregisteredBuildCount;
    bBasisRejected = false;
    AttachRootTransformHook();
    if (CompositeAsset.ToSoftObjectPath().IsNull())
    {
#if WITH_EDITORONLY_DATA
        SelectedMeshDependencies.Reset();
#endif
        ClearPlacementLeafSelection();
        ClearDerivedComponents();
        return;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr) return;
    UMHCompositeAsset* Asset = GetCompositeAsset();
    const FString Name = Asset != nullptr ? Asset->LogicalName : CompositeAsset.ToSoftObjectPath().GetAssetName();
    FMHResourceKey RootKey;
    RootKey.Kind = EMHResourceKind::Composite;
    RootKey.LogicalName = Name;

    // Preview plane (Recipe Model v2 §2.1, §2.5, R2b-2): compile the recipe
    // (cached by asset + RecipeRevision) and materialize the layout. No applied
    // graph, no closure, no receipt versus Source Root, no definition cache;
    // the definition-cache counters now report recipe cache hits and misses.
    TSharedPtr<const FMHRandomSourceGraph> CandidateGraph;
    TSharedPtr<const FMHResolvedCompositePlan> CandidatePlan;
    FString Error;
    if (Asset == nullptr)
    {
        Error = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: composite:") + Name + TEXT(" has no generated asset");
    }
    else if (UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get())
    {
        // Identity admission of the root through the endpoint registry (§2.4):
        // the canonical path must resolve to this very asset. No tag query, no
        // receipt versus Source Root; that is the proof plane's job.
        FString AdmissionError;
        const FMHCompiledRecipe* Recipe = nullptr;
        if (UMHEndpointPrototypeRegistry::ResolveEndpoint(RootKey, AdmissionError) != Asset)
        {
            Error = AdmissionError.IsEmpty()
                ? TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: ") + RootKey.ToString() + TEXT(" does not identify this unique generated asset")
                : AdmissionError;
        }
        else
        {
            if (Recipes->Find(*Asset) != nullptr) MHRecordDefinitionCacheHit();
            else MHRecordDefinitionCacheMiss();
            Recipe = Recipes->Compile(*Asset, Error);
        }
        if (Recipe != nullptr)
        {
            FMHMaterializeResult Materialized;
            {
                FMHPlacementStageScope Stage(EMHPlacementStage::ResolveCompositePlan);
                Materialized = MHMaterializeLayout(*Recipe, Seed, AppearanceSeed, CallContext.ToResolveContext(), GetActorTransform());
            }
            if (Materialized.Graph.IsValid())
            {
                CandidateGraph = Materialized.Graph;
                ObservedRecipeGraph = CandidateGraph;
                MHRecordMapLoadGraph(Name, *CandidateGraph);
            }
            if (Materialized.Succeeded())
            {
                CandidatePlan = Materialized.Plan;
                MHRecordMapLoadSelectedPlan(*CandidatePlan);
            }
            else
            {
                Error = Materialized.Error;
            }
        }
    }
    else
    {
        Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: compiled recipe registry is unavailable");
    }
    if (!Error.IsEmpty() && !Error.StartsWith(TEXT("MH_E_")))
        Error = TEXT("MH_E_COMPOSITE_GRAMMAR: ") + Error;
    LastPlacementWarnings.Reset();
    LastPlacementError = Error;
    if (!Error.IsEmpty())
    {
        bPlanAvailable = false;
        if (Error.Contains(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM")))
        {
            // No mutation of either old or new components at this boundary.
            ReportPlacementError();
            return;
        }
        FMHCompositePlacementCompileResult View;
        const TArray<TObjectPtr<UActorComponent>> Previous = CollectPreviousDerivedComponents();
        if (ResidentPlan.IsValid() && AppliedGraph.IsValid() && AppliedGraph->RootComposite == Name)
        {
            const FMHRandomComposite* PreviousRoot = AppliedGraph->Composites.Find(Name);
            if (PreviousRoot != nullptr)
            {
                View = MHCompileCompositePlacementV5(
                    *this, *ResidentPlan, *PreviousRoot, *Settings, Previous);
            }
        }
        FMHCompositePlacementCompileResult Marker = MHBuildCompositeDiagnosticView(*this, TEXT("composite:") + Name, Error);
        View.Components.Append(Marker.Components);
        View.Warnings.Append(Marker.Warnings);
        DerivedComponents = MoveTemp(View.Components);
        TopLevelPlacementComponents = MoveTemp(View.TopLevelComponents);
        LeafPlacementComponents = MoveTemp(View.LeafComponents);
        LeafMaterializations = MoveTemp(View.LeafMaterializations);
        LastPlacementWarnings = MoveTemp(View.Warnings);
        DestroyMHRetiredComponents(Previous, DerivedComponents);
        SyncPoolVisibility();
        BroadcastMHCompositeComponentsEdited();
        ReportPlacementError();
        return;
    }
    if (!CandidateGraph.IsValid() || !CandidatePlan.IsValid()) return;

    PendingPlacementGraph = CandidateGraph;
    PendingPlacementPlan = CandidatePlan;
    bPendingSeedOnly = bSeedOnly;
    bPendingRecipeChanged = bRecipeChanged;
    ++PendingPlacementEpoch;
    for (const FMHResolvedCompositeLeaf& Leaf : CandidatePlan->Leaves)
    {
        if (Leaf.Kind != EMHRandomSemanticKind::Mesh) continue;
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::StaticMesh;
        Key.LogicalName = Leaf.Resource;
        PendingSelectedMeshKeys.Add(MoveTemp(Key));
    }
    if (UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get())
    {
        EndpointLoadReadyHandle = Registry->OnEndpointLoadReady().AddUObject(
            this, &AMHCompositeActor::OnEndpointLoadReady);
    }
    FString EndpointError;
    if (!PendingEndpointsSettled(EndpointError))
    {
        // Refresh read-only editor surfaces into an explicit Loading state.
        // PreviewRevision remains the revision of the last committed view.
        BroadcastMHCompositeComponentsEdited();
        return;
    }
    if (!EndpointError.IsEmpty())
    {
        LastPlacementError = MoveTemp(EndpointError);
        bPlanAvailable = false;
        CancelPendingPlacement();
        ReportPlacementError();
        return;
    }
    CommitPendingPlacement();
}

void AMHCompositeActor::CancelPendingPlacement()
{
    ++PendingPlacementEpoch;
    if (EndpointLoadReadyHandle.IsValid())
    {
        if (UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get())
            Registry->OnEndpointLoadReady().Remove(EndpointLoadReadyHandle);
        EndpointLoadReadyHandle.Reset();
    }
    PendingPlacementGraph.Reset();
    PendingPlacementPlan.Reset();
    PendingSelectedMeshKeys.Reset();
    PendingSelectedMeshes.Reset();
    bPendingSeedOnly = false;
    bPendingRecipeChanged = false;
}

bool AMHCompositeActor::PendingEndpointsSettled(FString& OutError)
{
    using namespace UE::MimirComposite;
    OutError.Reset();
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (Registry == nullptr)
    {
        OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: endpoint prototype registry unavailable");
        return true;
    }
    bool bSettled = true;
    FMHPlacementStageScope LoadStage(EMHPlacementStage::LoadEndpoints);
    for (const FMHResourceKey& Key : PendingSelectedMeshKeys)
    {
        const FMHEndpointPrototype& Prototype = Registry->Resolve(Key);
        if (Prototype.State == EMHEndpointState::Loading)
        {
            bSettled = false;
            continue;
        }
        if (Prototype.State == EMHEndpointState::Ready)
        {
            UStaticMesh* Mesh = Cast<UStaticMesh>(Prototype.Object.Get());
            if (Mesh != nullptr) PendingSelectedMeshes.AddUnique(Mesh);
            continue;
        }
        if (!Prototype.AdmissionError.IsEmpty())
        {
            OutError = Prototype.AdmissionError;
            return true;
        }
    }
    return bSettled;
}

void AMHCompositeActor::OnEndpointLoadReady(const UE::MimirComposite::FMHResourceKey& Key)
{
    if (!PendingPlacementPlan.IsValid() || !PendingSelectedMeshKeys.Contains(Key) ||
        bRebuildInProgress || IsTemplate() || IsActorBeingDestroyed()) return;
    const uint64 ExpectedEpoch = PendingPlacementEpoch;
    TGuardValue<bool> Guard(bRebuildInProgress, true);
    FString EndpointError;
    if (!PendingEndpointsSettled(EndpointError) || ExpectedEpoch != PendingPlacementEpoch) return;
    if (!EndpointError.IsEmpty())
    {
        LastPlacementError = MoveTemp(EndpointError);
        bPlanAvailable = false;
        CancelPendingPlacement();
        ReportPlacementError();
        return;
    }
    CommitPendingPlacement();
}

void AMHCompositeActor::CommitPendingPlacement()
{
    using namespace UE::MimirComposite;
    const TSharedPtr<const FMHRandomSourceGraph> CandidateGraph = PendingPlacementGraph;
    const TSharedPtr<const FMHResolvedCompositePlan> CandidatePlan = PendingPlacementPlan;
    const bool bSeedOnly = bPendingSeedOnly;
    const bool bRecipeChanged = bPendingRecipeChanged;
    if (!CandidateGraph.IsValid() || !CandidatePlan.IsValid()) return;
    const FMHRandomComposite* Root = CandidateGraph->Composites.Find(CandidateGraph->RootComposite);
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Root == nullptr || Settings == nullptr)
    {
        CancelPendingPlacement();
        return;
    }
    const bool bLayoutReseed = bSeedOnly && bPlanAvailable && ResidentPlan.IsValid() &&
        ResidentPlan->Seed != CandidatePlan->Seed;
    TSharedPtr<const FMHResolvedCompositePlan> PreviousPlan;
    if ((bLayoutReseed || (bRecipeChanged && bPlanAvailable)) && ResidentPlan.IsValid())
    {
        // The previous preview plan is resident: the reseed diff never re-resolves.
        PreviousPlan = ResidentPlan;
        if (bLayoutReseed) RecordMHPlacementReseedComparison(*PreviousPlan, *CandidatePlan);
    }
    SeedAffectsResult = MHClassifyCompositeGraph(*CandidateGraph);
    // None means visual invariance, not absence of random draws. Resolve above
    // still refreshes decision traces for single-option and zero-deviation nodes.
    bool bComponentsEditedBroadcast = false;
    const auto CompileFullView = [&]() -> bool
    {
        const TArray<TObjectPtr<UActorComponent>> Previous = CollectPreviousDerivedComponents();
        FMHCompositePlacementCompileResult View = MHCompileCompositePlacementV5(
            *this, *CandidatePlan, *Root, *Settings, Previous);
        if (!View.Succeeded())
        {
            LastPlacementError = View.Error;
            bPlanAvailable = false;
            CancelPendingPlacement();
            ReportPlacementError();
            return false;
        }
        DerivedComponents = MoveTemp(View.Components);
        TopLevelPlacementComponents = MoveTemp(View.TopLevelComponents);
        LeafPlacementComponents = MoveTemp(View.LeafComponents);
        LeafMaterializations = MoveTemp(View.LeafMaterializations);
        LastPlacementWarnings = MoveTemp(View.Warnings);
        DestroyMHRetiredComponents(Previous, DerivedComponents);
        if (Previous != DerivedComponents)
        {
            BroadcastMHCompositeComponentsEdited();
            bComponentsEditedBroadcast = true;
        }
        return true;
    };
    bool bViewCompiled = false;
    if (PreviousPlan.IsValid() &&
        (bRecipeChanged || (bLayoutReseed && SeedAffectsResult != EMHCompositeSeedEffect::None)))
    {
        const TArray<TObjectPtr<UActorComponent>> Previous = CollectPreviousDerivedComponents();
        FMHCompositePlacementCompileResult View;
        if (MHTryCompileCompositePlacementReseedV5(
                *this, *PreviousPlan, *CandidatePlan, *Root, *Settings, Previous,
                TopLevelPlacementComponents, LeafPlacementComponents,
                LeafMaterializations, View))
        {
            if (!View.Succeeded())
            {
                LastPlacementError = View.Error;
                bPlanAvailable = false;
                CancelPendingPlacement();
                ReportPlacementError();
                return;
            }
            DerivedComponents = MoveTemp(View.Components);
            TopLevelPlacementComponents = MoveTemp(View.TopLevelComponents);
            LeafPlacementComponents = MoveTemp(View.LeafComponents);
            LeafMaterializations = MoveTemp(View.LeafMaterializations);
            LastPlacementWarnings = MoveTemp(View.Warnings);
            DestroyMHRetiredComponents(Previous, DerivedComponents);
            if (Previous != DerivedComponents)
            {
                BroadcastMHCompositeComponentsEdited();
                bComponentsEditedBroadcast = true;
            }
            if (bLayoutReseed) MHRecordPlacementReseedIncrementalApplied();
            bViewCompiled = true;
        }
        else
        {
            if (bLayoutReseed) MHRecordPlacementReseedFullFallback();
        }
    }
    if (!bViewCompiled && !(bSeedOnly && bPlanAvailable && SeedAffectsResult == EMHCompositeSeedEffect::None))
    {
        if (!CompileFullView()) return;
    }
    // S6.3.1: a skipped recompile still refreshes the appearance Custom
    // Primitive Data - the channels depend on AppearanceSeed alone. A leaf
    // view that no longer matches the plan is repaired by the full path.
    else if (!bViewCompiled && MHApplyCompositePlacementAppearance(LeafMaterializations,
                 *CandidatePlan, Settings->AppearanceCustomDataBaseIndex) == INDEX_NONE)
    {
        if (!CompileFullView()) return;
    }
    AppliedGraph = CandidateGraph;
    ResidentPlan = CandidatePlan;
    PrunePlacementLeafSelection();
#if WITH_EDITORONLY_DATA
    // Only a changed successful commit dirties the dependency hints. In
    // particular, a cold mesh finishing after Save must remain saveable;
    // reopening a map with unchanged hints must not dirty it again.
    TArray<TObjectPtr<UStaticMesh>> NewDependencies = PendingSelectedMeshes;
    NewDependencies.Sort([](const UStaticMesh& A, const UStaticMesh& B)
    {
        return A.GetPathName() < B.GetPathName();
    });
    if (SelectedMeshDependencies != NewDependencies)
    {
        SelectedMeshDependencies = MoveTemp(NewDependencies);
        MarkPackageDirty();
    }
#endif
    ++PreviewRevision;
    bPlanAvailable = true;
    CancelPendingPlacement();
    SyncPoolVisibility();
    // The existing Level Editor component-edited event is also the read-only
    // semantic-overlay invalidation signal. A reseed can preserve every
    // component pointer, so component-array inequality alone is insufficient.
    // Outliner listeners defer their tree work to the next Slate tick.
    if (!bComponentsEditedBroadcast) BroadcastMHCompositeComponentsEdited();
}

void AMHCompositeActor::UpdatePlacementBasis(USceneComponent*, EUpdateTransformFlags, ETeleportType)
{
    using namespace UE::MimirComposite;
    if (bRebuildInProgress) return;
    const bool bMovingRetainedView = IsPreviewLoading();
    if (!ResidentPlan.IsValid() || !AppliedGraph.IsValid()) return;
    if (!bMovingRetainedView &&
        (ResidentPlan->Seed != Seed || ResidentPlan->Appearance.AppearanceSeed != AppearanceSeed)) return;
    if (!bPlanAvailable && !bBasisRejected && !bMovingRetainedView)
    {
        // The cached plan may predate a rejected dependency update. Never let
        // moving the actor clear that failure using the older graph.
        RebuildComposite();
        return;
    }
    const FMHRandomComposite* Root = AppliedGraph->Composites.Find(AppliedGraph->RootComposite);
    if (Root == nullptr) return;
    bool bDesynchronized = false;
    {
        TGuardValue<bool> Guard(bRebuildInProgress, true);
        FString Error;
        // R2b-2: the resident preview plan moves with the actor; no Layout here.
        const TSharedPtr<const FMHResolvedCompositePlan> MaterializationPlan = ResidentPlan;
        if (!MaterializationPlan.IsValid()) Error = TEXT("no resident preview plan");
        if (!MaterializationPlan.IsValid() ||
            !MHUpdateCompositePlacementBasis(*this, *MaterializationPlan, *Root,
                TopLevelPlacementComponents, LeafPlacementComponents,
                LeafMaterializations, Error))
        {
            if (!Error.StartsWith(TEXT("MH_E_")))
                Error = TEXT("MH_E_PLACEMENT_STATE_DESYNC: resident placement state: ") + Error;
            LastPlacementError = Error;
            bPlanAvailable = false;
            bBasisRejected = true;
            ReportPlacementError();
            bDesynchronized = Error.StartsWith(TEXT("MH_E_PLACEMENT_STATE_DESYNC"));
        }
        else if (bBasisRejected && !bMovingRetainedView)
        {
            LastPlacementError.Reset();
            bPlanAvailable = true;
            bBasisRejected = false;
        }
    }
    if (!bDesynchronized) return;
    // A component view that no longer matches the plan is never repaired by a
    // partial walk over the shorter array. Rebuild the whole placement instead,
    // outside the reentrancy guard the basis update runs under.
    ++PlacementDesyncCount;
    RebuildComposite();
}

void AMHCompositeActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    AttachRootTransformHook();
    if (IsPreviewLoading())
    {
        UpdatePlacementBasis(nullptr, EUpdateTransformFlags::None, ETeleportType::None);
        return;
    }
    if (!ResidentPlan.IsValid() && LastPlacementError.IsEmpty()) RebuildComposite();
    else UpdatePlacementBasis(nullptr, EUpdateTransformFlags::None, ETeleportType::None);
}

void AMHCompositeActor::PostActorCreated()
{
    Super::PostActorCreated();
    if (!IsTemplate())
    {
        if (bAutoSeed) Seed = GenerateAutoSeed();
        if (bAutoAppearanceSeed) AppearanceSeed = GenerateAutoSeed();
        // A newly created placement authors its own AppearanceSeed, including an
        // explicit zero when auto is off. It is never a migration candidate.
        bAppearanceSeedStored = true;
    }
    AttachRootTransformHook();
}

void AMHCompositeActor::PostLoad()
{
    Super::PostLoad();
    // Migration (§3): a placement saved before this slice carries no stored
    // AppearanceSeed. Materialize it exactly once, here, into the property.
    // This is data, not components, so it is legal in PostLoad; and it is not a
    // computed default, so a later Seed reroll cannot move the appearance.
    if (!IsTemplate() && !bAppearanceSeedStored)
    {
        AppearanceSeed = UE::MimirComposite::MHDeriveAppearanceSeedFromLayoutSeed(Seed);
        bAppearanceSeedStored = true;
        bNeedsAppearanceSeedDirty = true;
    }
    AttachRootTransformHook();
    // A loaded actor is not required to be in a world yet, so PostLoad cannot
    // create or register placement components. Record the need; the single
    // admitted first-build point below runs once the actor is registered.
    bNeedsInitialPlacementBuild = true;
}

void AMHCompositeActor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    // The migrated AppearanceSeed is already in the property; only the dirty
    // flag has to wait until the package is no longer loading.
    if (bNeedsAppearanceSeedDirty)
    {
        bNeedsAppearanceSeedDirty = false;
        MarkPackageDirty();
    }
    // The one lifecycle point where the actor is in a world and its own
    // components are already registered. OnConstruction stays a basis update:
    // it runs on every PostEditMove and must never become a full rebuild.
    if (!bNeedsInitialPlacementBuild) return;
    bNeedsInitialPlacementBuild = false;
    UE::MimirComposite::FMHMapLoadInitialBuildScope PerfScope(*this);
    RebuildComposite();
    PerfScope.Complete(*this);
}

void AMHCompositeActor::PostDuplicate(const EDuplicateMode::Type DuplicateMode)
{
    Super::PostDuplicate(DuplicateMode);
    if (DuplicateMode == EDuplicateMode::Normal && !IsTemplate())
    {
        // Mirror of the existing layout auto-seed, one gate per seed.
        if (bAutoSeed) Seed = GenerateAutoSeed(Seed);
        if (bAutoAppearanceSeed) AppearanceSeed = GenerateAutoSeed(AppearanceSeed);
        bAppearanceSeedStored = true;
    }
    AttachRootTransformHook();
    // Field defect: a duplicate that built here, before its components were
    // registered, left its transient tracking arrays out of sync with the
    // editor's later text re-import and doubled every node. Extend the S6.2
    // single-build-point invariant to duplication: defer to registration.
    if (HasActorRegisteredAllComponents()) RebuildComposite();
    else bNeedsInitialPlacementBuild = true;
}

FBox AMHCompositeActor::GetComponentsBoundingBox(const bool bNonColliding, const bool bIncludeFromChildActors) const
{
    FBox Bounds = Super::GetComponentsBoundingBox(bNonColliding, bIncludeFromChildActors);
    // Pooled leaves are not components of this actor (16 §2.8); F / focus and
    // every bounds-based editor operation still frame the whole placement.
    if (const UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld()))
    {
        const FBox Pooled = Pool->GetOwnerBounds(*this);
        if (Pooled.IsValid) Bounds += Pooled;
    }
    return Bounds;
}

void AMHCompositeActor::Destroyed()
{
    if (CompositeRoot != nullptr) CompositeRoot->TransformUpdated.RemoveAll(this);
    CancelPendingPlacement();
    ClearDerivedComponents();
    Super::Destroyed();
}

#if WITH_EDITOR
void AMHCompositeActor::PreEditUndo()
{
    // The pool is derived, nontransactional state. Release it while this actor
    // is still live, before UE restores its record and garbage state.
    ClearDerivedComponents();
    Super::PreEditUndo();
}

void AMHCompositeActor::PostEditUndo()
{
    Super::PostEditUndo();
    RestorePlacementAfterUndo();
}

void AMHCompositeActor::PostEditUndo(TSharedPtr<ITransactionObjectAnnotation> TransactionAnnotation)
{
    // AActor's annotated overload does not dispatch our no-argument override.
    Super::PostEditUndo(TransactionAnnotation);
    RestorePlacementAfterUndo();
}

void AMHCompositeActor::RestorePlacementAfterUndo()
{
    // The actor is the transaction record; its plan-view is derived. Retire
    // anything the transaction restored, discard cached state, then rebuild
    // the preview through the normal recipe/materialization path.
    ClearDerivedComponents();
    RebuildComposite();
}

void AMHCompositeActor::PostEditImport()
{
    Super::PostEditImport();
    if (!IsTemplate())
    {
        if (bAutoSeed) Seed = GenerateAutoSeed(Seed);
        if (bAutoAppearanceSeed) AppearanceSeed = GenerateAutoSeed(AppearanceSeed);
        bAppearanceSeedStored = true;
    }
    AttachRootTransformHook();
    // Same single-build-point rule as PostDuplicate: paste re-imports the
    // transient tracking arrays as empty, so building before registration
    // orphans an earlier view instead of retiring it.
    if (HasActorRegisteredAllComponents()) RebuildComposite();
    else bNeedsInitialPlacementBuild = true;
}

bool AMHCompositeActor::GetReferencedContentObjects(TArray<UObject*>& Objects) const
{
    Super::GetReferencedContentObjects(Objects);
    // Browse to Asset (Ctrl+B) from a placed composite selects its generated
    // source asset in the Content Browser.
    if (UMHCompositeAsset* Asset = CompositeAsset.LoadSynchronous()) Objects.AddUnique(Asset);
    return true;
}

void AMHCompositeActor::SetIsTemporarilyHiddenInEditor(const bool bIsHidden)
{
    Super::SetIsTemporarilyHiddenInEditor(bIsHidden);
    SyncPoolVisibility();
}

void AMHCompositeActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    SyncPoolVisibility();
    const FName Name = PropertyChangedEvent.GetPropertyName();
    if (Name == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, CompositeAsset)) RebuildComposite();
    else if (Name == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, Seed)) RebuildPlacement(true);
    else if (Name == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, AppearanceSeed))
    {
        bAppearanceSeedStored = true;
        RebuildPlacement(true);
    }
}
#endif
