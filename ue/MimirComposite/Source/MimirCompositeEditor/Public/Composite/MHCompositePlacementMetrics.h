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
} // namespace UE::MimirComposite
