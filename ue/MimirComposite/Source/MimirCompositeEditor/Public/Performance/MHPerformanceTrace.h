#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceResolver.h"

class AMHCompositeActor;

namespace UE::MimirComposite
{
struct FMHRandomSourceGraph;
struct FMHResolvedCompositePlan;

enum class EMHPerfScanTrigger : uint8
{
    Automatic,
    Startup,
    Manual,
    Commandlet
};

struct MIMIRCOMPOSITEEDITOR_API FMHMapLoadPerfReport
{
    uint64 CompositeActors = 0;
    uint64 RootCompositesUnique = 0;
    uint64 DefinitionCacheHits = 0;
    uint64 DefinitionCacheMisses = 0;
    uint64 AllOptionComposites = 0;
    uint64 AllOptionUniqueMeshes = 0;
    uint64 SelectedUniqueMeshes = 0;
    uint64 AllOptionMeshesCompiling = 0;
    uint64 SelectedMeshesCompiling = 0;
    // Unique meshes handed to the compilation wait step (R1: selected only).
    uint64 WaitedMeshes = 0;
    uint64 RegistryLookups = 0;
    uint64 AssetRegistryTagQueries = 0;
    uint64 PackageLoadsSync = 0;
    uint64 IdentityAdmissions = 0;
    uint64 LiveReceiptTagReads = 0;
    double BuildAppliedGraphMs = 0.0;
    double ResolveCompositePlanMs = 0.0;
    double LoadEndpointsMs = 0.0;
    double WaitStaticMeshCompilationMs = 0.0;
    double CompilePlacementMs = 0.0;
    double RegisterComponentsMs = 0.0;
    double DestroyRetiredComponentsMs = 0.0;
    uint64 ComponentsCreated = 0;
    uint64 ComponentsReused = 0;
    uint64 ComponentsDestroyed = 0;
    uint64 ISMBuckets = 0;
    uint64 ISMInstances = 0;
    double TotalMs = 0.0;
    uint64 EmittedReports = 0;
};

struct MIMIRCOMPOSITEEDITOR_API FMHStartupScanPerfReport
{
    FString Trigger;
    uint64 EnumeratedFiles = 0;
    uint64 EnumeratedBytes = 0;
    uint64 ScanPasses = 0;
    uint64 HashedFiles = 0;
    uint64 ParsedFbx = 0;
    uint64 ParsedMaterial = 0;
    uint64 ParsedComposite = 0;
    uint64 ParsedProfile = 0;
    double EnumerationMs = 0.0;
    double IOHashMs = 0.0;
    double ParseMs = 0.0;
    double SQLiteMs = 0.0;
    int64 FullScanCountDelta = 0;
    double TotalMs = 0.0;
    uint64 EmittedReports = 0;
};

struct MIMIRCOMPOSITEEDITOR_API FMHReimportPerfReport
{
    FString ResourceKey;
    int64 FullScanCountDelta = 0;
    uint64 IncrementalPaths = 0;
    double AnalysisServicesMs = 0.0;
    double ImportBuildMs = 0.0;
    double CompileWaitMs = 0.0;
    double SavePackagesMs = 0.0;
    double ProjectionMs = 0.0;
    uint64 NotifiedResourceKeys = 0;
    uint64 NotifiedActors = 0;
    double ActorRebuildMsTotal = 0.0;
    double TotalMs = 0.0;
    uint64 EmittedReports = 0;
};

MIMIRCOMPOSITEEDITOR_API int32 MHGetPerfTraceLevel();
MIMIRCOMPOSITEEDITOR_API bool MHIsSourceScanPerfActive();
MIMIRCOMPOSITEEDITOR_API bool MHIsReimportPerfActive();
MIMIRCOMPOSITEEDITOR_API void MHResetPerformanceTraceForTests();
MIMIRCOMPOSITEEDITOR_API FMHMapLoadPerfReport MHGetMapLoadPerfReportForTests();
MIMIRCOMPOSITEEDITOR_API FMHStartupScanPerfReport MHGetStartupScanPerfReportForTests();
MIMIRCOMPOSITEEDITOR_API FMHReimportPerfReport MHGetReimportPerfReportForTests();

class MIMIRCOMPOSITEEDITOR_API FMHMapLoadInitialBuildScope final
{
public:
    explicit FMHMapLoadInitialBuildScope(const AMHCompositeActor& Actor);
    ~FMHMapLoadInitialBuildScope();
    void Complete(const AMHCompositeActor& Actor);

