#include "Performance/MHPerformanceTrace.h"

#include "HAL/IConsoleManager.h"

namespace
{
TAutoConsoleVariable<int32> CVarMHPerfTrace(
    TEXT("mh.PerfTrace"),
    0,
    TEXT("Mimir performance trace verbosity: 0=off, 1=aggregates, 2=verbose."),
    ECVF_Default);
}

namespace UE::MimirComposite
{
struct FMHMapLoadInitialBuildScope::FImpl {};
struct FMHSourceScanPerfScope::FImpl {};
struct FMHReimportPerfScope::FImpl {};

int32 MHGetPerfTraceLevel() { return CVarMHPerfTrace.GetValueOnGameThread(); }
void MHResetPerformanceTraceForTests() {}
FMHMapLoadPerfReport MHGetMapLoadPerfReportForTests() { return {}; }
FMHStartupScanPerfReport MHGetStartupScanPerfReportForTests() { return {}; }
FMHReimportPerfReport MHGetReimportPerfReportForTests() { return {}; }

FMHMapLoadInitialBuildScope::FMHMapLoadInitialBuildScope() = default;
FMHMapLoadInitialBuildScope::~FMHMapLoadInitialBuildScope() = default;
void FMHMapLoadInitialBuildScope::Complete(const AMHCompositeActor&) {}
void MHRecordMapLoadGraph(const FString&, const FMHRandomSourceGraph&) {}
void MHRecordMapLoadSelectedPlan(const FMHResolvedCompositePlan&) {}
void MHRecordMapLoadCompilingMesh(const FMHResourceKey&, const FString&) {}
void MHFlushMapLoadPerfReport() {}

FMHSourceScanPerfScope::FMHSourceScanPerfScope(EMHPerfScanTrigger) {}
FMHSourceScanPerfScope::~FMHSourceScanPerfScope() = default;
void MHRecordSourceScanEnumeration(uint64, int32) {}
void MHRecordSourceScanPass() {}
void MHRecordSourceScanEnumeratedBytes(uint64) {}
void MHRecordSourceScanIOHash(uint64, bool) {}
void MHRecordSourceScanParse(EMHResourceKind, uint64) {}
void MHRecordSourceScanSQLite(uint64) {}
void MHRecordFullScanCompleted() {}

FMHReimportPerfScope::FMHReimportPerfScope() = default;
FMHReimportPerfScope::~FMHReimportPerfScope() = default;
void FMHReimportPerfScope::SetResourceKey(const FMHResourceKey&) {}
void FMHReimportPerfScope::AddAnalysisServicesCycles(uint64) {}
void FMHReimportPerfScope::AddImportBuildCycles(uint64) {}
void FMHReimportPerfScope::AddCompileWaitCycles(uint64) {}
void FMHReimportPerfScope::AddSavePackagesCycles(uint64) {}
void FMHReimportPerfScope::AddProjectionCycles(uint64) {}
void MHRecordReimportNotifiedResource(const FMHResourceKey&) {}
void MHRecordReimportActorRebuild(uint64) {}
} // namespace UE::MimirComposite
