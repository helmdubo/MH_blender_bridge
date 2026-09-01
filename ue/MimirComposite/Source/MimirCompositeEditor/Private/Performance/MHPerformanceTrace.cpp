#include "Performance/MHPerformanceTrace.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Random/MHRandomStream.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHPerformanceTrace, Log, All);

namespace
{
using namespace UE::MimirComposite;

TAutoConsoleVariable<int32> CVarMHPerfTrace(
    TEXT("mh.PerfTrace"),
    0,
    TEXT("Mimir performance trace verbosity: 0=off, 1=aggregates, 2=verbose."),
    ECVF_Default);

double CyclesToMilliseconds(const uint64 Cycles)
{
    return FPlatformTime::ToMilliseconds64(Cycles);
}

struct FMapLoadAccumulator
{
    FMHMapLoadPerfReport Values;
    TSet<FString> RootComposites;
    TSet<FString> CountedGraphs;
    TSet<FString> AllOptionComposites;
    TSet<FMHResourceKey> AllOptionMeshes;
    TSet<FMHResourceKey> SelectedMeshes;
    TSet<FMHResourceKey> CompilingMeshes;
    TMap<FMHResourceKey, FString> VerboseMeshPaths;
    int32 ActiveScopes = 0;
    bool bPending = false;
};

FMapLoadAccumulator GMapLoad;
FMHMapLoadPerfReport GLastMapLoadReport;
FMHStartupScanPerfReport GLastStartupScanReport;
FMHReimportPerfReport GLastReimportReport;
FTSTicker::FDelegateHandle GMapLoadFlushHandle;
uint64 GObservedFullScans = 0;

struct FSourceScanAccumulator
{
    FMHStartupScanPerfReport Values;
    uint64 StartCycles = 0;
    uint64 FullScansBefore = 0;
};

struct FReimportAccumulator
{
    FMHReimportPerfReport Values;
    uint64 StartCycles = 0;
    uint64 FullScansBefore = 0;
    TSet<FMHResourceKey> NotifiedKeys;
};

thread_local FSourceScanAccumulator* GActiveSourceScan = nullptr;
thread_local FReimportAccumulator* GActiveReimport = nullptr;

FString ScanTriggerLabel(EMHPerfScanTrigger Trigger)
{
    if (Trigger == EMHPerfScanTrigger::Automatic)
    {
        Trigger = IsRunningCommandlet()
            ? EMHPerfScanTrigger::Commandlet
            : EMHPerfScanTrigger::Manual;
    }
    switch (Trigger)
    {
    case EMHPerfScanTrigger::Startup: return TEXT("startup");
    case EMHPerfScanTrigger::Manual: return TEXT("manual");
    case EMHPerfScanTrigger::Commandlet: return TEXT("commandlet");
    case EMHPerfScanTrigger::Automatic: break;
    }
    return TEXT("manual");
}

void AddStageDelta(
    const FMHPlacementStageMetrics& Before,
    const FMHPlacementStageMetrics& After,
    const EMHPlacementStage Stage,
    double& OutMilliseconds)
{
    const uint64 BeforeCycles = Before.Get(Stage).InclusiveCycles;
    const uint64 AfterCycles = After.Get(Stage).InclusiveCycles;
    OutMilliseconds += CyclesToMilliseconds(AfterCycles - BeforeCycles);
}

void CollectGraphNodeMeshes(const FMHRandomNode& Node, TSet<FMHResourceKey>& OutMeshes)
{
    if (Node.Kind == EMHRandomSemanticKind::Mesh)
    {
        OutMeshes.Add({EMHResourceKind::StaticMesh, Node.Resource});
    }
    for (const FMHRandomOption& Option : Node.Options)
    {
        if (Option.Kind == EMHRandomSemanticKind::Mesh)
        {
            OutMeshes.Add({EMHResourceKind::StaticMesh, Option.Resource});
        }
    }
    for (const FMHRandomNode& Child : Node.Children)
    {
        CollectGraphNodeMeshes(Child, OutMeshes);
    }
}

void LogMapLoadReport(const FMHMapLoadPerfReport& Report)
{
    UE_LOG(LogMHPerformanceTrace, Display,
        TEXT("MH_PERF_MAPLOAD composite_actors=%llu root_composites_unique=%llu definition_cache_hits=%llu definition_cache_misses=%llu all_option_composites=%llu all_option_unique_meshes=%llu selected_unique_meshes=%llu all_option_meshes_compiling=%llu selected_meshes_compiling=%llu build_applied_graph_ms=%.3f resolve_composite_plan_ms=%.3f load_endpoints_ms=%.3f wait_static_mesh_compilation_ms=%.3f compile_placement_ms=%.3f register_components_ms=%.3f destroy_retired_components_ms=%.3f components_created=%llu components_reused=%llu components_destroyed=%llu ism_buckets=%llu ism_instances=%llu total_ms=%.3f"),
        Report.CompositeActors,
        Report.RootCompositesUnique,
        Report.DefinitionCacheHits,
        Report.DefinitionCacheMisses,
        Report.AllOptionComposites,
        Report.AllOptionUniqueMeshes,
        Report.SelectedUniqueMeshes,
        Report.AllOptionMeshesCompiling,
        Report.SelectedMeshesCompiling,
        Report.BuildAppliedGraphMs,
        Report.ResolveCompositePlanMs,
        Report.LoadEndpointsMs,
        Report.WaitStaticMeshCompilationMs,
        Report.CompilePlacementMs,
        Report.RegisterComponentsMs,
        Report.DestroyRetiredComponentsMs,
        Report.ComponentsCreated,
        Report.ComponentsReused,
        Report.ComponentsDestroyed,
        Report.ISMBuckets,
        Report.ISMInstances,
        Report.TotalMs);
}

void FlushMapLoadInternal()
{
    if (!GMapLoad.bPending || GMapLoad.ActiveScopes != 0)
    {
        return;
    }
    if (MHGetPerfTraceLevel() <= 0)
    {
        GMapLoad = FMapLoadAccumulator();
        return;
    }
    GMapLoad.Values.RootCompositesUnique = GMapLoad.RootComposites.Num();
    GMapLoad.Values.AllOptionComposites = GMapLoad.AllOptionComposites.Num();
    GMapLoad.Values.AllOptionUniqueMeshes = GMapLoad.AllOptionMeshes.Num();
    GMapLoad.Values.SelectedUniqueMeshes = GMapLoad.SelectedMeshes.Num();
    GMapLoad.Values.AllOptionMeshesCompiling = GMapLoad.CompilingMeshes.Num();
    uint64 SelectedCompiling = 0;
    for (const FMHResourceKey& Key : GMapLoad.SelectedMeshes)
    {
        if (GMapLoad.CompilingMeshes.Contains(Key))
        {
            ++SelectedCompiling;
        }
    }
    GMapLoad.Values.SelectedMeshesCompiling = SelectedCompiling;
    GMapLoad.Values.EmittedReports = 1;
    GLastMapLoadReport = GMapLoad.Values;
    LogMapLoadReport(GLastMapLoadReport);

    if (MHGetPerfTraceLevel() >= 2)
    {
        TArray<FString> Meshes;
        for (const FMHResourceKey& Key : GMapLoad.AllOptionMeshes)
        {
            const FString* Path = GMapLoad.VerboseMeshPaths.Find(Key);
            Meshes.Add(Path != nullptr
                ? Key.ToString() + TEXT("@") + *Path
                : Key.ToString());
        }
        Meshes.Sort();
        UE_LOG(LogMHPerformanceTrace, Display,
            TEXT("MH_PERF_MAPLOAD_VERBOSE all_option_meshes=[%s]"),
            *FString::Join(Meshes, TEXT(",")));
    }
    GMapLoad = FMapLoadAccumulator();
}

bool FlushMapLoadTicker(float)
{
    GMapLoadFlushHandle.Reset();
    FlushMapLoadInternal();
    return false;
}

void ScheduleMapLoadFlush()
{
    if (!GMapLoadFlushHandle.IsValid())
    {
        GMapLoadFlushHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateStatic(&FlushMapLoadTicker));
    }
}

