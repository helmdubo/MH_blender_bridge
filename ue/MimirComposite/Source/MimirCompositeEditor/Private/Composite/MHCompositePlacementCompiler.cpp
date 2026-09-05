#include "Composite/MHCompositePlacementCompiler.h"

#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHInstancePool.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Components/BoxComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Performance/MHPerformanceTrace.h"
#include "StaticMeshCompiler.h"
#include "Settings/MHCompositeSettings.h"
#include "Templates/Greater.h"

namespace UE::MimirComposite
{
namespace
{
constexpr EObjectFlags PlanViewFlags = RF_Transient | RF_DuplicateTransient | RF_TextExportTransient;

/** Instrumentation counter behind MHGetPlacementPreviousComponentProbes. */
uint64 GMHPlacementPreviousComponentProbes = 0;

/** The level instance pool that materializes static leaves (16 §2.8); nullptr outside editor worlds. */
UMHInstancePoolSubsystem* PlanViewPool(const AActor& Target)
{
    return UMHInstancePoolSubsystem::Get(Target.GetWorld());
}

FMHPoolBucketDescriptor PlanViewPoolDescriptor(UStaticMesh& Mesh, const UMHCompositeSettings& Settings)
{
    return FMHPoolBucketDescriptor::FromMesh(Mesh,
        Settings.AppearanceCustomDataBaseIndex + MH_APPEARANCE_CHANNELS, Settings.AppearanceCustomDataBaseIndex);
}

/** Materialization row of a pooled leaf: the handle plus its current ISM address. */
bool PlanViewPooledRow(const UMHInstancePoolSubsystem& Pool, const FMHInstanceHandle& Handle,
    const FMHResolvedCompositeLeaf& Leaf, FMHCompositeLeafMaterialization& OutRow)
{
    UInstancedStaticMeshComponent* Component = nullptr;
    int32 InstanceIndex = INDEX_NONE;
    if (!Pool.GetInstance(Handle, Component, InstanceIndex) || Component == nullptr) return false;
    OutRow.Component = Component;
    OutRow.InstanceIndex = InstanceIndex;
    OutRow.ResolvedNodeIndex = Leaf.OwningResolvedNodeIndex;
    OutRow.NodePath = Leaf.Origin;
    OutRow.Handle = Handle;
    return true;
}

FMatrix PlanViewTrsMatrix(const FMHRandomTrs& Trs)
{
    return FTransform(FQuat(Trs.RotationQuat), FVector(Trs.TranslationCm), FVector(Trs.Scale)).ToMatrixWithScale();
}

void PlanViewSetWorld(USceneComponent& Component, const FMatrix& Matrix)
{
    const FTransform Transform(Matrix);
    if (!Component.GetComponentTransform().Equals(Transform, 0.0))
    {
        MHRecordPlacementWorldTransformUpdate();
        Component.SetWorldTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
    }
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
    MHRecordPlacementComponentCreated();
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
        MHRecordPlacementComponentRegistered();
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
    TConstArrayView<TObjectPtr<USceneComponent>> Leaves,
    TConstArrayView<FMHCompositeLeafMaterialization> Materializations, FString& OutError)
{
    // Fail-closed before any mutation: an intersection-length walk leaves the
    // surplus silently stale and still reports success. Require exact agreement
    // and let the caller rebuild the whole placement instead.
    if (Handles.Num() != RootDefinition.Nodes.Num() || Leaves.Num() != Plan.Leaves.Num() ||
        Materializations.Num() != Plan.Leaves.Num())
    {
        OutError = FString::Printf(
            TEXT("MH_E_PLACEMENT_STATE_DESYNC: %s handles %d of %d, leaves %d of %d"),
            *RootDefinition.Name, Handles.Num(), RootDefinition.Nodes.Num(), Leaves.Num(), Plan.Leaves.Num());
        return false;
    }
    if (!PlanViewPreflight(Plan, RootDefinition, Target.GetActorTransform(), OutError)) return false;
    const FMatrix Basis = Target.GetActorTransform().ToMatrixWithScale();
    UMHInstancePoolSubsystem* Pool = PlanViewPool(Target);
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        if (!IsValid(Handles[Index]) || Handles[Index]->GetOwner() != &Target)
        {
            OutError = TEXT("MH_E_PLACEMENT_STATE_DESYNC: invalid placement handle");
            return false;
        }
    }
    for (int32 Index = 0; Index < Leaves.Num(); ++Index)
    {
        const FMHCompositeLeafMaterialization& Row = Materializations[Index];
        if (Row.Component != Leaves[Index] || Row.NodePath != Plan.Leaves[Index].Origin ||
            Row.ResolvedNodeIndex != Plan.Leaves[Index].OwningResolvedNodeIndex || !IsValid(Row.Component))
        {
            OutError = TEXT("MH_E_PLACEMENT_STATE_DESYNC: leaf materialization mapping mismatch");
            return false;
        }
        if (!Row.IsInstanced())
        {
            if (Row.Component->GetOwner() != &Target)
            {
                OutError = TEXT("MH_E_PLACEMENT_STATE_DESYNC: leaf materialization mapping mismatch");
                return false;
            }
            continue;
        }
        // Pooled leaf (16 §2.8): the handle, not the ISM index, is the identity.
        if (Pool == nullptr || !Row.Handle.IsSet() || !Pool->IsValidHandle(Row.Handle))
        {
            OutError = TEXT("MH_E_PLACEMENT_STATE_DESYNC: invalid pooled leaf instance");
            return false;
        }
    }
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        PlanViewSetWorld(*Handles[Index],
            PlanViewTrsMatrix(RootDefinition.Nodes[Index].Transform) * Basis);
    }
    if (Pool != nullptr) Pool->BeginBulk();
    for (int32 Index = 0; Index < Leaves.Num(); ++Index)
    {
        const FMHCompositeLeafMaterialization& Row = Materializations[Index];
        if (Row.IsInstanced())
        {
            MHRecordPlacementWorldTransformUpdate();
            Pool->Update(Row.Handle, Plan.Leaves[Index].WorldMatrix * Basis);
        }
        else
        {
            PlanViewSetWorld(*Leaves[Index], Plan.Leaves[Index].WorldMatrix * Basis);
        }
    }
    if (Pool != nullptr) Pool->EndBulk();
    return true;
}

