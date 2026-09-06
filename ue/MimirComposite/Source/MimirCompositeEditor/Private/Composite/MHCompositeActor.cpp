#include "Composite/MHCompositeActor.h"

#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHInstancePool.h"
#include "Composite/MHMaterializeLayout.h"
#include "Performance/MHPerformanceTrace.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHCompositeRuntimeBridge.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/CollisionProfile.h"
#include "Components/BillboardComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "LevelEditor.h"
#include "Logging/MessageLog.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Serialization/Archive.h"
#include "Serialization/ArchiveSavePackageData.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
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
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
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
    if (bPlacementEditMode)
    {
        LastPlacementError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel Edit before changing Seed");
        return;
    }
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
    if (bPlacementEditMode)
    {
        LastPlacementError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel Edit before changing Appearance Seed");
        return;
    }
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
    if (!bPlanAvailable || !ResidentPlan.IsValid() ||
        ResidentPlan->Seed != Seed ||
        ResidentPlan->Appearance.AppearanceSeed != AppearanceSeed ||
        !LastPlacementError.IsEmpty()) return nullptr;
    // R2b-2: the preview plan is resident; nothing is re-resolved on read.
    return ResidentPlan.Get();
}

void AMHCompositeActor::SetCompositeAsset(UMHCompositeAsset* Asset)
{
    Modify();
    SetPlacementEditMode(false);
    if (CompositeAsset.Get() != Asset)
    {
        AppliedGraph.Reset();
        ObservedRecipeGraph.Reset();
        ResidentPlan.Reset();
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
    SelectedPlacementLeafPath = Row->NodePath;
    return true;
}

bool AMHCompositeActor::SelectPlacementLeafByNodePath(const FString& NodePath)
{
    const bool bExists = LeafMaterializations.ContainsByPredicate(
        [&NodePath](const UE::MimirComposite::FMHCompositeLeafMaterialization& Row)
        {
            return Row.NodePath == NodePath;
        });
    if (!bExists) return false;
    SelectedPlacementLeafPath = NodePath;
    return true;
}

void AMHCompositeActor::SetEditScope(const FString& InvocationNodePath)
{
    EditScopeInvocationPath = InvocationNodePath;
    EditScopeComposite.Reset();
    EditScopeParentLocal = FMatrix::Identity;
    if (InvocationNodePath.IsEmpty() || !ResidentPlan.IsValid()) return;
    const UE::MimirComposite::FMHResolvedCompositeNode* Invocation = ResidentPlan->Nodes.FindByPredicate(
        [&InvocationNodePath](const UE::MimirComposite::FMHResolvedCompositeNode& Node) { return Node.NodePath == InvocationNodePath; });
    if (Invocation == nullptr || Invocation->SemanticKind != UE::MimirComposite::EMHRandomSemanticKind::Composite) return;
    EditScopeComposite = Invocation->Resource;
    EditScopeParentLocal = Invocation->WorldMatrix;
}

USceneComponent* AMHCompositeActor::FindSessionHandleForNodePath(const FString& NodePath) const
{
    // R6-UX1: the handle that moves the node at NodePath — the scope handle of
    // its top-level ancestor inside the edited definition, or the top-level
    // placement handle in a root session.
    if (!bPlacementEditMode || NodePath.IsEmpty()) return nullptr;
    if (!EditScopeInvocationPath.IsEmpty())
    {
        for (int32 Index = 0; Index < EditScopeHandlePaths.Num() && Index < EditScopeHandles.Num(); ++Index)
        {
            const FString& Path = EditScopeHandlePaths[Index];
            if (NodePath == Path || NodePath.StartsWith(Path + TEXT("/")) || NodePath.StartsWith(Path + TEXT(">")))
            {
                return IsValid(EditScopeHandles[Index]) ? EditScopeHandles[Index].Get() : nullptr;
            }
        }
        return nullptr;
    }
    const UMHCompositeAsset* Asset = GetCompositeAsset();
    if (Asset == nullptr) return nullptr;
    // Root session: "<root prefix>:nodes[i]..." -> top-level handle i; the
    // prefix is the call-context namespace when the placement carries one.
    const FString Prefix = (CallContext.StreamNamespace.IsEmpty() ? Asset->LogicalName : CallContext.StreamNamespace) + TEXT(":nodes[");
    if (!NodePath.StartsWith(Prefix)) return nullptr;
    int32 Close = INDEX_NONE;
    if (!NodePath.FindChar(TEXT(']'), Close) || Close <= Prefix.Len()) return nullptr;
    const int32 Index = FCString::Atoi(*NodePath.Mid(Prefix.Len(), Close - Prefix.Len()));
    return TopLevelPlacementComponents.IsValidIndex(Index) && IsValid(TopLevelPlacementComponents[Index])
        ? TopLevelPlacementComponents[Index].Get() : nullptr;
}

FBox AMHCompositeActor::GetEditScopeBounds() const
{
    FBox Bounds(ForceInit);
    if (EditScopeInvocationPath.IsEmpty() || EditScopeComposite.IsEmpty()) return Bounds;
    for (const TObjectPtr<USceneComponent>& Handle : EditScopeHandles)
    {
        if (IsValid(Handle)) Bounds += Handle->GetComponentLocation();
    }
    const FString Prefix = EditScopeInvocationPath + TEXT(">") + EditScopeComposite + TEXT(":");
    for (const UE::MimirComposite::FMHCompositeLeafMaterialization& Row : GetLeafMaterializations())
    {
        if (!Row.NodePath.StartsWith(Prefix)) continue;
        const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Row.Component.Get());
        if (!IsValid(Primitive)) continue;
        if (Row.InstanceIndex != INDEX_NONE)
        {
            const UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Primitive);
            FTransform InstanceWorld;
            if (Bucket == nullptr || Bucket->GetStaticMesh() == nullptr ||
                !Bucket->GetInstanceTransform(Row.InstanceIndex, InstanceWorld, true)) continue;
            Bounds += Bucket->GetStaticMesh()->GetBoundingBox().TransformBy(InstanceWorld);
        }
        else
        {
            Bounds += Primitive->Bounds.GetBox();
        }
    }
    return Bounds;
}

