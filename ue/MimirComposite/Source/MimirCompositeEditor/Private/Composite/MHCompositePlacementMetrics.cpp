#include "Composite/MHCompositePlacementMetrics.h"

#include "HAL/PlatformTime.h"

namespace UE::MimirComposite
{
namespace
{
FMHPlacementStageMetrics GMHPlacementStageMetrics;
FMHDefinitionCacheMetrics GMHDefinitionCacheMetrics;
FMHPlacementMutationMetrics GMHPlacementMutationMetrics;
FMHPlacementReseedMetrics GMHPlacementReseedMetrics;
thread_local FMHPlacementStageScope* GMHPlacementStageCurrentScope = nullptr;
}

FMHPlacementStageScope::FMHPlacementStageScope(const EMHPlacementStage InStage)
    : Stage(InStage), StartCycles(FPlatformTime::Cycles64()), Parent(GMHPlacementStageCurrentScope)
{
    check(Stage < EMHPlacementStage::Count);
    GMHPlacementStageCurrentScope = this;
}

FMHPlacementStageScope::~FMHPlacementStageScope()
{
    check(GMHPlacementStageCurrentScope == this);
    const uint64 InclusiveCycles = FPlatformTime::Cycles64() - StartCycles;
    FMHPlacementStageMetric& Metric = GMHPlacementStageMetrics.Stages[static_cast<int32>(Stage)];
    ++Metric.Calls;
    Metric.InclusiveCycles += InclusiveCycles;
    Metric.ExclusiveCycles += InclusiveCycles - FMath::Min(InclusiveCycles, ChildCycles);
    GMHPlacementStageCurrentScope = Parent;
    if (Parent != nullptr) Parent->ChildCycles += InclusiveCycles;
}

void MHResetPlacementStageMetrics()
{
    check(GMHPlacementStageCurrentScope == nullptr);
    GMHPlacementStageMetrics = FMHPlacementStageMetrics();
}

FMHPlacementStageMetrics MHGetPlacementStageMetrics()
{
    return GMHPlacementStageMetrics;
}

void MHResetDefinitionCacheMetrics()
{
    GMHDefinitionCacheMetrics = FMHDefinitionCacheMetrics();
}

FMHDefinitionCacheMetrics MHGetDefinitionCacheMetrics()
{
    return GMHDefinitionCacheMetrics;
}

void MHRecordDefinitionCacheHit()
{
    ++GMHDefinitionCacheMetrics.Hits;
}

void MHRecordDefinitionCacheMiss()
{
    ++GMHDefinitionCacheMetrics.Misses;
}

void MHRecordDefinitionClosureHitBuild()
{
    ++GMHDefinitionCacheMetrics.ClosureHitBuilds;
}

void MHRecordDefinitionEndpointResolve()
{
    ++GMHDefinitionCacheMetrics.EndpointResolves;
}

void MHRecordDefinitionEndpointHit()
{
    ++GMHDefinitionCacheMetrics.EndpointHits;
}

void MHRecordDefinitionEndpointStore()
{
    ++GMHDefinitionCacheMetrics.EndpointStores;
}

void MHRecordDefinitionDeadEndpointReload()
{
    ++GMHDefinitionCacheMetrics.DeadEndpointReloads;
}

void MHRecordDefinitionLookupProbe()
{
    ++GMHDefinitionCacheMetrics.LookupProbes;
}

void MHRecordDefinitionInvalidationProbe()
{
    ++GMHDefinitionCacheMetrics.InvalidationProbes;
}

void MHResetPlacementMutationMetrics()
{
    GMHPlacementMutationMetrics = FMHPlacementMutationMetrics();
}

FMHPlacementMutationMetrics MHGetPlacementMutationMetrics()
{
    return GMHPlacementMutationMetrics;
}

void MHRecordPlacementComponentCreated()
{
    ++GMHPlacementMutationMetrics.CreatedComponents;
}

void MHRecordPlacementComponentDestroyed()
{
    ++GMHPlacementMutationMetrics.DestroyedComponents;
}

void MHRecordPlacementComponentRegistered()
{
    ++GMHPlacementMutationMetrics.RegisteredComponents;
}

void MHRecordPlacementAttachment()
{
    ++GMHPlacementMutationMetrics.Attachments;
}

void MHRecordPlacementStaticMeshAssignment()
{
    ++GMHPlacementMutationMetrics.StaticMeshAssignments;
}

void MHRecordPlacementWorldTransformUpdate()
{
    ++GMHPlacementMutationMetrics.WorldTransformUpdates;
}

void MHRecordPlacementAppearanceUpdate()
{
    ++GMHPlacementMutationMetrics.AppearanceUpdates;
}

void MHResetPlacementReseedMetrics()
{
    GMHPlacementReseedMetrics = FMHPlacementReseedMetrics();
}

FMHPlacementReseedMetrics MHGetPlacementReseedMetrics()
{
    return GMHPlacementReseedMetrics;
}

void MHRecordPlacementReseedComparison(
    const uint64 PreviousLeaves, const uint64 CandidateLeaves, const uint64 StableLeafIdentities,
    const uint64 ChangedLeafIdentities, const uint64 AddedLeafIdentities, const uint64 RemovedLeafIdentities)
{
    ++GMHPlacementReseedMetrics.Attempts;
    GMHPlacementReseedMetrics.PreviousLeaves += PreviousLeaves;
    GMHPlacementReseedMetrics.CandidateLeaves += CandidateLeaves;
    GMHPlacementReseedMetrics.StableLeafIdentities += StableLeafIdentities;
    GMHPlacementReseedMetrics.ChangedLeafIdentities += ChangedLeafIdentities;
    GMHPlacementReseedMetrics.AddedLeafIdentities += AddedLeafIdentities;
    GMHPlacementReseedMetrics.RemovedLeafIdentities += RemovedLeafIdentities;
}

void MHRecordPlacementReseedIncrementalApplied()
{
    ++GMHPlacementReseedMetrics.IncrementalApplied;
}

void MHRecordPlacementReseedFullFallback()
{
    ++GMHPlacementReseedMetrics.FullFallbacks;
}

const TCHAR* MHPlacementStageLabel(const EMHPlacementStage Stage)
{
    switch (Stage)
    {
    case EMHPlacementStage::BuildAppliedGraph: return TEXT("BuildAppliedGraph");
    case EMHPlacementStage::ResolveCompositePlan: return TEXT("ResolveCompositePlan");
    case EMHPlacementStage::LoadEndpoints: return TEXT("LoadEndpoints");
    case EMHPlacementStage::WaitStaticMeshCompilation: return TEXT("WaitStaticMeshCompilation");
    case EMHPlacementStage::CompilePlacement: return TEXT("CompilePlacement");
    case EMHPlacementStage::RegisterComponents: return TEXT("RegisterComponents");
    case EMHPlacementStage::DestroyRetiredComponents: return TEXT("DestroyRetiredComponents");
    case EMHPlacementStage::Count: break;
    }
    return TEXT("Unknown");
}
} // namespace UE::MimirComposite
