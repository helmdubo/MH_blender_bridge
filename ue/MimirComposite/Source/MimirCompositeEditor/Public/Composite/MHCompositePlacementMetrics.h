#pragma once

#include "Containers/StaticArray.h"
#include "CoreMinimal.h"

namespace UE::MimirComposite
{
/** Editor-only measurement stages for the S6.5 placement baseline. */
enum class EMHPlacementStage : uint8
{
    BuildAppliedGraph,
    ResolveCompositePlan,
    LoadEndpoints,
    WaitStaticMeshCompilation,
    CompilePlacement,
    RegisterComponents,
    DestroyRetiredComponents,
    Count
};

struct MIMIRCOMPOSITEEDITOR_API FMHPlacementStageMetric
{
    uint64 Calls = 0;
    uint64 InclusiveCycles = 0;
    uint64 ExclusiveCycles = 0;
};

struct MIMIRCOMPOSITEEDITOR_API FMHPlacementStageMetrics
{
    static constexpr int32 StageCount = static_cast<int32>(EMHPlacementStage::Count);
    TStaticArray<FMHPlacementStageMetric, StageCount> Stages;

    const FMHPlacementStageMetric& Get(EMHPlacementStage Stage) const
    {
        return Stages[static_cast<int32>(Stage)];
    }
};

/** Non-serialized physical work counters for the shared definition cache. */
struct MIMIRCOMPOSITEEDITOR_API FMHDefinitionCacheMetrics
{
    uint64 Hits = 0;
    uint64 Misses = 0;
    uint64 ClosureHitBuilds = 0;
    uint64 EndpointResolves = 0;
    uint64 EndpointHits = 0;
    uint64 EndpointStores = 0;
    uint64 DeadEndpointReloads = 0;
    // Definition-map entries actually examined; a linear scan over the whole
    // pool and an indexed bucket walk are only distinguishable here.
    uint64 LookupProbes = 0;
    uint64 InvalidationProbes = 0;
};

/** Non-serialized component work performed while materializing a placement. */
struct MIMIRCOMPOSITEEDITOR_API FMHPlacementMutationMetrics
{
    uint64 CreatedComponents = 0;
    uint64 DestroyedComponents = 0;
    uint64 RegisteredComponents = 0;
    uint64 Attachments = 0;
    uint64 StaticMeshAssignments = 0;
    uint64 WorldTransformUpdates = 0;
    uint64 AppearanceUpdates = 0;
};

/** Non-serialized layout-reseed comparison and path-selection counters. */
struct MIMIRCOMPOSITEEDITOR_API FMHPlacementReseedMetrics
{
    uint64 Attempts = 0;
    uint64 IncrementalApplied = 0;
    uint64 FullFallbacks = 0;
    uint64 PreviousLeaves = 0;
    uint64 CandidateLeaves = 0;
    uint64 StableLeafIdentities = 0;
    uint64 ChangedLeafIdentities = 0;
    uint64 AddedLeafIdentities = 0;
    uint64 RemovedLeafIdentities = 0;
};

/**
 * Non-serialized endpoint resolve/admission counters (Recipe Model M0).
 * They only measure the existing resolve path; R0 replaces that path and
 * asserts the forbidden-in-preview counters stay at zero.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHEndpointResolveMetrics
{
    // Endpoint resolve requests by resource key (R0: prototype registry lookups).
    uint64 RegistryLookups = 0;
    // Asset Registry queries filtered by MH.* tags.
    uint64 AssetRegistryTagQueries = 0;
    // Synchronous loads that brought a non-resident object into memory.
    uint64 PackageLoadsSync = 0;
    // Receipt validations of a live object (R0: identity admissions).
    uint64 IdentityAdmissions = 0;
    // Live GetAssetRegistryTags reads through FAssetData(&Object).
    uint64 LiveReceiptTagReads = 0;
};

/**
 * Inclusive/exclusive game-thread timing scope. Instrumentation is observable
 * only through the test API below and is never serialized or read by placement
 * production decisions.
 */
class MIMIRCOMPOSITEEDITOR_API FMHPlacementStageScope final
{
public:
    explicit FMHPlacementStageScope(EMHPlacementStage InStage);
    ~FMHPlacementStageScope();

    FMHPlacementStageScope(const FMHPlacementStageScope&) = delete;
    FMHPlacementStageScope& operator=(const FMHPlacementStageScope&) = delete;

private:
    EMHPlacementStage Stage;
    uint64 StartCycles = 0;
    uint64 ChildCycles = 0;
    FMHPlacementStageScope* Parent = nullptr;
};

MIMIRCOMPOSITEEDITOR_API void MHResetPlacementStageMetrics();
MIMIRCOMPOSITEEDITOR_API FMHPlacementStageMetrics MHGetPlacementStageMetrics();
MIMIRCOMPOSITEEDITOR_API const TCHAR* MHPlacementStageLabel(EMHPlacementStage Stage);
MIMIRCOMPOSITEEDITOR_API void MHResetDefinitionCacheMetrics();
MIMIRCOMPOSITEEDITOR_API FMHDefinitionCacheMetrics MHGetDefinitionCacheMetrics();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionCacheHit();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionCacheMiss();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionClosureHitBuild();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionEndpointResolve();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionEndpointHit();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionEndpointStore();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionDeadEndpointReload();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionLookupProbe();
MIMIRCOMPOSITEEDITOR_API void MHRecordDefinitionInvalidationProbe();
MIMIRCOMPOSITEEDITOR_API void MHResetPlacementMutationMetrics();
MIMIRCOMPOSITEEDITOR_API FMHPlacementMutationMetrics MHGetPlacementMutationMetrics();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementComponentCreated();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementComponentDestroyed();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementComponentRegistered();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementAttachment();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementStaticMeshAssignment();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementWorldTransformUpdate();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementAppearanceUpdate();
MIMIRCOMPOSITEEDITOR_API void MHResetPlacementReseedMetrics();
MIMIRCOMPOSITEEDITOR_API FMHPlacementReseedMetrics MHGetPlacementReseedMetrics();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementReseedComparison(
    uint64 PreviousLeaves, uint64 CandidateLeaves, uint64 StableLeafIdentities,
    uint64 ChangedLeafIdentities, uint64 AddedLeafIdentities, uint64 RemovedLeafIdentities);
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementReseedIncrementalApplied();
MIMIRCOMPOSITEEDITOR_API void MHRecordPlacementReseedFullFallback();
MIMIRCOMPOSITEEDITOR_API void MHResetEndpointResolveMetrics();
MIMIRCOMPOSITEEDITOR_API FMHEndpointResolveMetrics MHGetEndpointResolveMetrics();
MIMIRCOMPOSITEEDITOR_API void MHRecordEndpointRegistryLookup();
MIMIRCOMPOSITEEDITOR_API void MHRecordEndpointAssetRegistryTagQuery();
MIMIRCOMPOSITEEDITOR_API void MHRecordEndpointPackageLoadSync();
MIMIRCOMPOSITEEDITOR_API void MHRecordEndpointIdentityAdmission();
MIMIRCOMPOSITEEDITOR_API void MHRecordEndpointLiveReceiptTagRead();
} // namespace UE::MimirComposite