void LogStartupScanReport(const FMHStartupScanPerfReport& Report)
{
    UE_LOG(LogMHPerformanceTrace, Display,
        TEXT("MH_PERF_STARTUP_SCAN trigger=%s enumerated_files=%llu enumerated_bytes=%llu scan_passes=%llu hashed_files=%llu parsed_fbx=%llu parsed_material=%llu parsed_composite=%llu parsed_profile=%llu enumeration_ms=%.3f io_hash_ms=%.3f parse_ms=%.3f sqlite_ms=%.3f full_scan_count_delta=%lld total_ms=%.3f"),
        *Report.Trigger,
        Report.EnumeratedFiles,
        Report.EnumeratedBytes,
        Report.ScanPasses,
        Report.HashedFiles,
        Report.ParsedFbx,
        Report.ParsedMaterial,
        Report.ParsedComposite,
        Report.ParsedProfile,
        Report.EnumerationMs,
        Report.IOHashMs,
        Report.ParseMs,
        Report.SQLiteMs,
        Report.FullScanCountDelta,
        Report.TotalMs);
}

void LogReimportReport(const FMHReimportPerfReport& Report)
{
    UE_LOG(LogMHPerformanceTrace, Display,
        TEXT("MH_PERF_REIMPORT resource_key=%s full_scan_count_delta=%lld incremental_paths=%llu analysis_services_ms=%.3f import_build_ms=%.3f compile_wait_ms=%.3f save_packages_ms=%.3f projection_ms=%.3f notified_resource_keys=%llu notified_actors=%llu actor_rebuild_ms_total=%.3f total_ms=%.3f"),
        *Report.ResourceKey,
        Report.FullScanCountDelta,
        Report.IncrementalPaths,
        Report.AnalysisServicesMs,
        Report.ImportBuildMs,
        Report.CompileWaitMs,
        Report.SavePackagesMs,
        Report.ProjectionMs,
        Report.NotifiedResourceKeys,
        Report.NotifiedActors,
        Report.ActorRebuildMsTotal,
        Report.TotalMs);
}
} // namespace