int32 MHApplyCompositePlacementAppearance(
    TConstArrayView<FMHCompositeLeafMaterialization> Materializations,
    const FMHResolvedCompositePlan& Plan, const int32 BaseIndex)
{
    if (Materializations.Num() != Plan.Leaves.Num() ||
        !MHIsAdmissibleAppearanceCustomDataBaseIndex(BaseIndex)) return INDEX_NONE;
    UMHInstancePoolSubsystem* Pool = nullptr;
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHCompositeLeafMaterialization& Row = Materializations[Index];
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        if (Row.NodePath != Leaf.Origin || Row.ResolvedNodeIndex != Leaf.OwningResolvedNodeIndex ||
            !IsValid(Row.Component)) return INDEX_NONE;
        if (!Row.IsInstanced()) continue;
        if (Pool == nullptr) Pool = UMHInstancePoolSubsystem::Get(Row.Component->GetWorld());
        if (Pool == nullptr || !Row.Handle.IsSet() || !Pool->IsValidHandle(Row.Handle)) return INDEX_NONE;
    }
    int32 Applied = 0;
    if (Pool != nullptr) Pool->BeginBulk();
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHCompositeLeafMaterialization& Row = Materializations[Index];
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        if (Row.IsInstanced())
        {
            if (!Pool->UpdateAppearance(Row.Handle, Leaf.AppearanceChannels))
            {
                Pool->EndBulk();
                return INDEX_NONE;
            }
            ++Applied;
        }
        else if (MHApplyLeafAppearanceCustomData(Row.Component, Leaf, BaseIndex))
        {
            ++Applied;
        }
    }
    if (Pool != nullptr) Pool->EndBulk();
    return Applied;
}

/**
 * R1: the compilation wait is handed only the meshes the seed selected. The
 * closure loads every option for proof but never waits; R4 removes this wait
 * with placeholders and async prototypes.
 */
/**
 * R4 (KICKOFF §5, 16 §2.2): the interactive path never waits for static mesh
 * compilation. A selected mesh that is still compiling is bound as it is; the
 * engine swaps its render data in when the compilation lands. The report keeps
 * naming selected compiling meshes; waited_meshes stays empty by construction.
 */
void PlanViewWaitSelectedMeshes(const TMap<FMHResourceKey, UStaticMesh*>& SelectedMeshes)
{
    static_cast<void>(SelectedMeshes);
}

/** Registry mesh for a leaf: the Ready mesh, or the placeholder while it loads (R4). */
UStaticMesh* PlanViewResolveLeafMesh(const FString& Resource, const UMHCompositeSettings& Settings, FString& OutError)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::StaticMesh;
    Key.LogicalName = Resource;
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (Registry == nullptr)
    {
        OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: endpoint prototype registry unavailable for ") + Key.ToString();
        return nullptr;
    }
    bool bPlaceholder = false;
    return Registry->ResolveMeshForPreview(Key, Settings, bPlaceholder, OutError);
}