void AMHCompositeActor::SyncEditScopeFrame()
{
    // R6-UX1: a wireframe box marks the edited subtree in the scene; it is
    // never grabbable and never rendered in game.
    const FBox Bounds = GetEditScopeBounds();
    if (!Bounds.IsValid)
    {
        if (IsValid(EditScopeFrame)) EditScopeFrame->DestroyComponent();
        EditScopeFrame = nullptr;
        return;
    }
    if (!IsValid(EditScopeFrame))
    {
        UBoxComponent* Frame = NewObject<UBoxComponent>(this, UBoxComponent::StaticClass(),
            MakeUniqueObjectName(this, UBoxComponent::StaticClass(), TEXT("MH_ScopeFrame")),
            RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);
        AddInstanceComponent(Frame);
        Frame->ComponentTags.Add(TEXT("MHScope.Frame"));
        Frame->SetupAttachment(GetRootComponent());
        Frame->SetAbsolute(true, true, true);
        Frame->bSelectable = false;
        Frame->bDrawOnlyIfSelected = false;
        Frame->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Frame->SetCanEverAffectNavigation(false);
        Frame->SetHiddenInGame(true);
        Frame->ShapeColor = FColor(255, 170, 40);
        Frame->SetLineThickness(3.0f);
        Frame->RegisterComponent();
        EditScopeFrame = Frame;
    }
    EditScopeFrame->SetWorldTransform(FTransform(Bounds.GetCenter()), false, nullptr, ETeleportType::TeleportPhysics);
    EditScopeFrame->SetBoxExtent(Bounds.GetExtent(), false);
}

void AMHCompositeActor::DestroyEditScopeHandles()
{
    for (TObjectPtr<USceneComponent>& Handle : EditScopeHandles)
    {
        if (IsValid(Handle)) Handle->DestroyComponent();
    }
    EditScopeHandles.Reset();
    EditScopeHandlePaths.Reset();
    if (IsValid(EditScopeFrame)) EditScopeFrame->DestroyComponent();
    EditScopeFrame = nullptr;
}