namespace UE::MimirComposite
{
struct FMHMapLoadInitialBuildScope::FImpl
{
    uint64 StartCycles = 0;
    FMHPlacementStageMetrics StagesBefore;
    FMHDefinitionCacheMetrics DefinitionsBefore;
    FMHPlacementMutationMetrics MutationsBefore;
    TSet<const UActorComponent*> PreviousComponents;
    bool bCompleted = false;
};

struct FMHSourceScanPerfScope::FImpl
{
    FSourceScanAccumulator Accumulator;
};

struct FMHReimportPerfScope::FImpl
{
    FReimportAccumulator Accumulator;
};

int32 MHGetPerfTraceLevel()
{
    return IsInGameThread()
        ? CVarMHPerfTrace.GetValueOnGameThread()
        : CVarMHPerfTrace.GetValueOnAnyThread();
}

bool MHIsSourceScanPerfActive()
{
    return GActiveSourceScan != nullptr;
}

bool MHIsReimportPerfActive()
{
    return GActiveReimport != nullptr;
}

void MHResetPerformanceTraceForTests()
{
    if (GMapLoadFlushHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(GMapLoadFlushHandle);
        GMapLoadFlushHandle.Reset();
    }
    GMapLoad = FMapLoadAccumulator();
    GLastMapLoadReport = FMHMapLoadPerfReport();
    GLastStartupScanReport = FMHStartupScanPerfReport();
    GLastReimportReport = FMHReimportPerfReport();
    GObservedFullScans = 0;
}

FMHMapLoadPerfReport MHGetMapLoadPerfReportForTests() { return GLastMapLoadReport; }
FMHStartupScanPerfReport MHGetStartupScanPerfReportForTests() { return GLastStartupScanReport; }
FMHReimportPerfReport MHGetReimportPerfReportForTests() { return GLastReimportReport; }

FMHMapLoadInitialBuildScope::FMHMapLoadInitialBuildScope(const AMHCompositeActor& Actor)
{
    if (MHGetPerfTraceLevel() <= 0)
    {
        return;
    }
    if (!GMapLoad.bPending)
    {
        GMapLoad = FMapLoadAccumulator();
        GMapLoad.bPending = true;
    }
    Impl = MakeUnique<FImpl>();
    Impl->StartCycles = FPlatformTime::Cycles64();
    Impl->StagesBefore = MHGetPlacementStageMetrics();
    Impl->DefinitionsBefore = MHGetDefinitionCacheMetrics();
    Impl->MutationsBefore = MHGetPlacementMutationMetrics();
    for (const UActorComponent* Component : Actor.GetDerivedComponents())
    {
        if (IsValid(Component))
        {
            Impl->PreviousComponents.Add(Component);
        }
    }
    for (const UActorComponent* Component : Actor.GetInstanceComponents())
    {
        if (!IsValid(Component) || Impl->PreviousComponents.Contains(Component))
        {
            continue;
        }
        for (const FName& Tag : Component->ComponentTags)
        {
            if (Tag.ToString().StartsWith(TEXT("MH.")))
            {
                Impl->PreviousComponents.Add(Component);
                break;
            }
        }
    }
    ++GMapLoad.ActiveScopes;
}

FMHMapLoadInitialBuildScope::~FMHMapLoadInitialBuildScope()
{
    if (Impl.IsValid() && !Impl->bCompleted)
    {
        --GMapLoad.ActiveScopes;
        ScheduleMapLoadFlush();
    }
}

void FMHMapLoadInitialBuildScope::Complete(const AMHCompositeActor& Actor)
{
    if (!Impl.IsValid() || Impl->bCompleted)
    {
        return;
    }
    Impl->bCompleted = true;
    const FMHPlacementStageMetrics StagesAfter = MHGetPlacementStageMetrics();
    const FMHDefinitionCacheMetrics DefinitionsAfter = MHGetDefinitionCacheMetrics();
    const FMHPlacementMutationMetrics MutationsAfter = MHGetPlacementMutationMetrics();
    ++GMapLoad.Values.CompositeActors;
    if (const UMHCompositeAsset* Asset = Actor.GetCompositeAsset())
    {
        GMapLoad.RootComposites.Add(Asset->LogicalName);
    }
    GMapLoad.Values.DefinitionCacheHits += DefinitionsAfter.Hits - Impl->DefinitionsBefore.Hits;
    GMapLoad.Values.DefinitionCacheMisses += DefinitionsAfter.Misses - Impl->DefinitionsBefore.Misses;
    AddStageDelta(Impl->StagesBefore, StagesAfter, EMHPlacementStage::BuildAppliedGraph,
        GMapLoad.Values.BuildAppliedGraphMs);
    AddStageDelta(Impl->StagesBefore, StagesAfter, EMHPlacementStage::ResolveCompositePlan,
        GMapLoad.Values.ResolveCompositePlanMs);
    AddStageDelta(Impl->StagesBefore, StagesAfter, EMHPlacementStage::LoadEndpoints,
        GMapLoad.Values.LoadEndpointsMs);
    AddStageDelta(Impl->StagesBefore, StagesAfter, EMHPlacementStage::WaitStaticMeshCompilation,
        GMapLoad.Values.WaitStaticMeshCompilationMs);
    AddStageDelta(Impl->StagesBefore, StagesAfter, EMHPlacementStage::CompilePlacement,
        GMapLoad.Values.CompilePlacementMs);
    AddStageDelta(Impl->StagesBefore, StagesAfter, EMHPlacementStage::RegisterComponents,
        GMapLoad.Values.RegisterComponentsMs);
    AddStageDelta(Impl->StagesBefore, StagesAfter, EMHPlacementStage::DestroyRetiredComponents,
        GMapLoad.Values.DestroyRetiredComponentsMs);
    GMapLoad.Values.ComponentsCreated +=
        MutationsAfter.CreatedComponents - Impl->MutationsBefore.CreatedComponents;
    GMapLoad.Values.ComponentsDestroyed +=
        MutationsAfter.DestroyedComponents - Impl->MutationsBefore.DestroyedComponents;
    for (const UActorComponent* Component : Actor.GetDerivedComponents())
    {
        if (!IsValid(Component))
        {
            continue;
        }
        if (Impl->PreviousComponents.Contains(Component))
        {
            ++GMapLoad.Values.ComponentsReused;
        }
        if (const UInstancedStaticMeshComponent* Bucket =
                Cast<UInstancedStaticMeshComponent>(Component))
        {
            ++GMapLoad.Values.ISMBuckets;
            GMapLoad.Values.ISMInstances += Bucket->GetInstanceCount();
        }
    }
    GMapLoad.Values.TotalMs +=
        CyclesToMilliseconds(FPlatformTime::Cycles64() - Impl->StartCycles);
    --GMapLoad.ActiveScopes;
    ScheduleMapLoadFlush();
}

void MHRecordMapLoadGraph(const FString& RootComposite, const FMHRandomSourceGraph& Graph)
{
    if (!GMapLoad.bPending || GMapLoad.ActiveScopes <= 0 ||
        GMapLoad.CountedGraphs.Contains(RootComposite))
    {
        return;
    }
    GMapLoad.CountedGraphs.Add(RootComposite);
    for (const TPair<FString, FMHRandomComposite>& Pair : Graph.Composites)
    {
        GMapLoad.AllOptionComposites.Add(Pair.Key);
        for (const FMHRandomNode& Node : Pair.Value.Nodes)
        {
            CollectGraphNodeMeshes(Node, GMapLoad.AllOptionMeshes);
        }
    }
}

void MHRecordMapLoadSelectedPlan(const FMHResolvedCompositePlan& Plan)
{
    if (!GMapLoad.bPending || GMapLoad.ActiveScopes <= 0)
    {
        return;
    }
    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
    {
        if (Leaf.Kind == EMHRandomSemanticKind::Mesh)
        {
            GMapLoad.SelectedMeshes.Add({EMHResourceKind::StaticMesh, Leaf.Resource});
        }
    }
}

void MHRecordMapLoadCompilingMesh(const FMHResourceKey& Key, const UObject& Object)
{
    if (!GMapLoad.bPending || GMapLoad.ActiveScopes <= 0)
    {
        return;
    }
    GMapLoad.CompilingMeshes.Add(Key);
    if (MHGetPerfTraceLevel() >= 2)
    {
        GMapLoad.VerboseMeshPaths.Add(Key, Object.GetPathName());
    }
}

void MHFlushMapLoadPerfReport()
{
    if (GMapLoadFlushHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(GMapLoadFlushHandle);
        GMapLoadFlushHandle.Reset();
    }
    FlushMapLoadInternal();
}

FMHSourceScanPerfScope::FMHSourceScanPerfScope(const EMHPerfScanTrigger Trigger)
{
    if (MHGetPerfTraceLevel() <= 0)
    {
        return;
    }
    check(GActiveSourceScan == nullptr);
    Impl = MakeUnique<FImpl>();
    Impl->Accumulator.Values.Trigger = ScanTriggerLabel(Trigger);
    Impl->Accumulator.StartCycles = FPlatformTime::Cycles64();
    Impl->Accumulator.FullScansBefore = GObservedFullScans;
    GActiveSourceScan = &Impl->Accumulator;
}

FMHSourceScanPerfScope::~FMHSourceScanPerfScope()
{
    if (!Impl.IsValid())
    {
        return;
    }
    check(GActiveSourceScan == &Impl->Accumulator);
    GActiveSourceScan = nullptr;
    FMHStartupScanPerfReport& Report = Impl->Accumulator.Values;
    Report.FullScanCountDelta = static_cast<int64>(GObservedFullScans) -
        static_cast<int64>(Impl->Accumulator.FullScansBefore);
    Report.TotalMs = CyclesToMilliseconds(
        FPlatformTime::Cycles64() - Impl->Accumulator.StartCycles);
    Report.EmittedReports = 1;
    GLastStartupScanReport = Report;
    LogStartupScanReport(GLastStartupScanReport);
}

void MHRecordSourceScanEnumeration(const uint64 Cycles, const int32 EnumeratedFiles)
{
    if (GActiveSourceScan == nullptr)
    {
        return;
    }
    GActiveSourceScan->Values.EnumerationMs += CyclesToMilliseconds(Cycles);
    GActiveSourceScan->Values.EnumeratedFiles = FMath::Max<uint64>(
        GActiveSourceScan->Values.EnumeratedFiles,
        static_cast<uint64>(FMath::Max(0, EnumeratedFiles)));
}

void MHRecordSourceScanPass()
{
    if (GActiveSourceScan != nullptr) ++GActiveSourceScan->Values.ScanPasses;
}

void MHRecordSourceScanEnumeratedBytes(const uint64 Bytes)
{
    if (GActiveSourceScan != nullptr) GActiveSourceScan->Values.EnumeratedBytes += Bytes;
}

void MHRecordSourceScanIOHash(const uint64 Cycles, const bool bHashed)
{
    if (GActiveSourceScan == nullptr)
    {
        return;
    }
    GActiveSourceScan->Values.IOHashMs += CyclesToMilliseconds(Cycles);
    if (bHashed) ++GActiveSourceScan->Values.HashedFiles;
}

void MHRecordSourceScanParse(const EMHResourceKind Kind, const uint64 Cycles)
{
    if (GActiveSourceScan == nullptr)
    {
        return;
    }
    GActiveSourceScan->Values.ParseMs += CyclesToMilliseconds(Cycles);
    switch (Kind)
    {
    case EMHResourceKind::StaticMesh: ++GActiveSourceScan->Values.ParsedFbx; break;
    case EMHResourceKind::Material: ++GActiveSourceScan->Values.ParsedMaterial; break;
    case EMHResourceKind::Composite: ++GActiveSourceScan->Values.ParsedComposite; break;
    case EMHResourceKind::PlacementProfile: ++GActiveSourceScan->Values.ParsedProfile; break;
    case EMHResourceKind::Texture: break;
    }
}

void MHRecordSourceScanSQLite(const uint64 Cycles)
{
    if (GActiveSourceScan != nullptr)
        GActiveSourceScan->Values.SQLiteMs += CyclesToMilliseconds(Cycles);
}

void MHRecordFullScanCompleted()
{
    if (MHGetPerfTraceLevel() > 0) ++GObservedFullScans;
}

FMHReimportPerfScope::FMHReimportPerfScope()
{
    if (MHGetPerfTraceLevel() <= 0)
    {
        return;
    }
    check(GActiveReimport == nullptr);
    Impl = MakeUnique<FImpl>();
    Impl->Accumulator.StartCycles = FPlatformTime::Cycles64();
    Impl->Accumulator.FullScansBefore = GObservedFullScans;
    GActiveReimport = &Impl->Accumulator;
}

FMHReimportPerfScope::~FMHReimportPerfScope()
{
    if (!Impl.IsValid())
    {
        return;
    }
    check(GActiveReimport == &Impl->Accumulator);
    GActiveReimport = nullptr;
    FMHReimportPerfReport& Report = Impl->Accumulator.Values;
    Report.FullScanCountDelta = static_cast<int64>(GObservedFullScans) -
        static_cast<int64>(Impl->Accumulator.FullScansBefore);
    Report.NotifiedResourceKeys = Impl->Accumulator.NotifiedKeys.Num();
    Report.TotalMs = CyclesToMilliseconds(
        FPlatformTime::Cycles64() - Impl->Accumulator.StartCycles);
    Report.EmittedReports = 1;
    GLastReimportReport = Report;
    LogReimportReport(GLastReimportReport);
}

void FMHReimportPerfScope::SetResourceKey(const FMHResourceKey& Key)
{
    if (Impl.IsValid()) Impl->Accumulator.Values.ResourceKey = Key.ToString();
}

void FMHReimportPerfScope::AddAnalysisServicesCycles(const uint64 Cycles)
{
    if (Impl.IsValid()) Impl->Accumulator.Values.AnalysisServicesMs += CyclesToMilliseconds(Cycles);
}

void FMHReimportPerfScope::AddImportBuildCycles(const uint64 Cycles)
{
    if (Impl.IsValid()) Impl->Accumulator.Values.ImportBuildMs += CyclesToMilliseconds(Cycles);
}

void FMHReimportPerfScope::AddCompileWaitCycles(const uint64 Cycles)
{
    if (Impl.IsValid()) Impl->Accumulator.Values.CompileWaitMs += CyclesToMilliseconds(Cycles);
}

void FMHReimportPerfScope::AddSavePackagesCycles(const uint64 Cycles)
{
    if (Impl.IsValid()) Impl->Accumulator.Values.SavePackagesMs += CyclesToMilliseconds(Cycles);
}

void FMHReimportPerfScope::AddProjectionCycles(const uint64 Cycles)
{
    if (Impl.IsValid()) Impl->Accumulator.Values.ProjectionMs += CyclesToMilliseconds(Cycles);
}

void MHRecordReimportNotifiedResource(const FMHResourceKey& Key)
{
    if (GActiveReimport != nullptr) GActiveReimport->NotifiedKeys.Add(Key);
}

void MHRecordReimportActorRebuild(const uint64 Cycles)
{
    if (GActiveReimport != nullptr)
    {
        ++GActiveReimport->Values.NotifiedActors;
        GActiveReimport->Values.ActorRebuildMsTotal += CyclesToMilliseconds(Cycles);
    }
}
} // namespace UE::MimirComposite