bool MHTryCompileCompositePlacementReseedV5(AActor& Target,
    const FMHResolvedCompositePlan& PreviousPlan, const FMHResolvedCompositePlan& CandidatePlan,
    const FMHRandomComposite& RootDefinition, const UMHCompositeSettings& Settings,
    TConstArrayView<TObjectPtr<UActorComponent>> PreviousComponents,
    TConstArrayView<TObjectPtr<USceneComponent>> PreviousHandles,
    TConstArrayView<TObjectPtr<USceneComponent>> PreviousLeaves,
    TConstArrayView<FMHCompositeLeafMaterialization> PreviousMaterializations,
    FMHCompositePlacementCompileResult& OutResult,
    const FString& UninstancedLeafPath)
{
    FMHPlacementStageScope CompileStage(EMHPlacementStage::CompilePlacement);
    OutResult = FMHCompositePlacementCompileResult();
    if (!PlanViewPreflight(CandidatePlan, RootDefinition, Target.GetActorTransform(), OutResult.Error)) return true;

    const bool bHasInstancedLeaves = PreviousMaterializations.ContainsByPredicate(
        [](const FMHCompositeLeafMaterialization& Row) { return Row.IsInstanced(); });
    if (bHasInstancedLeaves)
    {
        // Pooled view (16 §2.8, R5b-1): the diff is applied through stable
        // handles; no ISM index arithmetic and no bucket ownership on this actor.
        UMHInstancePoolSubsystem* Pool = PlanViewPool(Target);
        ULevel* Level = Target.GetLevel();
        if (Pool == nullptr || Level == nullptr) return false;
        if (PreviousHandles.Num() != RootDefinition.Nodes.Num() ||
            PreviousLeaves.Num() != PreviousPlan.Leaves.Num() ||
            PreviousMaterializations.Num() != PreviousPlan.Leaves.Num()) return false;
        TMap<FString, TObjectPtr<UStaticMesh>> MeshesByResource;
        for (int32 Index = 0; Index < PreviousPlan.Leaves.Num(); ++Index)
        {
            const FMHResolvedCompositeLeaf& Leaf = PreviousPlan.Leaves[Index];
            const FMHCompositeLeafMaterialization& Row = PreviousMaterializations[Index];
            if (Row.Component != PreviousLeaves[Index] || Row.NodePath != Leaf.Origin ||
                Row.ResolvedNodeIndex != Leaf.OwningResolvedNodeIndex || !IsValid(Row.Component)) return false;
            if (!Row.IsInstanced())
            {
                if (Row.Component->GetOwner() != &Target || !PreviousComponents.Contains(Row.Component)) return false;
                continue;
            }
            UInstancedStaticMeshComponent* Bucket = nullptr;
            int32 InstanceIndex = INDEX_NONE;
            if (Leaf.Kind != EMHRandomSemanticKind::Mesh || !Row.Handle.IsSet() ||
                !Pool->GetInstance(Row.Handle, Bucket, InstanceIndex) || Bucket == nullptr) return false;
            TObjectPtr<UStaticMesh>& ExpectedMesh = MeshesByResource.FindOrAdd(Leaf.Resource);
            if (ExpectedMesh == nullptr)
            {
                ExpectedMesh = PlanViewResolveLeafMesh(Leaf.Resource, Settings, OutResult.Error);
                if (!OutResult.Error.IsEmpty()) return true;
            }
            // The endpoint behind a kept handle must still be the mesh its
            // bucket renders; anything else is the full compiler's job.
            if (ExpectedMesh == nullptr || Bucket->GetStaticMesh() != ExpectedMesh) return false;
        }
        if (!UninstancedLeafPath.IsEmpty()) return false;
        for (int32 Index = 0; Index < PreviousHandles.Num(); ++Index)
        {
            USceneComponent* Handle = PreviousHandles[Index];
            const FName ExpectedTag(*FString::Printf(TEXT("MH.Handle:%d"), Index));
            if (!IsValid(Handle) || Handle->GetOwner() != &Target ||
                Handle->GetClass() != USceneComponent::StaticClass() || !Handle->IsRegistered() ||
                !Handle->ComponentTags.Contains(ExpectedTag) ||
                Handle->GetAttachParent() != Target.GetRootComponent() ||
                !Handle->IsUsingAbsoluteLocation() || !Handle->IsUsingAbsoluteRotation() ||
                !Handle->IsUsingAbsoluteScale()) return false;
        }

        TArray<int32> PreviousIndexForCandidate;
        PreviousIndexForCandidate.Init(INDEX_NONE, CandidatePlan.Leaves.Num());
        TBitArray<> PreviousKept(false, PreviousPlan.Leaves.Num());
        int32 PositionalPathMismatches = 0;
        if (PreviousPlan.Leaves.Num() == CandidatePlan.Leaves.Num())
        {
            for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
                if (PreviousPlan.Leaves[Index].Origin != CandidatePlan.Leaves[Index].Origin)
                    ++PositionalPathMismatches;
        }
        const bool bUsePositionalDiff =
            PreviousPlan.Leaves.Num() == CandidatePlan.Leaves.Num() &&
            PositionalPathMismatches <= 1;
        TMap<FString, int32> PreviousByPath;
        if (!bUsePositionalDiff)
        {
            for (int32 Index = 0; Index < PreviousPlan.Leaves.Num(); ++Index)
            {
                if (PreviousByPath.Contains(PreviousPlan.Leaves[Index].Origin)) return false;
                PreviousByPath.Add(PreviousPlan.Leaves[Index].Origin, Index);
            }
        }
        TSet<FString> CandidatePaths;
        for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
        {
            const FMHResolvedCompositeLeaf& Candidate = CandidatePlan.Leaves[Index];
            int32 PreviousIndex = bUsePositionalDiff ? Index : INDEX_NONE;
            if (!bUsePositionalDiff)
            {
                if (CandidatePaths.Contains(Candidate.Origin)) return false;
                CandidatePaths.Add(Candidate.Origin);
                if (const int32* Found = PreviousByPath.Find(Candidate.Origin))
                    PreviousIndex = *Found;
            }
            if (!PreviousPlan.Leaves.IsValidIndex(PreviousIndex)) continue;
            const FMHResolvedCompositeLeaf& PreviousLeaf = PreviousPlan.Leaves[PreviousIndex];
            if (PreviousLeaf.Origin != Candidate.Origin ||
                PreviousLeaf.Kind != Candidate.Kind ||
                PreviousLeaf.Resource != Candidate.Resource) continue;
            if (PreviousLeaf.RootNodeIndex != Candidate.RootNodeIndex) return false;
            PreviousIndexForCandidate[Index] = PreviousIndex;
            PreviousKept[PreviousIndex] = true;
        }

        const auto ResolveMesh = [&](const FString& Resource) -> UStaticMesh*
        {
            TObjectPtr<UStaticMesh>& Cached = MeshesByResource.FindOrAdd(Resource);
            if (Cached != nullptr) return Cached;
            Cached = PlanViewResolveLeafMesh(Resource, Settings, OutResult.Error);
            return Cached;
        };
        for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
        {
            if (PreviousIndexForCandidate[Index] != INDEX_NONE) continue;
            if (CandidatePlan.Leaves[Index].Kind != EMHRandomSemanticKind::Mesh) return false;
            if (ResolveMesh(CandidatePlan.Leaves[Index].Resource) == nullptr)
            {
                if (OutResult.Error.IsEmpty()) return false;
                return true;
            }
        }
        for (int32 Index = 0; Index < PreviousPlan.Leaves.Num(); ++Index)
        {
            if (!PreviousKept[Index] && !PreviousMaterializations[Index].IsInstanced()) return false;
        }

        const FMatrix Basis = Target.GetActorTransform().ToMatrixWithScale();
        Pool->BeginBulk();
        for (int32 Index = 0; Index < PreviousPlan.Leaves.Num(); ++Index)
        {
            if (!PreviousKept[Index]) Pool->Remove(PreviousMaterializations[Index].Handle);
        }
        OutResult.TopLevelComponents.Append(PreviousHandles);
        OutResult.LeafComponents.SetNum(CandidatePlan.Leaves.Num());
        OutResult.LeafMaterializations.SetNum(CandidatePlan.Leaves.Num());
        for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
        {
            const int32 PreviousIndex = PreviousIndexForCandidate[Index];
            if (PreviousIndex == INDEX_NONE) continue;
            const FMHResolvedCompositeLeaf& PreviousLeaf = PreviousPlan.Leaves[PreviousIndex];
            const FMHResolvedCompositeLeaf& CandidateLeaf = CandidatePlan.Leaves[Index];
            FMHCompositeLeafMaterialization Row = PreviousMaterializations[PreviousIndex];
            if (Row.IsInstanced())
            {
                if (FMemory::Memcmp(&PreviousLeaf.WorldMatrix, &CandidateLeaf.WorldMatrix,
                        sizeof(FMatrix)) != 0)
                {
                    MHRecordPlacementWorldTransformUpdate();
                    Pool->Update(Row.Handle, CandidateLeaf.WorldMatrix * Basis);
                }
                if (FMemory::Memcmp(PreviousLeaf.AppearanceChannels,
                        CandidateLeaf.AppearanceChannels,
                        sizeof(CandidateLeaf.AppearanceChannels)) != 0)
                {
                    MHRecordPlacementAppearanceUpdate();
                    Pool->UpdateAppearance(Row.Handle, CandidateLeaf.AppearanceChannels);
                }
            }
            else
            {
                PlanViewSetWorld(*Row.Component, CandidateLeaf.WorldMatrix * Basis);
            }
            Row.NodePath = CandidateLeaf.Origin;
            Row.ResolvedNodeIndex = CandidateLeaf.OwningResolvedNodeIndex;
            OutResult.LeafComponents[Index] = Row.Component;
            OutResult.LeafMaterializations[Index] = MoveTemp(Row);
        }

        TMap<UStaticMesh*, FMHPoolBucketDescriptor> Descriptors;
        for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
        {
            if (PreviousIndexForCandidate[Index] != INDEX_NONE) continue;
            const FMHResolvedCompositeLeaf& Leaf = CandidatePlan.Leaves[Index];
            UStaticMesh* Mesh = ResolveMesh(Leaf.Resource);
            if (Mesh == nullptr)
            {
                Pool->EndBulk();
                return true;
            }
            FMHPoolBucketDescriptor* Descriptor = Descriptors.Find(Mesh);
            if (Descriptor == nullptr) Descriptor = &Descriptors.Add(Mesh, PlanViewPoolDescriptor(*Mesh, Settings));
            MHRecordPlacementWorldTransformUpdate();
            MHRecordPlacementAppearanceUpdate();
            const FMHInstanceHandle Handle = Pool->Add(Target, Leaf.Origin, *Level, *Descriptor,
                Leaf.WorldMatrix * Basis, Leaf.AppearanceChannels);
            if (!Handle.IsSet())
            {
                Pool->EndBulk();
                OutResult.Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: instance pool refused ") + Leaf.Origin + TEXT(" -> ") + Leaf.Resource;
                return true;
            }
            OutResult.LeafMaterializations[Index] = {nullptr, INDEX_NONE, Leaf.OwningResolvedNodeIndex, Leaf.Origin, Handle};
        }
        Pool->EndBulk();
        // Current ISM addresses behind the handles, after every removal and addition.
        for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
        {
            FMHCompositeLeafMaterialization& Row = OutResult.LeafMaterializations[Index];
            if (!Row.Handle.IsSet()) continue;
            UInstancedStaticMeshComponent* Bucket = nullptr;
            int32 InstanceIndex = INDEX_NONE;
            Pool->GetInstance(Row.Handle, Bucket, InstanceIndex);
            Row.Component = Bucket;
            Row.InstanceIndex = InstanceIndex;
            OutResult.LeafComponents[Index] = Bucket;
        }

        OutResult.Components.Reset();
        TSet<UActorComponent*> AddedComponents;
        for (USceneComponent* Handle : PreviousHandles)
        {
            OutResult.Components.Add(Handle);
            AddedComponents.Add(Handle);
            const int32 Index = OutResult.TopLevelComponents.IndexOfByKey(Handle);
            if (RootDefinition.Nodes.IsValidIndex(Index))
                PlanViewSetWorld(*Handle,
                    PlanViewTrsMatrix(RootDefinition.Nodes[Index].Transform) * Basis);
        }
        for (const FMHCompositeLeafMaterialization& Row : OutResult.LeafMaterializations)
        {
            if (Row.Handle.IsSet() || !IsValid(Row.Component) || AddedComponents.Contains(Row.Component)) continue;
            OutResult.Components.Add(Row.Component);
            AddedComponents.Add(Row.Component);
        }
        return true;
    }

    // This is an optimization admission boundary. Reject every structural
    // mismatch before endpoint lookup or component mutation; the actor will run
    // the existing full compiler over its orphan-union previous view.
    if (PreviousHandles.Num() != RootDefinition.Nodes.Num() ||
        PreviousLeaves.Num() != PreviousPlan.Leaves.Num() ||
        PreviousComponents.Num() != PreviousHandles.Num() + PreviousLeaves.Num()) return false;

    const auto BuildRootPaths = [&RootDefinition](
        const FMHResolvedCompositePlan& Plan, TArray<FString>& OutPaths) -> bool
    {
        OutPaths.SetNum(RootDefinition.Nodes.Num());
        TBitArray<> Seen(false, RootDefinition.Nodes.Num());
        for (const FMHResolvedCompositeNode& Node : Plan.Nodes)
        {
            if (Node.ParentResolvedNodeIndex != INDEX_NONE) continue;
            if (!Seen.IsValidIndex(Node.RootNodeIndex) || Seen[Node.RootNodeIndex]) return false;
            Seen[Node.RootNodeIndex] = true;
            OutPaths[Node.RootNodeIndex] = Node.NodePath;
        }
        return !Seen.Contains(false);
    };
    TArray<FString> PreviousRootPaths;
    TArray<FString> CandidateRootPaths;
    if (!BuildRootPaths(PreviousPlan, PreviousRootPaths) ||
        !BuildRootPaths(CandidatePlan, CandidateRootPaths) ||
        PreviousRootPaths != CandidateRootPaths) return false;

    for (int32 Index = 0; Index < PreviousHandles.Num(); ++Index)
    {
        USceneComponent* Handle = PreviousHandles[Index];
        const FName ExpectedTag(*FString::Printf(TEXT("MH.Handle:%d"), Index));
        if (!IsValid(Handle) || Handle->GetOwner() != &Target ||
            Handle->GetClass() != USceneComponent::StaticClass() || !Handle->IsRegistered() ||
            Handle->ComponentTags.Num() != 1 || !Handle->ComponentTags.Contains(ExpectedTag) ||
            Handle->GetAttachParent() != Target.GetRootComponent() ||
            !Handle->IsUsingAbsoluteLocation() || !Handle->IsUsingAbsoluteRotation() ||
            !Handle->IsUsingAbsoluteScale() || PreviousComponents[Index] != Handle) return false;
    }

    for (int32 Index = 0; Index < PreviousPlan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = PreviousPlan.Leaves[Index];
        if (!PreviousHandles.IsValidIndex(Leaf.RootNodeIndex)) return false;
        USceneComponent* Component = PreviousLeaves[Index];
        UClass* ExpectedClass = Leaf.Kind == EMHRandomSemanticKind::Mesh
            ? UStaticMeshComponent::StaticClass()
            : (Leaf.Kind == EMHRandomSemanticKind::Actor ? UChildActorComponent::StaticClass() : nullptr);
        const FName ExpectedTag(*FString::Printf(
            TEXT("MH.Leaf:%s:%d:%s"), *Leaf.Origin, static_cast<int32>(Leaf.Kind), *Leaf.Resource));
        if (ExpectedClass == nullptr || !IsValid(Component) || Component->GetOwner() != &Target ||
            Component->GetClass() != ExpectedClass || !Component->IsRegistered() ||
            Component->ComponentTags.Num() != 1 || !Component->ComponentTags.Contains(ExpectedTag) ||
            Component->GetAttachParent() != PreviousHandles[Leaf.RootNodeIndex] ||
            !Component->IsUsingAbsoluteLocation() || !Component->IsUsingAbsoluteRotation() ||
            !Component->IsUsingAbsoluteScale() ||
            PreviousComponents[PreviousHandles.Num() + Index] != Component) return false;
        if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
        {
            const UStaticMesh* BoundMesh = Mesh->GetStaticMesh();
            const UStaticMesh* ExpectedMesh = PlanViewResolveLeafMesh(Leaf.Resource, Settings, OutResult.Error);
            if (!OutResult.Error.IsEmpty()) return true;
            if (!IsValid(BoundMesh) || BoundMesh != ExpectedMesh) return false;
            if (!MHIsAdmissibleAppearanceCustomDataBaseIndex(Settings.AppearanceCustomDataBaseIndex)) return false;
            const TArray<float>& Data = Mesh->GetCustomPrimitiveData().Data;
            for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
            {
                const int32 DataIndex = Settings.AppearanceCustomDataBaseIndex + Channel;
                if (!Data.IsValidIndex(DataIndex) ||
                    Data[DataIndex] != Leaf.AppearanceChannels[Channel]) return false;
            }
        }
        else if (const UChildActorComponent* Child = Cast<UChildActorComponent>(Component))
        {
            const FSoftClassPath* ExpectedPath = Settings.ActorClassRegistry.Find(Leaf.Resource);
            UClass* ExpectedActorClass = ExpectedPath != nullptr ? ExpectedPath->ResolveClass() : nullptr;
            if (!MHIsSpawnableCompositeActorClass(ExpectedActorClass) ||
                Child->GetChildActorClass() != ExpectedActorClass) return false;
        }
    }
    TArray<int32> PreviousIndexForCandidate;
    PreviousIndexForCandidate.Init(INDEX_NONE, CandidatePlan.Leaves.Num());
    TArray<bool> NeedsReplacement;
    NeedsReplacement.Init(true, CandidatePlan.Leaves.Num());
    int32 PositionalPathMismatches = 0;
    if (PreviousPlan.Leaves.Num() == CandidatePlan.Leaves.Num())
    {
        for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
            if (PreviousPlan.Leaves[Index].Origin != CandidatePlan.Leaves[Index].Origin)
                ++PositionalPathMismatches;
    }
    const bool bUsePositionalDiff = PreviousPlan.Leaves.Num() == CandidatePlan.Leaves.Num() &&
        PositionalPathMismatches <= 1;
    TMap<FString, int32> PreviousLeafByPath;
    if (!bUsePositionalDiff)
    {
        PreviousLeafByPath.Reserve(PreviousPlan.Leaves.Num());
        for (int32 Index = 0; Index < PreviousPlan.Leaves.Num(); ++Index)
        {
            const FString& Path = PreviousPlan.Leaves[Index].Origin;
            if (PreviousLeafByPath.Contains(Path)) return false;
            PreviousLeafByPath.Add(Path, Index);
        }
    }
    TSet<FString> CandidatePaths;
    if (!bUsePositionalDiff) CandidatePaths.Reserve(CandidatePlan.Leaves.Num());
    for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = CandidatePlan.Leaves[Index];
        int32 PreviousIndex = INDEX_NONE;
        if (bUsePositionalDiff)
        {
            PreviousIndex = Index;
        }
        else
        {
            if (CandidatePaths.Contains(Leaf.Origin)) return false;
            CandidatePaths.Add(Leaf.Origin);
            if (const int32* Found = PreviousLeafByPath.Find(Leaf.Origin)) PreviousIndex = *Found;
        }
        if (PreviousIndex == INDEX_NONE) continue;
        const FMHResolvedCompositeLeaf& OldLeaf = PreviousPlan.Leaves[PreviousIndex];
        if (OldLeaf.Origin != Leaf.Origin) continue;
        PreviousIndexForCandidate[Index] = PreviousIndex;
        if (OldLeaf.Kind != Leaf.Kind || OldLeaf.Resource != Leaf.Resource) continue;
        // A stable leaf may not silently move under another authored handle;
        // doing so would require the reattach forbidden by the fast path.
        if (OldLeaf.RootNodeIndex != Leaf.RootNodeIndex) return false;
        NeedsReplacement[Index] = false;
    }

    struct FReseedEndpoint
    {
        UStaticMesh* Mesh = nullptr;
        UClass* ActorClass = nullptr;
    };
    TArray<FReseedEndpoint> Endpoints;
    Endpoints.SetNum(CandidatePlan.Leaves.Num());
    TMap<FMHResourceKey, UStaticMesh*> SelectedMeshes;
    {
        FMHPlacementStageScope LoadStage(EMHPlacementStage::LoadEndpoints);
        for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
        {
            if (!NeedsReplacement[Index]) continue;
            const FMHResolvedCompositeLeaf& Leaf = CandidatePlan.Leaves[Index];
            FReseedEndpoint& Endpoint = Endpoints[Index];
            if (Leaf.Kind == EMHRandomSemanticKind::Mesh)
            {
                FMHResourceKey Key;
                Key.Kind = EMHResourceKind::StaticMesh;
                Key.LogicalName = Leaf.Resource;
                Endpoint.Mesh = PlanViewResolveLeafMesh(Leaf.Resource, Settings, OutResult.Error);
                if (!OutResult.Error.IsEmpty()) return true;
                if (Endpoint.Mesh != nullptr) SelectedMeshes.Add(Key, Endpoint.Mesh);
            }
            else if (Leaf.Kind == EMHRandomSemanticKind::Actor)
            {
                const FSoftClassPath* Path = Settings.ActorClassRegistry.Find(Leaf.Resource);
                Endpoint.ActorClass = Path != nullptr ? Path->TryLoadClass<AActor>() : nullptr;
                if (!MHIsSpawnableCompositeActorClass(Endpoint.ActorClass)) Endpoint.ActorClass = nullptr;
            }
            else
            {
                OutResult.Error = TEXT("MH_E_COMPOSITE_GRAMMAR: resolved leaf is neither mesh nor actor");
                return true;
            }
        }
    }
    PlanViewWaitSelectedMeshes(SelectedMeshes);

    const FMatrix Basis = Target.GetActorTransform().ToMatrixWithScale();
    for (int32 Index = 0; Index < PreviousHandles.Num(); ++Index)
    {
        USceneComponent* Handle = PreviousHandles[Index];
        OutResult.Components.Add(Handle);
        OutResult.TopLevelComponents.Add(Handle);
        PlanViewSetWorld(*Handle, PlanViewTrsMatrix(RootDefinition.Nodes[Index].Transform) * Basis);
    }
    for (int32 Index = 0; Index < CandidatePlan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = CandidatePlan.Leaves[Index];
        const FString Label = Leaf.DisplayName.IsEmpty() ? Leaf.Resource : Leaf.DisplayName;
        USceneComponent* Component = nullptr;
        if (!NeedsReplacement[Index])
        {
            Component = PreviousLeaves[PreviousIndexForCandidate[Index]];
            OutResult.Components.Add(Component);
        }
        else
        {
            const FReseedEndpoint& Endpoint = Endpoints[Index];
            const FName Key(*FString::Printf(
                TEXT("MH.Leaf:%s:%d:%s"), *Leaf.Origin, static_cast<int32>(Leaf.Kind), *Leaf.Resource));
            UClass* Class = Endpoint.Mesh != nullptr ? UStaticMeshComponent::StaticClass() :
                (Endpoint.ActorClass != nullptr ? UChildActorComponent::StaticClass() : nullptr);
            if (Class == nullptr)
            {
                Component = PlanViewPlaceholder(Target, Label, Key, OutResult);
                OutResult.Warnings.Add(TEXT("MH_W_UNRESOLVED_PLACEMENT: ") + Leaf.Origin + TEXT(" -> ") + Leaf.Resource);
            }
            else
            {
                Component = PlanViewNew(Target, Class, TEXT("MH_Leaf_") + Leaf.Resource, Key, OutResult, nullptr,
                    [&Endpoint](USceneComponent& New)
                    {
                        if (UStaticMeshComponent* NewMesh = Cast<UStaticMeshComponent>(&New))
                        {
                            MHRecordPlacementStaticMeshAssignment();
                            NewMesh->SetStaticMesh(Endpoint.Mesh);
                        }
                        else if (UChildActorComponent* NewChild = Cast<UChildActorComponent>(&New))
                        {
                            NewChild->SetEditorTreeViewVisualizationMode(
                                EChildActorComponentTreeViewVisualizationMode::Hidden);
                            NewChild->SetChildActorClass(Endpoint.ActorClass);
                        }
                    });
            }
            Component->SetAbsolute(true, true, true);
            MHRecordPlacementAttachment();
            Component->AttachToComponent(OutResult.TopLevelComponents[Leaf.RootNodeIndex],
                FAttachmentTransformRules::KeepWorldTransform);
            if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
            {
                MHRecordPlacementStaticMeshAssignment();
                Mesh->SetStaticMesh(Endpoint.Mesh);
            }
        }
        PlanViewSetWorld(*Component, Leaf.WorldMatrix * Basis);
        bool bAppearanceChanged = NeedsReplacement[Index];
        if (!bAppearanceChanged)
        {
            const FMHResolvedCompositeLeaf& OldLeaf =
                PreviousPlan.Leaves[PreviousIndexForCandidate[Index]];
            bAppearanceChanged = FMemory::Memcmp(
                OldLeaf.AppearanceChannels, Leaf.AppearanceChannels,
                sizeof(Leaf.AppearanceChannels)) != 0;
        }
        if (bAppearanceChanged)
        {
            MHRecordPlacementAppearanceUpdate();
            MHApplyLeafAppearanceCustomData(Component, Leaf, Settings.AppearanceCustomDataBaseIndex);
        }
        if (UChildActorComponent* Child = Cast<UChildActorComponent>(Component))
        {
            Child->SetEditorTreeViewVisualizationMode(EChildActorComponentTreeViewVisualizationMode::Hidden);
            if (AActor* Actor = Child->GetChildActor()) Actor->SetActorLabel(Label, false);
        }
        OutResult.LeafComponents.Add(Component);
        OutResult.LeafMaterializations.Add(
            {Component, INDEX_NONE, Leaf.OwningResolvedNodeIndex, Leaf.Origin});
    }
    return true;
}