    FMHMapLoadInitialBuildScope(const FMHMapLoadInitialBuildScope&) = delete;
    FMHMapLoadInitialBuildScope& operator=(const FMHMapLoadInitialBuildScope&) = delete;

private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};

MIMIRCOMPOSITEEDITOR_API void MHRecordMapLoadGraph(
    const FString& RootComposite,
    const FMHRandomSourceGraph& Graph);
MIMIRCOMPOSITEEDITOR_API void MHRecordMapLoadSelectedPlan(
    const FMHResolvedCompositePlan& Plan);
MIMIRCOMPOSITEEDITOR_API void MHRecordMapLoadCompilingMesh(
    const FMHResourceKey& Key,
    const UObject& Object);
MIMIRCOMPOSITEEDITOR_API void MHRecordMapLoadWaitedMesh(const FMHResourceKey& Key);
MIMIRCOMPOSITEEDITOR_API void MHFlushMapLoadPerfReport();

class MIMIRCOMPOSITEEDITOR_API FMHSourceScanPerfScope final
{
public:
    explicit FMHSourceScanPerfScope(EMHPerfScanTrigger Trigger);
    ~FMHSourceScanPerfScope();

    FMHSourceScanPerfScope(const FMHSourceScanPerfScope&) = delete;
    FMHSourceScanPerfScope& operator=(const FMHSourceScanPerfScope&) = delete;

private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};

MIMIRCOMPOSITEEDITOR_API void MHRecordSourceScanEnumeration(
    uint64 Cycles,
    int32 EnumeratedFiles);
MIMIRCOMPOSITEEDITOR_API void MHRecordSourceScanPass();
MIMIRCOMPOSITEEDITOR_API void MHRecordSourceScanEnumeratedBytes(uint64 Bytes);
MIMIRCOMPOSITEEDITOR_API void MHRecordSourceScanIOHash(uint64 Cycles, bool bHashed);
MIMIRCOMPOSITEEDITOR_API void MHRecordSourceScanParse(
    const FMHResourceKey& Key,
    const FString& SourcePath,
    uint64 Cycles);
MIMIRCOMPOSITEEDITOR_API void MHRecordSourceScanSQLite(uint64 Cycles);
MIMIRCOMPOSITEEDITOR_API void MHRecordFullScanCompleted();

class MIMIRCOMPOSITEEDITOR_API FMHReimportPerfScope final
{
public:
    FMHReimportPerfScope();
    ~FMHReimportPerfScope();
    bool IsActive() const { return Impl.IsValid(); }
    void SetResourceKey(const FMHResourceKey& Key);
    void AddAnalysisServicesCycles(uint64 Cycles);
    void AddImportBuildCycles(uint64 Cycles);
    void AddCompileWaitCycles(uint64 Cycles);
    void AddSavePackagesCycles(uint64 Cycles);
    void AddProjectionCycles(uint64 Cycles);

    FMHReimportPerfScope(const FMHReimportPerfScope&) = delete;
    FMHReimportPerfScope& operator=(const FMHReimportPerfScope&) = delete;

private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};

MIMIRCOMPOSITEEDITOR_API void MHRecordReimportNotifiedResource(
    const FMHResourceKey& Key);
MIMIRCOMPOSITEEDITOR_API void MHRecordReimportActorRebuild(
    const UObject& Actor,
    uint64 Cycles);
} // namespace UE::MimirComposite
