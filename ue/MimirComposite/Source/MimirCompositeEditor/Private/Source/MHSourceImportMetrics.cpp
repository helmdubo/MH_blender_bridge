#include "Source/MHSourceImportMetrics.h"

#include "HAL/PlatformTime.h"

namespace UE::MimirComposite
{
namespace
{
FMHSourceImportMetrics GMHSourceImportMetrics;
thread_local FMHSourceImportMetricScope* GMHSourceImportCurrentScope = nullptr;
}

uint64 FMHSourceImportMetrics::CallsForStage(const EMHSourceImportMetricStage Stage) const
{
    uint64 Calls = 0;
    for (int32 ResourceIndex = 0; ResourceIndex < ResourceCount; ++ResourceIndex)
    {
        Calls += Metrics[ResourceIndex * StageCount + static_cast<int32>(Stage)].Calls;
    }
    return Calls;
}

FMHSourceImportMetricScope::FMHSourceImportMetricScope(
    const EMHSourceImportMetricResource InResource,
    const EMHSourceImportMetricStage InStage)
    : Resource(InResource),
      Stage(InStage),
      StartCycles(FPlatformTime::Cycles64()),
      Parent(GMHSourceImportCurrentScope)
{
    check(Resource < EMHSourceImportMetricResource::Count);
    check(Stage < EMHSourceImportMetricStage::Count);
    GMHSourceImportCurrentScope = this;
}

FMHSourceImportMetricScope::~FMHSourceImportMetricScope()
{
    check(GMHSourceImportCurrentScope == this);
    const uint64 InclusiveCycles = FPlatformTime::Cycles64() - StartCycles;
    const int32 Index = static_cast<int32>(Resource) * FMHSourceImportMetrics::StageCount +
        static_cast<int32>(Stage);
    FMHSourceImportMetric& Metric = GMHSourceImportMetrics.Metrics[Index];
    ++Metric.Calls;
    Metric.InclusiveCycles += InclusiveCycles;
    Metric.ExclusiveCycles += InclusiveCycles - FMath::Min(InclusiveCycles, ChildCycles);
    GMHSourceImportCurrentScope = Parent;
    if (Parent != nullptr)
    {
        Parent->ChildCycles += InclusiveCycles;
    }
}

void MHResetSourceImportMetrics()
{
    check(GMHSourceImportCurrentScope == nullptr);
    GMHSourceImportMetrics = FMHSourceImportMetrics();
}

FMHSourceImportMetrics MHGetSourceImportMetrics()
{
    return GMHSourceImportMetrics;
}

void MHRecordSourceImportProgressScope()
{
    ++GMHSourceImportMetrics.ProgressScopes;
}

void MHRecordSourceImportProgressResourceTick()
{
    ++GMHSourceImportMetrics.ProgressResourceTicks;
}

const TCHAR* MHSourceImportMetricResourceLabel(const EMHSourceImportMetricResource Resource)
{
    switch (Resource)
    {
    case EMHSourceImportMetricResource::Texture: return TEXT("Texture");
    case EMHSourceImportMetricResource::Material: return TEXT("Material");
    case EMHSourceImportMetricResource::StaticMesh: return TEXT("StaticMesh");
    case EMHSourceImportMetricResource::Composite: return TEXT("Composite");
    case EMHSourceImportMetricResource::Batch: return TEXT("Batch");
    case EMHSourceImportMetricResource::Count: break;
    }
    return TEXT("Unknown");
}

const TCHAR* MHSourceImportMetricStageLabel(const EMHSourceImportMetricStage Stage)
{
    switch (Stage)
    {
    case EMHSourceImportMetricStage::Create: return TEXT("Create");
    case EMHSourceImportMetricStage::BuildWait: return TEXT("BuildWait");
    case EMHSourceImportMetricStage::SavePackage: return TEXT("SavePackage");
    case EMHSourceImportMetricStage::Count: break;
    }
    return TEXT("Unknown");
}
} // namespace UE::MimirComposite