FMHCompositePlacementCompileResult MHCompileCompositePlacementV5(AActor& Target,
    const FMHResolvedCompositePlan& Plan, const FMHRandomComposite& RootDefinition,
    const UMHCompositeSettings& Settings, TConstArrayView<TObjectPtr<UActorComponent>> PreviousComponents,
    const FString& UninstancedLeafPath)
{
    FMHPlacementStageScope CompileStage(EMHPlacementStage::CompilePlacement);
    FMHCompositePlacementCompileResult Result;
    if (!PlanViewPreflight(Plan, RootDefinition, Target.GetActorTransform(), Result.Error)) return Result;
    if (!MHIsAdmissibleAppearanceCustomDataBaseIndex(Settings.AppearanceCustomDataBaseIndex))
    {
        Result.Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: appearance custom data layout exceeds the engine limit");
        return Result;
    }
    const int32 AppearanceLayout =
        Settings.AppearanceCustomDataBaseIndex + MH_APPEARANCE_CHANNELS;
    struct FEndpoint { UStaticMesh* Mesh = nullptr; UClass* ActorClass = nullptr; };
    TArray<FEndpoint> Endpoints;
    TMap<FMHResourceKey, UStaticMesh*> SelectedMeshes;
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
                Endpoint.Mesh = PlanViewResolveLeafMesh(Leaf.Resource, Settings, Result.Error);
                if (!Result.Error.IsEmpty()) return Result;
                if (Endpoint.Mesh != nullptr) SelectedMeshes.Add(Key, Endpoint.Mesh);
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
    PlanViewWaitSelectedMeshes(SelectedMeshes);
    const FMatrix Basis = Target.GetActorTransform().ToMatrixWithScale();
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

    // Static leaves materialize through the level's instance pool (16 §2.8,
    // R5b-1): one bucket per {level, descriptor} shared by every placement,
    // stable handles, no ISM on this actor. Without a pool (non-editor world)
    // every static leaf takes the ordinary per-leaf component path below.
    UMHInstancePoolSubsystem* Pool = PlanViewPool(Target);
    ULevel* Level = Target.GetLevel();
    TBitArray<> Pooled(false, Plan.Leaves.Num());
    Result.LeafComponents.SetNum(Plan.Leaves.Num());
    Result.LeafMaterializations.SetNum(Plan.Leaves.Num());
    if (Pool != nullptr && Level != nullptr)
    {
        // A full compile re-materializes this owner from scratch: the pool
        // keeps its buckets, the handles are reissued.
        Pool->BeginBulk();
        Pool->RemoveOwner(Target);
        TMap<UStaticMesh*, FMHPoolBucketDescriptor> Descriptors;
        for (int32 LeafIndex = 0; LeafIndex < Plan.Leaves.Num(); ++LeafIndex)
        {
            const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[LeafIndex];
            UStaticMesh* Mesh = Endpoints[LeafIndex].Mesh;
            if (Mesh == nullptr || Leaf.Origin == UninstancedLeafPath) continue;
            FMHPoolBucketDescriptor* Descriptor = Descriptors.Find(Mesh);
            if (Descriptor == nullptr) Descriptor = &Descriptors.Add(Mesh, PlanViewPoolDescriptor(*Mesh, Settings));
            MHRecordPlacementWorldTransformUpdate();
            MHRecordPlacementAppearanceUpdate();
            const FMHInstanceHandle Handle = Pool->Add(Target, Leaf.Origin, *Level, *Descriptor,
                Leaf.WorldMatrix * Basis, Leaf.AppearanceChannels);
            if (!Handle.IsSet() || !PlanViewPooledRow(*Pool, Handle, Leaf, Result.LeafMaterializations[LeafIndex]))
            {
                Pool->RemoveOwner(Target);
                Pool->EndBulk();
                Result.Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: instance pool refused ") + Leaf.Origin + TEXT(" -> ") + Leaf.Resource;
                return Result;
            }
            Result.LeafComponents[LeafIndex] = Result.LeafMaterializations[LeafIndex].Component;
            Pooled[LeafIndex] = true;
        }
        Pool->EndBulk();
    }

    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        if (Pooled[Index]) continue;
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
                    if (UStaticMeshComponent* NewMesh = Cast<UStaticMeshComponent>(&New))
                    {
                        MHRecordPlacementStaticMeshAssignment();
                        NewMesh->SetStaticMesh(Endpoint.Mesh);
                    }
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
        MHRecordPlacementAttachment();
        Component->AttachToComponent(Result.TopLevelComponents[Leaf.RootNodeIndex], FAttachmentTransformRules::KeepWorldTransform);
        PlanViewSetWorld(*Component, Leaf.WorldMatrix * Basis);
        if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
        {
            MHRecordPlacementStaticMeshAssignment();
            Mesh->SetStaticMesh(Endpoint.Mesh);
        }
        // S6.3.1: the resolved appearance channels reach the material as Custom
        // Primitive Data. Materialization side only - never part of a preimage.
        MHRecordPlacementAppearanceUpdate();
        MHApplyLeafAppearanceCustomData(Component, Leaf, Settings.AppearanceCustomDataBaseIndex);
        if (UChildActorComponent* Child = Cast<UChildActorComponent>(Component))
        {
            Child->SetEditorTreeViewVisualizationMode(EChildActorComponentTreeViewVisualizationMode::Hidden);
            if (Child->GetChildActorClass() != Endpoint.ActorClass) Child->SetChildActorClass(Endpoint.ActorClass);
            if (AActor* Actor = Child->GetChildActor()) Actor->SetActorLabel(Label, false);
        }
        Result.LeafComponents[Index] = Component;
        Result.LeafMaterializations[Index] = {
            Component, INDEX_NONE, Leaf.OwningResolvedNodeIndex, Leaf.Origin};
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
