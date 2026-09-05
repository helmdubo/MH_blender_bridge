#include "Composite/MHCompositePlacementEvents.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHProofCache.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Performance/MHPerformanceTrace.h"
#include "UObject/UObjectIterator.h"

namespace UE::MimirComposite
{

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
TFunction<void(const FMHResourceKey&)> GGeneratedResourceChangedObserverForTests;
}
#endif

int32 MHRebuildAllLoadedCompositeActors()
{
    int32 RebuiltCount = 0;
    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        UWorld* World = IsValid(Actor) ? Actor->GetWorld() : nullptr;
        if (!IsValid(Actor) || Actor->IsTemplate() || Actor->IsActorBeingDestroyed() ||
            Actor->IsPlacementEditMode() || World == nullptr || World->IsGameWorld() ||
            World->IsBeingCleanedUp() || World->IsCleanedUp())
        {
            continue;
        }
        Actor->RebuildComposite();
        ++RebuiltCount;
    }
    return RebuiltCount;
}

void MHNotifyGeneratedResourceChanged(const FMHResourceKey& Key)
{
    if (!Key.IsCanonical())
    {
        return;
    }
    MHRecordReimportNotifiedResource(Key);

#if WITH_DEV_AUTOMATION_TESTS
    if (GGeneratedResourceChangedObserverForTests)
    {
        GGeneratedResourceChangedObserverForTests(Key);
    }
#endif

    FMHEndpointInterfaceDelta Delta;
    TArray<TWeakObjectPtr<AMHCompositeActor>> AffectedActors;
    bool bNeedsMeshAdmission = false;
    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        UWorld* World = IsValid(Actor) ? Actor->GetWorld() : nullptr;
        if (!IsValid(Actor) || Actor->IsTemplate() || Actor->IsActorBeingDestroyed() ||
            World == nullptr || World->IsBeingCleanedUp() || World->IsCleanedUp() ||
            !Actor->DependsOnResource(Key)) continue;
        AffectedActors.Add(Actor);
        const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
        // Dependency graphs include unselected options. First admission of such
        // an option is not recovery of a missing rendered leaf (DECIDED-R3B-1).
        bNeedsMeshAdmission |= Plan == nullptr || Plan->Leaves.ContainsByPredicate(
            [&](const FMHResolvedCompositeLeaf& Leaf)
            { return Leaf.Kind == EMHRandomSemanticKind::Mesh && Leaf.Resource == Key.LogicalName; });
    }
    bool bMeshDeltaKnown = false;
    UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get();
    const bool bTrace = MHIsReimportPerfActive();
    const uint32 GenerationBefore = bTrace && Recipes != nullptr ? Recipes->GetGeneration() : 0;
    // Observe actual compilations through the existing registry API. Do not
    // compile recipes merely to measure them, and do no extra traversal with tracing off.
    TMap<TWeakObjectPtr<const UMHCompositeAsset>, TOptional<uint32>> ParentRecipesBefore;
    if (bTrace && Recipes != nullptr && Key.Kind == EMHResourceKind::Composite)
    {
        TArray<FMHResourceKey> Pending = {Key};
        TSet<FString> Seen = {Key.LogicalName};
        while (!Pending.IsEmpty())
        {
            const FMHResourceKey Dependency = Pending.Pop();
            for (const TWeakObjectPtr<const UMHCompositeAsset>& Parent : Recipes->GetDependents(Dependency))
            {
                const UMHCompositeAsset* Asset = Parent.Get();
                if (Asset == nullptr || Seen.Contains(Asset->LogicalName)) continue;
                Seen.Add(Asset->LogicalName);
                const FMHCompiledRecipe* Cached = Recipes->Find(*Asset);
                ParentRecipesBefore.Add(Parent, Cached != nullptr
                    ? TOptional<uint32>(Cached->RecipeRevision) : TOptional<uint32>());
                FMHResourceKey ParentKey;
                ParentKey.Kind = EMHResourceKind::Composite;
                ParentKey.LogicalName = Asset->LogicalName;
                Pending.Add(ParentKey);
            }
        }
    }
    if (GEditor != nullptr)
    {
        if (UMHEndpointPrototypeRegistry* Registry =
                GEditor->GetEditorSubsystem<UMHEndpointPrototypeRegistry>())
        {
            Registry->Invalidate(Key);
            if (Key.Kind == EMHResourceKind::StaticMesh && bNeedsMeshAdmission)
            {
                Registry->Resolve(Key);
                Delta = Registry->GetLastInterfaceDelta(Key);
                bMeshDeltaKnown = true;
            }
        }
        if (Recipes != nullptr)
        {
            // 16 §4: a composite reimport recompiles that recipe only; an inlined
            // profile reimport recompiles the recipes carrying it (R2b-2).
            if (Key.Kind == EMHResourceKind::Composite) Recipes->InvalidateComposite(Key.LogicalName);
            else if (Key.Kind == EMHResourceKind::PlacementProfile) Recipes->InvalidateProfile(Key.LogicalName);
        }
        if (UMHProofCacheSubsystem* Proofs = GEditor->GetEditorSubsystem<UMHProofCacheSubsystem>())
        {
            if (Key.Kind != EMHResourceKind::StaticMesh || !bMeshDeltaKnown || Delta.Any()) Proofs->InvalidateAll();
        }
    }

    if (Key.Kind == EMHResourceKind::Material || Key.Kind == EMHResourceKind::Texture) return;

    for (const TWeakObjectPtr<AMHCompositeActor>& Affected : AffectedActors)
    {
        AMHCompositeActor* Actor = Affected.Get();
        UWorld* World = IsValid(Actor) ? Actor->GetWorld() : nullptr;
        if (!IsValid(Actor) || Actor->IsTemplate() || Actor->IsActorBeingDestroyed() ||
            World == nullptr || World->IsBeingCleanedUp() || World->IsCleanedUp() ||
            !Actor->DependsOnResource(Key))
        {
            continue;
        }
        const auto Reconcile = [&]()
        {
            if (Key.Kind == EMHResourceKind::StaticMesh) Actor->ReconcileEndpoint(Key, Delta);
            else if (Key.Kind == EMHResourceKind::Composite) Actor->ReconcileRecipe(Key);
            else Actor->RebuildComposite();
        };
        if (bTrace)
        {
            const uint64 RebuildStart = FPlatformTime::Cycles64();
            const uint32 RebuildsBefore = Actor->GetPlacementRebuildCount();
            Reconcile();
            if (Actor->GetPlacementRebuildCount() != RebuildsBefore)
                MHRecordReimportActorRebuild(*Actor, FPlatformTime::Cycles64() - RebuildStart);
            else MHRecordReimportActorReconciled();
        }
        else
        {
            Reconcile();
        }
    }
    if (bTrace && Recipes != nullptr)
    {
        uint64 ParentsRecompiled = 0;
        for (const auto& Pair : ParentRecipesBefore)
        {
            const UMHCompositeAsset* Parent = Pair.Key.Get();
            const FMHCompiledRecipe* After = Parent != nullptr ? Recipes->Find(*Parent) : nullptr;
            if (After != nullptr && (!Pair.Value.IsSet() || Pair.Value.GetValue() != After->RecipeRevision))
                ++ParentsRecompiled;
        }
        MHRecordReimportRecipeCompilations(Recipes->GetGeneration() - GenerationBefore, ParentsRecompiled);
    }
}

void MHNotifyCompositeAssetChanged(UMHCompositeAsset& Asset)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Asset.LogicalName;
    MHNotifyGeneratedResourceChanged(Key);
}

#if WITH_DEV_AUTOMATION_TESTS
void MHSetGeneratedResourceChangedObserverForTests(
    TFunction<void(const FMHResourceKey&)> Observer)
{
    GGeneratedResourceChangedObserverForTests = MoveTemp(Observer);
}
#endif

} // namespace UE::MimirComposite
