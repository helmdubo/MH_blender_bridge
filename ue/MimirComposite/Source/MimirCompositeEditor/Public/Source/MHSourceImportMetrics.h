#pragma once

#include "Containers/StaticArray.h"
#include "CoreMinimal.h"

namespace UE::MimirComposite
{
/** Non-serialized resource buckets for bulk-import performance measurements. */
enum class EMHSourceImportMetricResource : uint8
{
    Texture,
    Material,
    StaticMesh,
    Composite,
    Batch,
    Count
};

/** Mutually exclusive work buckets inside one resource import. */
enum class EMHSourceImportMetricStage : uint8
{
    Create,
    BuildWait,
    SavePackage,
    Count
};

struct MIMIRCOMPOSITEEDITOR_API FMHSourceImportMetric
{
    uint64 Calls = 0;
    uint64 InclusiveCycles = 0;
    uint64 ExclusiveCycles = 0;
};

struct MIMIRCOMPOSITEEDITOR_API FMHSourceImportMetrics
{
    static constexpr int32 ResourceCount =
        static_cast<int32>(EMHSourceImportMetricResource::Count);
    static constexpr int32 StageCount =
        static_cast<int32>(EMHSourceImportMetricStage::Count);
    static constexpr int32 MetricCount = ResourceCount * StageCount;

    TStaticArray<FMHSourceImportMetric, MetricCount> Metrics;
    uint64 ProgressScopes = 0;
    uint64 ProgressResourceTicks = 0;

    const FMHSourceImportMetric& Get(
        EMHSourceImportMetricResource Resource,
        EMHSourceImportMetricStage Stage) const
    {
        return Metrics[
            static_cast<int32>(Resource) * StageCount + static_cast<int32>(Stage)];
    }

    uint64 CallsForStage(EMHSourceImportMetricStage Stage) const;
};

/**
 * Inclusive/exclusive game-thread timer. Measurements are observational only:
 * import code never reads them to choose behavior.
 */
class MIMIRCOMPOSITEEDITOR_API FMHSourceImportMetricScope final
{
public:
    FMHSourceImportMetricScope(
        EMHSourceImportMetricResource InResource,
        EMHSourceImportMetricStage InStage);
    ~FMHSourceImportMetricScope();

    FMHSourceImportMetricScope(const FMHSourceImportMetricScope&) = delete;
    FMHSourceImportMetricScope& operator=(const FMHSourceImportMetricScope&) = delete;

private:
    EMHSourceImportMetricResource Resource;
    EMHSourceImportMetricStage Stage;
    uint64 StartCycles = 0;
    uint64 ChildCycles = 0;
    FMHSourceImportMetricScope* Parent = nullptr;
};

MIMIRCOMPOSITEEDITOR_API void MHResetSourceImportMetrics();
MIMIRCOMPOSITEEDITOR_API FMHSourceImportMetrics MHGetSourceImportMetrics();
MIMIRCOMPOSITEEDITOR_API void MHRecordSourceImportProgressScope();
MIMIRCOMPOSITEEDITOR_API void MHRecordSourceImportProgressResourceTick();
MIMIRCOMPOSITEEDITOR_API const TCHAR* MHSourceImportMetricResourceLabel(
    EMHSourceImportMetricResource Resource);
MIMIRCOMPOSITEEDITOR_API const TCHAR* MHSourceImportMetricStageLabel(
    EMHSourceImportMetricStage Stage);
} // namespace UE::MimirComposite
