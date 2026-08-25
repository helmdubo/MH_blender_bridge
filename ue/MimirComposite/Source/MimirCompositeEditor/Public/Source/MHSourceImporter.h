#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Source/MHSourceAnalyzer.h"
#include "MHSourceImporter.generated.h"

class UMaterialInstanceConstant;
class UMHCompositeAsset;
class UMHCompositeSettings;
class UStaticMesh;
struct FFileChangeData;

namespace UE::MimirComposite
{

/** Empty ResourceKeys means the complete stable source snapshot. */
struct MIMIRCOMPOSITEEDITOR_API FMHImportSourcesScope
{
    TArray<FMHResourceKey> ResourceKeys;

    static FMHImportSourcesScope All() { return FMHImportSourcesScope(); }
};

/**
 * Filters an already-built plan without re-reading source files.
 */
MIMIRCOMPOSITEEDITOR_API void MHFilterAnalysisToScope(
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& InOutAnalysis);

/** Pure coordinator core: analyze/filter only, with no asset mutation. */
MIMIRCOMPOSITEEDITOR_API bool MHBuildSourceImportPlan(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted);

/** Full-scan coordinator with no Message Log, modal, subsystem or Slate use. */
MIMIRCOMPOSITEEDITOR_API bool MHImportSourcesHeadless(
    const FString& SourceRoot,
    const FMHImportSourcesScope& Scope,
    const UMHCompositeSettings& Settings,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted);

#if WITH_DEV_AUTOMATION_TESTS
/** Observes the coordinator's fixed texture -> material -> mesh -> composite stages. */
MIMIRCOMPOSITEEDITOR_API void MHSetImportStageObserverForTests(
    TFunction<void(EMHResourceKind)> Observer);
#endif

} // namespace UE::MimirComposite

/**
 * Source Protocol v4 editor import coordinator.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHSourceImporter final : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** Game-thread only. Executes texture -> material -> static mesh -> Composite. */
    bool ImportSources(
        const UE::MimirComposite::FMHImportSourcesScope& Scope,
        UE::MimirComposite::FMHSourceAnalysis& OutAnalysis,
        bool& bOutExecuted);

    /** Explicit source-wins mesh rebuild; bypasses equal-hash NO_CHANGE. */
    bool ReimportStaticMesh(
        UStaticMesh* StaticMesh,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Explicit source-wins material rebuild; reapplies the complete managed source document. */
    bool ReimportMaterial(
        UMaterialInstanceConstant* Material,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Manual file-drop adapter. The file must already be the unique source_root candidate. */
    bool ImportCompositeFile(
        const FString& Filename,
        const FString& TargetPackageName,
        UMHCompositeAsset*& OutAsset,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Explicit full-overwrite publish. Adopt folder/name are required for unmanaged MIs. */
    bool PublishMaterial(
        UMaterialInstanceConstant* Material,
        const FString& AdoptFolder,
        const FString& AdoptLogicalName,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Opens the S2 folder+name modal only when the MI has no source receipt. */
    bool PublishMaterialInteractive(
        UMaterialInstanceConstant* Material,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Explicit full-overwrite Composite publish; folder/name adopt unmanaged assets. */
    bool PublishComposite(
        UMHCompositeAsset* Asset,
        const FString& AdoptFolder,
        const FString& AdoptLogicalName,
        TArray<FString>& OutWarnings,
        FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
    void SetLifecycleTimeForTests(double TimeSeconds);
    void SetAssetRegistryReadyForTests(bool bReady);
    void SetPIEActiveForTests(bool bActive);
    void QueueSourcePathsForTests(const TArray<FString>& Paths, bool bRequestFullScan = false);
    void TickSourceLifecycleForTests();
    void SetBatchExecutorForTests(
        TFunction<bool(const TArray<FString>&, bool)> Executor);
    void SetStartupExecutorForTests(TFunction<bool()> Executor);
    int32 GetExecutedBatchCountForTests() const { return ExecutedBatchCountForTests; }
    int32 GetPendingPathCountForTests() const { return PendingSourcePaths.Num(); }
    bool HasStartupPlanRunForTests() const { return bStartupPlanRan; }
#endif

private:
    void OnAssetRegistryFilesLoaded();
    bool RunStartupPlan();
    void PresentPlan(const UE::MimirComposite::FMHSourceAnalysis& Analysis) const;
    bool TickSourceLifecycle(float DeltaSeconds);
    void OnBeginPIE(bool bIsSimulating);
    void OnEndPIE(bool bIsSimulating);
    void OnDirectoryChanged(const TArray<FFileChangeData>& FileChanges);
    void QueueSourcePaths(const TArray<FString>& Paths, bool bRequestFullScan);
    bool EnsureDirectoryWatcher(const FString& SourceRoot, FString& OutError);
    void StopDirectoryWatcher();
    void FlushPendingSourcePaths();
    bool ImportChangedSourcePaths(const TArray<FString>& Paths);
    double LifecycleNowSeconds() const;

    FDelegateHandle FilesLoadedHandle;
    FTSTicker::FDelegateHandle LifecycleTickerHandle;
    FDelegateHandle BeginPIEHandle;
    FDelegateHandle EndPIEHandle;
    FDelegateHandle DirectoryWatcherHandle;
    FString WatchedSourceRoot;
    TSet<FString> PendingSourcePaths;
    double LastSourceChangeSeconds = 0.0;
    bool bAssetRegistryReady = false;
    bool bStartupPlanRan = false;
    bool bPIEActive = false;
    bool bImportInProgress = false;
    bool bPendingFullScan = false;

#if WITH_DEV_AUTOMATION_TESTS
    TOptional<double> LifecycleTimeForTests;
    TFunction<bool(const TArray<FString>&, bool)> BatchExecutorForTests;
    TFunction<bool()> StartupExecutorForTests;
    int32 ExecutedBatchCountForTests = 0;
#endif
};