void AMHCompositeActor::SyncEditScopeHandles()
{
    using namespace UE::MimirComposite;
    if (EditScopeInvocationPath.IsEmpty() || !ResidentPlan.IsValid())
    {
        DestroyEditScopeHandles();
        return;
    }
    // The scoped definition's nodes are the resident-plan nodes whose parent
    // is the invocation (16 §2.10); their world under the placement basis is
    // where the artist grabs them.
    const int32 InvocationIndex = ResidentPlan->Nodes.IndexOfByPredicate(
        [this](const FMHResolvedCompositeNode& Node) { return Node.NodePath == EditScopeInvocationPath; });
    TArray<const FMHResolvedCompositeNode*> ScopeNodes;
    if (InvocationIndex != INDEX_NONE)
    {
        EditScopeParentLocal = ResidentPlan->Nodes[InvocationIndex].WorldMatrix;
        for (const FMHResolvedCompositeNode& Node : ResidentPlan->Nodes)
        {
            if (Node.ParentResolvedNodeIndex == InvocationIndex) ScopeNodes.Add(&Node);
        }
    }
    const FMatrix Basis = GetActorTransform().ToMatrixWithScale();
    while (EditScopeHandles.Num() > ScopeNodes.Num())
    {
        if (IsValid(EditScopeHandles.Last())) EditScopeHandles.Last()->DestroyComponent();
        EditScopeHandles.Pop();
    }
    for (int32 Index = 0; Index < ScopeNodes.Num(); ++Index)
    {
        USceneComponent* Handle = EditScopeHandles.IsValidIndex(Index) ? EditScopeHandles[Index].Get() : nullptr;
        if (!IsValid(Handle))
        {
            // Not an "MH." tag: the placement compiler's retirement never
            // touches scope handles; the session owns their lifetime. A sprite
            // (R6-UX1): clickable in the viewport, so the gizmo can grab the
            // node without the Outliner.
            UBillboardComponent* Sprite = NewObject<UBillboardComponent>(this, UBillboardComponent::StaticClass(),
                MakeUniqueObjectName(this, UBillboardComponent::StaticClass(), TEXT("MH_ScopeNode")),
                RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);
            AddInstanceComponent(Sprite);
            Sprite->ComponentTags.Add(FName(*FString::Printf(TEXT("MHScope.Handle:%d"), Index)));
            Sprite->SetupAttachment(GetRootComponent());
            Sprite->SetAbsolute(true, true, true);
            Sprite->SetHiddenInGame(true);
            Sprite->bIsScreenSizeScaled = true;
            if (UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EditorResources/S_TargetPoint.S_TargetPoint")))
            {
                Sprite->SetSprite(Texture);
            }
            Sprite->RegisterComponent();
            Handle = Sprite;
            if (EditScopeHandles.IsValidIndex(Index)) EditScopeHandles[Index] = Handle;
            else EditScopeHandles.Add(Handle);
        }
        Handle->SetWorldTransform(FTransform(ScopeNodes[Index]->WorldMatrix * Basis), false, nullptr, ETeleportType::TeleportPhysics);
    }
    EditScopeHandlePaths.Reset();
    for (const FMHResolvedCompositeNode* Node : ScopeNodes) EditScopeHandlePaths.Add(Node->NodePath);
    SyncEditScopeFrame();
}

void AMHCompositeActor::SetPlacementEditMode(const bool bEnabled)
{
    if (bPlacementEditMode == bEnabled) return;
    TArray<FTransform> PendingHandleEdits;
    if (bEnabled)
    {
        PendingHandleEdits.Reserve(TopLevelPlacementComponents.Num());
        for (const USceneComponent* Handle : TopLevelPlacementComponents)
            PendingHandleEdits.Add(IsValid(Handle)
                ? Handle->GetComponentTransform() : FTransform::Identity);
    }
    // Reconcile the materialization while the regular rebuild gate is open.
    // On entry only the selected static leaf is extracted; on exit it is
    // admitted back into its immutable-policy bucket.
    bPlacementEditMode = false;
    bExtractSelectedLeafForEdit = bEnabled && !SelectedPlacementLeafPath.IsEmpty();
    if (HasActorRegisteredAllComponents() && ResidentPlan.IsValid()) RebuildPlacement(false);
    // Beginning an edit session must not erase a handle transform the user
    // changed immediately before invoking Edit. The subsequent editor tick
    // turns these restored world transforms into the prospective signed plan.
    if (bEnabled && PendingHandleEdits.Num() == TopLevelPlacementComponents.Num())
    {
        for (int32 Index = 0; Index < PendingHandleEdits.Num(); ++Index)
            if (IsValid(TopLevelPlacementComponents[Index]))
                TopLevelPlacementComponents[Index]->SetWorldTransform(
                    PendingHandleEdits[Index], false, nullptr, ETeleportType::TeleportPhysics);
    }
    bPlacementEditMode = bEnabled;
    EditingGraph.Reset();
    EditingDocument.Reset();
    LastEditHandleTransforms.Reset();
    LastEditBasis = GetActorTransform();
    if (bEnabled && AppliedGraph.IsValid())
    {
        EditingGraph = *AppliedGraph;
        // R6-D1: under a nested scope the session edits the invoked definition's
        // document; its nodes get handles under the invocation's world.
        const UMHCompositeAsset* EditedAsset = GetCompositeAsset();
        if (!EditScopeInvocationPath.IsEmpty())
        {
            UE::MimirComposite::FMHResourceKey ChildKey;
            ChildKey.Kind = EMHResourceKind::Composite;
            ChildKey.LogicalName = EditScopeComposite;
            FString AdmissionError;
            EditedAsset = Cast<UMHCompositeAsset>(UMHEndpointPrototypeRegistry::ResolveEndpoint(ChildKey, AdmissionError));
        }
        if (EditedAsset != nullptr)
        {
            UE::MimirComposite::FMHCompositeDocument Document;
            FString Error;
            if (UE::MimirComposite::MHExtractCompositeV5(*EditedAsset, Document, Error)) EditingDocument = MoveTemp(Document);
        }
        SyncEditScopeHandles();
        const TArray<TObjectPtr<USceneComponent>>& SessionHandles = EditScopeInvocationPath.IsEmpty() ? TopLevelPlacementComponents : EditScopeHandles;
        for (const USceneComponent* Handle : SessionHandles)
            LastEditHandleTransforms.Add(Handle != nullptr ? Handle->GetComponentTransform() : FTransform::Identity);
    }
    if (!bEnabled)
    {
        DestroyEditScopeHandles();
        EditScopeInvocationPath.Reset();
        EditScopeComposite.Reset();
        EditScopeParentLocal = FMatrix::Identity;
    }
    SetActorTickEnabled(bEnabled);
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

bool AMHCompositeActor::GetEditedCompositeDocument(UE::MimirComposite::FMHCompositeDocument& OutDocument) const
{
    if (!bPlacementEditMode || !EditingDocument.IsSet() ||
        !bPlanAvailable || !ResidentPlan.IsValid()) return false;
    const TArray<TObjectPtr<USceneComponent>>& SessionHandles = EditScopeInvocationPath.IsEmpty() ? TopLevelPlacementComponents : EditScopeHandles;
    if (SessionHandles.Num() != EditingDocument->Nodes.Num() ||
        SessionHandles.ContainsByPredicate([](const USceneComponent* Handle) { return !IsValid(Handle); })) return false;
    OutDocument = EditingDocument.GetValue();
    return true;
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
    // Pooled leaves are released with the owner (16 §2.8); Undo rebuilds them
    // from the actor's record afterwards (OPEN-R-1).
    if (UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(GetWorld())) Pool->RemoveOwner(*this);
    DestroyEditScopeHandles();
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
    if (!Delta.Any() || bRebuildInProgress || bPlacementEditMode || IsTemplate() || IsActorBeingDestroyed() ||
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
    if (Key.Kind != EMHResourceKind::Composite || bRebuildInProgress || bPlacementEditMode ||
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
    if (bRebuildInProgress || bPlacementEditMode || IsTemplate() || IsActorBeingDestroyed()) return;
    TGuardValue<bool> Guard(bRebuildInProgress, true);
    ++PlacementRebuildCount;
    // Instrumentation for the S6.2 lifecycle guard: placement components may
    // only be created once this actor's own components are already registered.
    if (!HasActorRegisteredAllComponents()) ++PlacementUnregisteredBuildCount;
    bBasisRejected = false;
    AttachRootTransformHook();
    if (CompositeAsset.ToSoftObjectPath().IsNull())
    {
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
                    *this, *ResidentPlan, *PreviousRoot, *Settings, Previous,
                    bExtractSelectedLeafForEdit ? SelectedPlacementLeafPath : FString());
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
    const FMHRandomComposite* Root = CandidateGraph->Composites.Find(Name);
    if (Root == nullptr) return;
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
            *this, *CandidatePlan, *Root, *Settings, Previous,
            bExtractSelectedLeafForEdit ? SelectedPlacementLeafPath : FString());
        if (!View.Succeeded())
        {
            LastPlacementError = View.Error;
            bPlanAvailable = false;
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
                LeafMaterializations, View,
                bExtractSelectedLeafForEdit ? SelectedPlacementLeafPath : FString()))
        {
            if (!View.Succeeded())
            {
                LastPlacementError = View.Error;
                bPlanAvailable = false;
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
    ++PreviewRevision;
    bPlanAvailable = true;
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
    if (bPlacementEditMode)
    {
        Tick(0.0f);
        return;
    }
    if (!ResidentPlan.IsValid() || !AppliedGraph.IsValid() ||
        ResidentPlan->Seed != Seed ||
        ResidentPlan->Appearance.AppearanceSeed != AppearanceSeed) return;
    if (!bPlanAvailable && !bBasisRejected)
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
        else if (bBasisRejected)
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
    ClearDerivedComponents();
    Super::Destroyed();
}

void AMHCompositeActor::Tick(const float DeltaSeconds)
{
    using namespace UE::MimirComposite;
    Super::Tick(DeltaSeconds);
    if (!bPlacementEditMode || bRebuildInProgress || !EditingGraph.IsSet() || !EditingDocument.IsSet()) return;
    FMHRandomComposite* Root = EditingGraph->Composites.Find(EditingGraph->RootComposite);
    // R6-D1: under a nested scope the edited definition is the invoked child
    // and the handles are the scope handles; the local space of an edited node
    // is the invocation's effective world, not the placement basis.
    const bool bScoped = !EditScopeInvocationPath.IsEmpty();
    FMHRandomComposite* Edited = bScoped ? EditingGraph->Composites.Find(EditScopeComposite) : Root;
    const TArray<TObjectPtr<USceneComponent>>& SessionHandles = bScoped ? EditScopeHandles : TopLevelPlacementComponents;
    if (Root == nullptr || Edited == nullptr || Edited->Nodes.Num() != SessionHandles.Num() ||
        SessionHandles.ContainsByPredicate([](const USceneComponent* Handle) { return !IsValid(Handle); }))
    {
        LastPlacementError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: an authored Edit handle disappeared");
        bPlanAvailable = false;
        return;
    }
    const FTransform CurrentBasis = GetActorTransform();
    bool bChanged = !CurrentBasis.Equals(LastEditBasis, 0.0);
    bool bAuthoredChanged = false;
    const FMatrix ParentInverse = bScoped
        ? (EditScopeParentLocal * LastEditBasis.ToMatrixWithScale()).Inverse()
        : LastEditBasis.ToInverseMatrixWithScale();
    for (int32 Index = 0; Index < SessionHandles.Num(); ++Index)
    {
        USceneComponent* Handle = SessionHandles[Index];
        const FTransform Current = Handle->GetComponentTransform();
        if (LastEditHandleTransforms.IsValidIndex(Index) && Current.Equals(LastEditHandleTransforms[Index], 0.0)) continue;
        // Absolute handles still live in the previously admitted basis when
        // the actor moves. A placement move must not become a source edit.
        const FMatrix LocalMatrix = Current.ToMatrixWithScale() * ParentInverse;
        if (!MHIsRepresentableTransformMatrix(LocalMatrix))
        {
            LastPlacementError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: edited handle");
            bPlanAvailable = false;
            return;
        }
        const FTransform Local(LocalMatrix);
        Edited->Nodes[Index].Transform.TranslationCm = FVector3f(Local.GetTranslation());
        Edited->Nodes[Index].Transform.RotationQuat = FQuat4f(Local.GetRotation());
        Edited->Nodes[Index].Transform.Scale = FVector3f(Local.GetScale3D());
        EditingDocument->Nodes[Index].Transform.TranslationCm = Local.GetTranslation();
        EditingDocument->Nodes[Index].Transform.RotationQuat = Local.GetRotation();
        EditingDocument->Nodes[Index].Transform.Scale = Local.GetScale3D();
        bAuthoredChanged = true;
        bChanged = true;
    }
    if (!bChanged) return;
    TGuardValue<bool> Guard(bRebuildInProgress, true);
    TSharedRef<FMHResolvedCompositePlan> Plan = MakeShared<FMHResolvedCompositePlan>();
    FString Error;
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (bAuthoredChanged)
    {
        TArray<uint8> ProspectiveBytes;
        if (!MHWriteCanonicalCompositeV5(*EditingDocument, ProspectiveBytes, Error))
        {
            LastPlacementError = Error;
            bPlanAvailable = false;
            return;
        }
    }
    // Preview plane: the prospective source resolves through Layout +
    // Appearance only; proof (closure, signatures) is never built here.
    if (!MHResolvePreviewGraph(*EditingGraph, Seed, AppearanceSeed, CallContext.ToResolveContext(), *Plan, Error) ||
        !MHValidateResolvedPlacementTransforms(*Plan, GetActorTransform(), Error))
    {
        LastPlacementError = Error;
        bPlanAvailable = false;
        return;
    }
    FMHCompositePlacementCompileResult View = MHCompileCompositePlacementV5(
        *this, *Plan, *Root, *Settings, DerivedComponents,
        bExtractSelectedLeafForEdit ? SelectedPlacementLeafPath : FString());
    if (!View.Succeeded())
    {
        LastPlacementError = View.Error;
        bPlanAvailable = false;
        return;
    }
    const TArray<TObjectPtr<UActorComponent>> Previous = MoveTemp(DerivedComponents);
    DerivedComponents = MoveTemp(View.Components);
    TopLevelPlacementComponents = MoveTemp(View.TopLevelComponents);
    LeafPlacementComponents = MoveTemp(View.LeafComponents);
    LeafMaterializations = MoveTemp(View.LeafMaterializations);
    DestroyMHRetiredComponents(Previous, DerivedComponents);
    ResidentPlan = Plan;
    SyncEditScopeHandles();
    LastEditHandleTransforms.Reset();
    const TArray<TObjectPtr<USceneComponent>>& NextHandles = bScoped ? EditScopeHandles : TopLevelPlacementComponents;
    for (const USceneComponent* Handle : NextHandles) LastEditHandleTransforms.Add(IsValid(Handle) ? Handle->GetComponentTransform() : FTransform::Identity);
    LastEditBasis = CurrentBasis;
    ++PreviewRevision;
    LastPlacementError.Reset();
    bPlanAvailable = true;
    SyncPoolVisibility();
}

#if WITH_EDITOR
void AMHCompositeActor::PostEditUndo()
{
    Super::PostEditUndo();
    // R5-F: a live Placement Edit session would block its own restore (the
    // rebuild gate is closed while editing) and then tick against an empty
    // view. The session is transient state, not part of the record: end it.
    if (bPlacementEditMode)
    {
        bPlacementEditMode = false;
        bExtractSelectedLeafForEdit = false;
        EditingGraph.Reset();
        EditingDocument.Reset();
        LastEditHandleTransforms.Reset();
        SetActorTickEnabled(false);
    }
    DestroyEditScopeHandles();
    EditScopeInvocationPath.Reset();
    EditScopeComposite.Reset();
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

bool AMHCompositeActor::CanEditChange(const FProperty* Property) const
{
    if (bPlacementEditMode && Property != nullptr &&
        (Property->GetFName() == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, Seed) ||
         Property->GetFName() == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, bAutoSeed) ||
         Property->GetFName() == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, AppearanceSeed) ||
         Property->GetFName() == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, bAutoAppearanceSeed))) return false;
    return Super::CanEditChange(Property);
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
