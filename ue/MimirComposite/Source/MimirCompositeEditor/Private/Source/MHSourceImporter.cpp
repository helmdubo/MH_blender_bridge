#include "Source/MHSourceImporter.h"

#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeImporter.h"
#include "Containers/Ticker.h"
#include "DirectoryWatcherModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "IDirectoryWatcher.h"
#include "Logging/MessageLog.h"
#include "MessageLogModule.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialAdoptDialog.h"
#include "Material/MHMaterialProtocol.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMesh/MHStaticMeshImporter.h"
#include "Texture/MHTextureImporter.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHSourceImporter)

namespace UE::MimirComposite
{
namespace
{

#if WITH_DEV_AUTOMATION_TESTS
TFunction<void(EMHResourceKind)> GImportStageObserverForTests;
#endif

void ObserveImportStage(const EMHResourceKind Kind)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (GImportStageObserverForTests)
    {
        GImportStageObserverForTests(Kind);
    }
#else
    (void)Kind;
#endif
}

bool ShouldExecuteEntry(const FMHSourceAnalysisEntry& Entry)
{
    return Entry.Errors.IsEmpty() &&
        (Entry.Change == EMHSourceChange::Create ||
         Entry.Change == EMHSourceChange::Reimport ||
         Entry.Change == EMHSourceChange::Move);
}

bool MaterialReferencesFailedTexture(
    const FMHSourceAnalysisEntry& Entry,
    const TSet<FString>& FailedTextures,
    FString& OutTexture)
{
    OutTexture.Reset();
    if (FailedTextures.IsEmpty())
    {
        return false;
    }
    TArray<uint8> Bytes;
    FMHMaterialDocument Document;
    FString Error;
    if (!FFileHelper::LoadFileToArray(Bytes, *Entry.PayloadPath) ||
        !MHParseMaterialV4(Bytes, Document, Error))
    {
        return false;
    }
    for (const TPair<int32, FString>& Texture : Document.Textures)
    {
        if (FailedTextures.Contains(Texture.Value))
        {
            OutTexture = Texture.Value;
            return true;
        }
    }
    return false;
}

bool ExecutePreparedSourceImports(
    const FString& SourceRoot,
    const FMHImportSourcesScope& Scope,
    const UMHCompositeSettings& Settings,
    FMHSourceAnalysisServices& Services,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted)
{
    MHBuildSourceImportPlan(
        *Services.ChangeDetector,
        *Services.Resolver,
        SourceRoot,
        Scope,
        OutAnalysis,
        bOutExecuted);

    TSet<FString> FailedTextures;
    ObserveImportStage(EMHResourceKind::Texture);
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::Texture || !ShouldExecuteEntry(Entry))
        {
            continue;
        }
        FMHTextureOperationResult TextureResult = MHEnsureTextureV4(
            Entry,
            SourceRoot,
            true);
        Entry.Warnings.Append(TextureResult.Warnings);
        if (!TextureResult.Succeeded())
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(TextureResult.Error);
            FailedTextures.Add(Entry.Key.LogicalName);
        }
        else
        {
            bOutExecuted |= TextureResult.bImported;
        }
    }

    ObserveImportStage(EMHResourceKind::Material);
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::Material || !ShouldExecuteEntry(Entry))
        {
            continue;
        }
        FString FailedTexture;
        if (MaterialReferencesFailedTexture(Entry, FailedTextures, FailedTexture))
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: material:%s depends on failed texture:%s"),
                *Entry.Key.LogicalName,
                *FailedTexture));
            continue;
        }
        FMHMaterialOperationResult MaterialResult = MHImportMaterialV4(
            Entry,
            *Services.Resolver,
            SourceRoot,
            Settings);
        Entry.Warnings.Append(MaterialResult.Warnings);
        if (!MaterialResult.Succeeded())
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(MaterialResult.Error);
        }
        else
        {
            bOutExecuted = true;
        }
    }

    ObserveImportStage(EMHResourceKind::StaticMesh);
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::StaticMesh || !Entry.Errors.IsEmpty())
        {
            continue;
        }
        if (Entry.Change == EMHSourceChange::NoChange)
        {
            const FString PackageName = FString(TEXT("/Game/MH/Generated/Meshes/")) +
                Entry.Key.LogicalName;
            const FString ObjectPath = PackageName + TEXT(".") + Entry.Key.LogicalName;
            if (const UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath))
            {
                const UMHStaticMeshImportData* Data = Cast<UMHStaticMeshImportData>(
                    Mesh->GetAssetImportData());
                if (Data != nullptr && Data->ImporterVersion != MHStaticMeshImporterVersion)
                {
                    Entry.Change = EMHSourceChange::Reimport;
                }
            }
        }
        if (!ShouldExecuteEntry(Entry))
        {
            continue;
        }
        FMHStaticMeshOperationResult MeshResult = MHImportStaticMeshV4(
            Entry,
            *Services.Resolver,
            SourceRoot);
        Entry.Warnings.Append(MeshResult.Warnings);
        if (!MeshResult.Succeeded())
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(MeshResult.Error);
        }
        else
        {
            bOutExecuted |= MeshResult.bRebuilt || MeshResult.bReceiptUpdated;
        }
    }

    ObserveImportStage(EMHResourceKind::Composite);
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::Composite || !ShouldExecuteEntry(Entry))
        {
            continue;
        }
        FMHCompositeOperationResult CompositeResult = MHImportCompositeV4(
            Entry,
            *Services.Resolver,
            SourceRoot,
            Settings);
        Entry.Warnings.Append(CompositeResult.Warnings);
        if (!CompositeResult.Succeeded())
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(CompositeResult.Error);
        }
        else
        {
            bOutExecuted = true;
        }
    }
    return !OutAnalysis.HasErrors();
}

} // namespace

void MHFilterAnalysisToScope(
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& InOutAnalysis)
{
    if (Scope.ResourceKeys.IsEmpty())
    {
        return;
    }

    TSet<FMHResourceKey> Analyzed;
    for (const FMHSourceAnalysisEntry& Entry : InOutAnalysis.Entries)
    {
        Analyzed.Add(Entry.Key);
    }

    TSet<FMHResourceKey> Included;
    TArray<FMHResourceKey> Invalid;
    for (const FMHResourceKey& Key : Scope.ResourceKeys)
    {
        if (Key.IsCanonical())
        {
            Included.Add(Key);
        }
        else
        {
            Invalid.Add(Key);
        }
    }
    InOutAnalysis.Entries.RemoveAll(
        [&Included](const FMHSourceAnalysisEntry& Entry)
        {
            return !Included.Contains(Entry.Key);
        });

    for (const FMHResourceKey& Key : Included)
    {
        if (Analyzed.Contains(Key))
        {
            continue;
        }

        FMHSourceAnalysisEntry& Missing = InOutAnalysis.Entries.AddDefaulted_GetRef();
        Missing.Key = Key;
        Missing.Change = EMHSourceChange::Blocked;
        Missing.Errors.Add(FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: requested scope key %s was not found in the source snapshot or applied state"),
            *Key.ToString()));
    }
    for (const FMHResourceKey& Key : Invalid)
    {
        FMHSourceAnalysisEntry& Rejected = InOutAnalysis.Entries.AddDefaulted_GetRef();
        Rejected.Key = Key;
        Rejected.Change = EMHSourceChange::Blocked;
        Rejected.Errors.Add(FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: requested scope key is not canonical: %s"),
            *Key.ToString()));
    }
    InOutAnalysis.Entries.Sort([](
        const FMHSourceAnalysisEntry& A,
        const FMHSourceAnalysisEntry& B)
    {
        if (A.Key.Kind != B.Key.Kind)
        {
            return static_cast<uint8>(A.Key.Kind) < static_cast<uint8>(B.Key.Kind);
        }
        return A.Key.LogicalName < B.Key.LogicalName;
    });
}

bool MHBuildSourceImportPlan(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted)
{
    OutAnalysis = FMHSourceAnalysis();
    bOutExecuted = false;
    MHAnalyzeSources(ChangeDetector, Resolver, SourceRoot, OutAnalysis);
    MHFilterAnalysisToScope(Scope, OutAnalysis);
    return !OutAnalysis.HasErrors();
}

bool MHImportSourcesHeadless(
    const FString& SourceRoot,
    const FMHImportSourcesScope& Scope,
    const UMHCompositeSettings& Settings,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted)
{
    OutAnalysis = FMHSourceAnalysis();
    bOutExecuted = false;
    if (!IsInGameThread())
    {
        OutAnalysis.Errors.Add(
            TEXT("MH_E_IMPORT_THREAD_INVALID: MHImportSourcesHeadless must run on the game thread"));
        return false;
    }
    if (SourceRoot.IsEmpty())
    {
        OutAnalysis.Errors.Add(TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured"));
        return false;
    }

    FMHSourceAnalysisServices Services;
    FString Error;
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, Services, Error))
    {
        OutAnalysis.Errors.Add(MoveTemp(Error));
        return false;
    }
    return ExecutePreparedSourceImports(
        SourceRoot,
        Scope,
        Settings,
        Services,
        OutAnalysis,
        bOutExecuted);
}

#if WITH_DEV_AUTOMATION_TESTS
void MHSetImportStageObserverForTests(TFunction<void(EMHResourceKind)> Observer)
{
    GImportStageObserverForTests = MoveTemp(Observer);
}
#endif

} // namespace UE::MimirComposite

using namespace UE::MimirComposite;

void UMHSourceImporter::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    if (IsRunningCommandlet())
    {
        return;
    }

    LifecycleTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UMHSourceImporter::TickSourceLifecycle));
    BeginPIEHandle = FEditorDelegates::BeginPIE.AddUObject(
        this,
        &UMHSourceImporter::OnBeginPIE);
    EndPIEHandle = FEditorDelegates::EndPIE.AddUObject(
        this,
        &UMHSourceImporter::OnEndPIE);

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (AssetRegistry.IsLoadingAssets())
    {
        FilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddUObject(
            this,
            &UMHSourceImporter::OnAssetRegistryFilesLoaded);
    }
    else
    {
        OnAssetRegistryFilesLoaded();
    }
}

void UMHSourceImporter::Deinitialize()
{
    StopDirectoryWatcher();
    if (LifecycleTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(LifecycleTickerHandle);
        LifecycleTickerHandle.Reset();
    }
    if (BeginPIEHandle.IsValid())
    {
        FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
        BeginPIEHandle.Reset();
    }
    if (EndPIEHandle.IsValid())
    {
        FEditorDelegates::EndPIE.Remove(EndPIEHandle);
        EndPIEHandle.Reset();
    }
    if (FilesLoadedHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
    {
        FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get()
            .OnFilesLoaded()
            .Remove(FilesLoadedHandle);
        FilesLoadedHandle.Reset();
    }
    PendingSourcePaths.Reset();
    bPendingFullScan = false;
    Super::Deinitialize();
}

void UMHSourceImporter::OnAssetRegistryFilesLoaded()
{
    if (FilesLoadedHandle.IsValid())
    {
        FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get()
            .OnFilesLoaded()
            .Remove(FilesLoadedHandle);
        FilesLoadedHandle.Reset();
    }
    TWeakObjectPtr<UMHSourceImporter> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis]()
    {
        if (WeakThis.IsValid())
        {
            WeakThis->bAssetRegistryReady = true;
            WeakThis->RunStartupPlan();
        }
    });
}

bool UMHSourceImporter::RunStartupPlan()
{
    if (bStartupPlanRan || !bAssetRegistryReady || bImportInProgress)
    {
        return bStartupPlanRan;
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (StartupExecutorForTests)
    {
        const bool bAttempted = StartupExecutorForTests();
        bStartupPlanRan = bAttempted;
        return bAttempted;
    }
#endif

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || Settings->GetSourceRootPath().IsEmpty())
    {
        return false;
    }

    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    bImportInProgress = true;
    ImportSources(FMHImportSourcesScope::All(), Analysis, bExecuted);
    bImportInProgress = false;
    // A configured startup root is attempted exactly once. Validation failures
    // remain visible in the plan; subsequent source fixes arrive via watcher.
    bStartupPlanRan = true;
    return true;
}

bool UMHSourceImporter::ImportSources(
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted)
{
    OutAnalysis = FMHSourceAnalysis();
    bOutExecuted = false;
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (Settings == nullptr)
    {
        OutAnalysis.Errors.Add(TEXT("MH_E_SOURCE_INDEX_INVALID: Mimir settings are unavailable"));
        PresentPlan(OutAnalysis);
        return false;
    }
    const bool bSucceeded = Settings != nullptr && MHImportSourcesHeadless(
        SourceRoot,
        Scope,
        *Settings,
        OutAnalysis,
        bOutExecuted);
    FString WatchError;
    if (!SourceRoot.IsEmpty() && !EnsureDirectoryWatcher(SourceRoot, WatchError))
    {
        OutAnalysis.Errors.Add(MoveTemp(WatchError));
    }
    PresentPlan(OutAnalysis);
    return bSucceeded && !OutAnalysis.HasErrors();
}

double UMHSourceImporter::LifecycleNowSeconds() const
{
#if WITH_DEV_AUTOMATION_TESTS
    if (LifecycleTimeForTests.IsSet())
    {
        return LifecycleTimeForTests.GetValue();
    }
#endif
    return FPlatformTime::Seconds();
}

bool UMHSourceImporter::TickSourceLifecycle(const float DeltaSeconds)
{
    (void)DeltaSeconds;
    if (bAssetRegistryReady && !bStartupPlanRan)
    {
        RunStartupPlan();
    }
    if (!bPIEActive &&
        !bImportInProgress &&
        (bPendingFullScan || !PendingSourcePaths.IsEmpty()) &&
        LifecycleNowSeconds() - LastSourceChangeSeconds >= 1.0)
    {
        FlushPendingSourcePaths();
    }
    return true;
}

void UMHSourceImporter::OnBeginPIE(const bool bIsSimulating)
{
    (void)bIsSimulating;
    bPIEActive = true;
}

void UMHSourceImporter::OnEndPIE(const bool bIsSimulating)
{
    (void)bIsSimulating;
    bPIEActive = false;
    TickSourceLifecycle(0.0f);
}

void UMHSourceImporter::OnDirectoryChanged(
    const TArray<FFileChangeData>& FileChanges)
{
    TArray<FString> Paths;
    bool bRequestFullScan = false;
    for (const FFileChangeData& Change : FileChanges)
    {
        if (Change.Action == FFileChangeData::FCA_RescanRequired)
        {
            bRequestFullScan = true;
        }
        else if (!Change.Filename.IsEmpty())
        {
            Paths.Add(Change.Filename);
        }
    }

    if (IsInGameThread())
    {
        QueueSourcePaths(Paths, bRequestFullScan);
        return;
    }
    TWeakObjectPtr<UMHSourceImporter> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis, Paths = MoveTemp(Paths), bRequestFullScan]()
    {
        if (WeakThis.IsValid())
        {
            WeakThis->QueueSourcePaths(Paths, bRequestFullScan);
        }
    });
}

void UMHSourceImporter::QueueSourcePaths(
    const TArray<FString>& Paths,
    const bool bRequestFullScan)
{
    for (const FString& Path : Paths)
    {
        if (Path.IsEmpty())
        {
            continue;
        }
        FString Absolute = FPaths::ConvertRelativePathToFull(Path);
        FPaths::NormalizeFilename(Absolute);
        PendingSourcePaths.Add(MoveTemp(Absolute));
    }
    bPendingFullScan |= bRequestFullScan;
    if (bRequestFullScan || !Paths.IsEmpty())
    {
        LastSourceChangeSeconds = LifecycleNowSeconds();
    }
}

bool UMHSourceImporter::EnsureDirectoryWatcher(
    const FString& SourceRoot,
    FString& OutError)
{
    OutError.Reset();
    FString NormalizedRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(NormalizedRoot);
    if (NormalizedRoot.IsEmpty() || !FPaths::DirectoryExists(NormalizedRoot))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: source_root directory does not exist: %s"),
            *NormalizedRoot);
        return false;
    }
    if (DirectoryWatcherHandle.IsValid() &&
        FPaths::IsSamePath(WatchedSourceRoot, NormalizedRoot))
    {
        return true;
    }

    StopDirectoryWatcher();
    FDirectoryWatcherModule& Module =
        FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
    IDirectoryWatcher* Watcher = Module.Get();
    if (Watcher == nullptr ||
        !Watcher->RegisterDirectoryChangedCallback_Handle(
            NormalizedRoot,
            IDirectoryWatcher::FDirectoryChanged::CreateUObject(
                this,
                &UMHSourceImporter::OnDirectoryChanged),
            DirectoryWatcherHandle))
    {
        DirectoryWatcherHandle.Reset();
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: cannot watch source_root: %s"),
            *NormalizedRoot);
        return false;
    }
    WatchedSourceRoot = MoveTemp(NormalizedRoot);
    return true;
}

void UMHSourceImporter::StopDirectoryWatcher()
{
    if (DirectoryWatcherHandle.IsValid() &&
        FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
    {
        FDirectoryWatcherModule& Module =
            FModuleManager::GetModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
        if (IDirectoryWatcher* Watcher = Module.Get())
        {
            Watcher->UnregisterDirectoryChangedCallback_Handle(
                WatchedSourceRoot,
                DirectoryWatcherHandle);
        }
    }
    DirectoryWatcherHandle.Reset();
    WatchedSourceRoot.Reset();
}

bool UMHSourceImporter::ImportChangedSourcePaths(const TArray<FString>& Paths)
{
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    if (Settings == nullptr || SourceRoot.IsEmpty())
    {
        Analysis.Errors.Add(TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured"));
        PresentPlan(Analysis);
        return false;
    }

    FMHSourceAnalysisServices Services;
    FMHProjectIndexUpdateResult Update;
    bool bUsedFullScan = false;
    FString Error;
    if (!MHCreateIncrementalSourceAnalysisServices(
            SourceRoot,
            Paths,
            Services,
            Update,
            bUsedFullScan,
            Error))
    {
        Analysis.Errors.Add(MoveTemp(Error));
        PresentPlan(Analysis);
        return false;
    }

    const bool bSucceeded = ExecutePreparedSourceImports(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        Services,
        Analysis,
        bExecuted);
    FMessageLog Log(TEXT("Mimir"));
    for (const FString& Event : Update.SessionEvents)
    {
        Log.Info(FText::FromString(Event));
    }
    if (bUsedFullScan)
    {
        Log.Info(INVTEXT("Project index was recreated; watcher batch used one full projection"));
    }
    PresentPlan(Analysis);
    return bSucceeded;
}

void UMHSourceImporter::FlushPendingSourcePaths()
{
    if (bPIEActive || bImportInProgress ||
        (!bPendingFullScan && PendingSourcePaths.IsEmpty()))
    {
        return;
    }

    TArray<FString> Paths = PendingSourcePaths.Array();
    Paths.Sort();
    const bool bRunFullScan = bPendingFullScan;
    PendingSourcePaths.Reset();
    bPendingFullScan = false;
    bImportInProgress = true;

#if WITH_DEV_AUTOMATION_TESTS
    if (BatchExecutorForTests)
    {
        BatchExecutorForTests(Paths, bRunFullScan);
        ++ExecutedBatchCountForTests;
        bImportInProgress = false;
        return;
    }
#endif

    if (bRunFullScan)
    {
        FMHSourceAnalysis Analysis;
        bool bExecuted = false;
        ImportSources(FMHImportSourcesScope::All(), Analysis, bExecuted);
    }
    else
    {
        ImportChangedSourcePaths(Paths);
    }
    bImportInProgress = false;
}

#if WITH_DEV_AUTOMATION_TESTS
void UMHSourceImporter::SetLifecycleTimeForTests(const double TimeSeconds)
{
    LifecycleTimeForTests = TimeSeconds;
}

void UMHSourceImporter::SetAssetRegistryReadyForTests(const bool bReady)
{
    bAssetRegistryReady = bReady;
}

void UMHSourceImporter::SetPIEActiveForTests(const bool bActive)
{
    bPIEActive = bActive;
    if (!bPIEActive)
    {
        TickSourceLifecycle(0.0f);
    }
}

void UMHSourceImporter::QueueSourcePathsForTests(
    const TArray<FString>& Paths,
    const bool bRequestFullScan)
{
    QueueSourcePaths(Paths, bRequestFullScan);
}

void UMHSourceImporter::TickSourceLifecycleForTests()
{
    TickSourceLifecycle(0.0f);
}

void UMHSourceImporter::SetBatchExecutorForTests(
    TFunction<bool(const TArray<FString>&, bool)> Executor)
{
    BatchExecutorForTests = MoveTemp(Executor);
}

void UMHSourceImporter::SetStartupExecutorForTests(TFunction<bool()> Executor)
{
    StartupExecutorForTests = MoveTemp(Executor);
}
#endif

bool UMHSourceImporter::ReimportStaticMesh(
    UStaticMesh* StaticMesh,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || StaticMesh == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: ReimportStaticMesh requires a mesh on the game thread");
        return false;
    }
    const UMHStaticMeshImportData* Data = Cast<UMHStaticMeshImportData>(StaticMesh->GetAssetImportData());
    if (Data == nullptr || Data->LogicalName.IsEmpty() || Data->SourceRelativePath.IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: static mesh has no v4 source receipt");
        return false;
    }
    FMHResourceKey MeshKey;
    MeshKey.Kind = EMHResourceKind::StaticMesh;
    MeshKey.LogicalName = Data->LogicalName;
    if (!MeshKey.IsCanonical())
    {
        OutError = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: managed mesh receipt has a noncanonical logical name");
        return false;
    }
    const FString ExpectedPackageName = FString(TEXT("/Game/MH/Generated/Meshes/")) + MeshKey.LogicalName;
    const FString ExpectedObjectPath = ExpectedPackageName + TEXT(".") + MeshKey.LogicalName;
    if (StaticMesh->GetPathName() != ExpectedObjectPath ||
        StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *ExpectedObjectPath) != StaticMesh)
    {
        OutError = TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: explicit reimport target is not the canonical managed mesh UObject");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (SourceRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }

    FMHSourceAnalysisServices Services;
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, Services, OutError))
    {
        return false;
    }
    FMHSourceAnalysisEntry Entry;
    Entry.Key = MoveTemp(MeshKey);
    const FMHResolveOutcome Outcome = Services.Resolver->Resolve(Entry.Key);
    if (Outcome.Status != EMHResolveStatus::Resolved)
    {
        OutError = Outcome.Diagnostic.IsEmpty()
            ? TEXT("MH_E_INVALID_RESOURCE_SOURCE: managed mesh source does not resolve")
            : Outcome.Diagnostic;
        return false;
    }
    Entry.PayloadPath = Outcome.PayloadPath;
    FString RelativeBase = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(RelativeBase);
    RelativeBase += TEXT("/");
    Entry.SourcePath = Outcome.PayloadPath;
    if (!FPaths::MakePathRelativeTo(Entry.SourcePath, *RelativeBase))
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: cannot derive the current mesh source path");
        return false;
    }
    FPaths::NormalizeFilename(Entry.SourcePath);
    Entry.RawHash = Outcome.RawHash;
    Entry.Change = EMHSourceChange::Reimport;
    FMHStaticMeshOperationResult Result = MHImportStaticMeshV4(
        Entry,
        *Services.Resolver,
        SourceRoot,
        true);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    if (!Result.Succeeded())
    {
        return false;
    }
    if (Result.StaticMesh != StaticMesh)
    {
        OutError = TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: explicit reimport resolved a different managed UObject");
        return false;
    }
    return Result.bRebuilt;
}

bool UMHSourceImporter::ReimportMaterial(
    UMaterialInstanceConstant* Material,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || Material == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: ReimportMaterial requires a material instance on the game thread");
        return false;
    }

    const UMHMaterialSourceData* Data = Cast<UMHMaterialSourceData>(
        Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    if (Data == nullptr || Data->LogicalName.IsEmpty() || Data->SourceRelativePath.IsEmpty())
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: material '%s' has no v4 source receipt"),
            *Material->GetPathName());
        return false;
    }

    FMHResourceKey MaterialKey;
    MaterialKey.Kind = EMHResourceKind::Material;
    MaterialKey.LogicalName = Data->LogicalName;
    if (!MaterialKey.IsCanonical())
    {
        OutError = FString::Printf(
            TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: managed material '%s' has noncanonical receipt name '%s'"),
            *Material->GetPathName(),
            *Data->LogicalName);
        return false;
    }

    const FString ExpectedPackageName = FString(TEXT("/Game/MH/Generated/Materials/")) + MaterialKey.LogicalName;
    const FString ExpectedObjectPath = ExpectedPackageName + TEXT(".") + MaterialKey.LogicalName;
    if (Material->GetPathName() != ExpectedObjectPath ||
        StaticLoadObject(UMaterialInstanceConstant::StaticClass(), nullptr, *ExpectedObjectPath) != Material)
    {
        OutError = FString::Printf(
            TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: '%s' is not canonical managed material '%s'"),
            *Material->GetPathName(),
            *ExpectedObjectPath);
        return false;
    }

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (SourceRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }

    FMHSourceAnalysisServices Services;
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, Services, OutError))
    {
        return false;
    }

    FMHSourceAnalysisEntry Entry;
    Entry.Key = MoveTemp(MaterialKey);
    const FMHResolveOutcome Outcome = Services.Resolver->Resolve(Entry.Key);
    if (Outcome.Status != EMHResolveStatus::Resolved)
    {
        OutError = Outcome.Diagnostic.IsEmpty()
            ? FString::Printf(
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: source for material:%s does not resolve"),
                *Entry.Key.LogicalName)
            : Outcome.Diagnostic;
        return false;
    }

    Entry.PayloadPath = Outcome.PayloadPath;
    FString RelativeBase = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(RelativeBase);
    RelativeBase += TEXT("/");
    Entry.SourcePath = Outcome.PayloadPath;
    if (!FPaths::MakePathRelativeTo(Entry.SourcePath, *RelativeBase))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: cannot derive source path for material:%s from '%s'"),
            *Entry.Key.LogicalName,
            *Outcome.PayloadPath);
        return false;
    }
    FPaths::NormalizeFilename(Entry.SourcePath);
    Entry.RawHash = Outcome.RawHash;
    Entry.Change = EMHSourceChange::Reimport;

    FMHMaterialOperationResult Result = MHImportMaterialV4(
        Entry,
        *Services.Resolver,
        SourceRoot,
        *Settings);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    if (!Result.Succeeded())
    {
        if (!OutError.Contains(Outcome.PayloadPath, ESearchCase::CaseSensitive))
        {
            OutError = FString::Printf(TEXT("%s: %s"), *Outcome.PayloadPath, *OutError);
        }
        return false;
    }
    if (Result.Material != Material)
    {
        OutError = FString::Printf(
            TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: material:%s reimport resolved a different managed UObject"),
            *Entry.Key.LogicalName);
        return false;
    }
    return true;
}

bool UMHSourceImporter::ImportCompositeFile(
    const FString& Filename,
    const FString& TargetPackageName,
    UMHCompositeAsset*& OutAsset,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutAsset = nullptr;
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread())
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: ImportCompositeFile must run on the game thread");
        return false;
    }

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (Settings == nullptr || SourceRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }

    FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FString AbsoluteFile = FPaths::ConvertRelativePathToFull(Filename);
    FPaths::NormalizeDirectoryName(AbsoluteRoot);
    FPaths::NormalizeFilename(AbsoluteFile);
    FPaths::CollapseRelativeDirectories(AbsoluteRoot);
    FPaths::CollapseRelativeDirectories(AbsoluteFile);
    if (!FPaths::GetExtension(AbsoluteFile, true).Equals(TEXT(".composite"), ESearchCase::CaseSensitive) ||
        !FPaths::IsUnderDirectory(AbsoluteFile, AbsoluteRoot))
    {
        OutError = FString::Printf(
            TEXT("%s: MH_E_INVALID_RESOURCE_SOURCE: manual .composite import accepts only a source file already inside source_root '%s'"),
            *AbsoluteFile,
            *AbsoluteRoot);
        return false;
    }

    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = FPaths::GetBaseFilename(AbsoluteFile);
    if (!Key.IsCanonical())
    {
        OutError = FString::Printf(
            TEXT("%s: MH_E_NONCANONICAL_RESOURCE_NAME: composite filename must be <[a-z0-9_]+>.composite"),
            *AbsoluteFile);
        return false;
    }

    const FString ExpectedPackageName = FString(TEXT("/Game/MH/Generated/Composites/")) + Key.LogicalName;
    if (!TargetPackageName.Equals(ExpectedPackageName, ESearchCase::CaseSensitive))
    {
        OutError = FString::Printf(
            TEXT("%s: MH_E_INVALID_RESOURCE_SOURCE: import this file into '%s'; generated composites cannot be created in '%s'"),
            *AbsoluteFile,
            *ExpectedPackageName,
            *TargetPackageName);
        return false;
    }

    FMHSourceAnalysisServices Services;
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, Services, OutError))
    {
        OutError = FString::Printf(TEXT("%s: %s"), *AbsoluteFile, *OutError);
        return false;
    }
    const FMHResolveOutcome Outcome = Services.Resolver->Resolve(Key);
    FString ResolvedPath = FPaths::ConvertRelativePathToFull(Outcome.PayloadPath);
    FPaths::NormalizeFilename(ResolvedPath);
    FPaths::CollapseRelativeDirectories(ResolvedPath);
    if (Outcome.Status != EMHResolveStatus::Resolved ||
        !ResolvedPath.Equals(AbsoluteFile, ESearchCase::IgnoreCase))
    {
        const FString Diagnostic = Outcome.Diagnostic.IsEmpty()
            ? FString::Printf(
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: composite:%s is not the unique resolved candidate for this file"),
                *Key.LogicalName)
            : Outcome.Diagnostic;
        OutError = FString::Printf(TEXT("%s: %s"), *AbsoluteFile, *Diagnostic);
        return false;
    }

    FMHSourceAnalysisEntry Entry;
    Entry.Key = Key;
    Entry.PayloadPath = Outcome.PayloadPath;
    Entry.SourcePath = AbsoluteFile;
    FString RelativeBase = AbsoluteRoot + TEXT("/");
    if (!FPaths::MakePathRelativeTo(Entry.SourcePath, *RelativeBase))
    {
        OutError = FString::Printf(
            TEXT("%s: MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: cannot derive source-relative composite path"),
            *AbsoluteFile);
        return false;
    }
    FPaths::NormalizeFilename(Entry.SourcePath);
    Entry.RawHash = Outcome.RawHash;
    Entry.Change = EMHSourceChange::Reimport;

    FMHCompositeOperationResult Result = MHImportCompositeV4(
        Entry,
        *Services.Resolver,
        SourceRoot,
        *Settings);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    if (!Result.Succeeded())
    {
        OutError = FString::Printf(TEXT("%s: %s"), *AbsoluteFile, *OutError);
        return false;
    }
    OutAsset = Result.Asset;
    return true;
}

bool UMHSourceImporter::PublishMaterial(
    UMaterialInstanceConstant* Material,
    const FString& AdoptFolder,
    const FString& AdoptLogicalName,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || Material == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: PublishMaterial requires a material on the game thread");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || Settings->GetSourceRootPath().IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }
    FMHMaterialAdoptTarget Adopt;
    const FMHMaterialAdoptTarget* AdoptPtr = nullptr;
    if (!AdoptFolder.IsEmpty() || !AdoptLogicalName.IsEmpty())
    {
        Adopt.Folder = AdoptFolder;
        Adopt.LogicalName = AdoptLogicalName;
        AdoptPtr = &Adopt;
    }
    FMHMaterialOperationResult Result = MHPublishMaterialV4(
        *Material,
        Settings->GetSourceRootPath(),
        *Settings,
        AdoptPtr);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    return Result.Succeeded();
}

bool UMHSourceImporter::PublishMaterialInteractive(
    UMaterialInstanceConstant* Material,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || Material == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: PublishMaterialInteractive requires a material on the game thread");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || Settings->GetSourceRootPath().IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }
    const UMHMaterialSourceData* Data = Cast<UMHMaterialSourceData>(
        Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    FMHMaterialOperationResult Result = Data != nullptr && !Data->SourceRelativePath.IsEmpty()
        ? MHPublishMaterialV4(*Material, Settings->GetSourceRootPath(), *Settings)
        : MHShowMaterialAdoptDialog(*Material, Settings->GetSourceRootPath(), *Settings);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    return Result.Succeeded();
}

bool UMHSourceImporter::PublishComposite(
    UMHCompositeAsset* Asset,
    const FString& AdoptFolder,
    const FString& AdoptLogicalName,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || Asset == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: PublishComposite requires an asset on the game thread");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || Settings->GetSourceRootPath().IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }
    FMHCompositeAdoptTarget Adopt;
    const FMHCompositeAdoptTarget* AdoptPtr = nullptr;
    if (!AdoptFolder.IsEmpty() || !AdoptLogicalName.IsEmpty())
    {
        Adopt.Folder = AdoptFolder;
        Adopt.LogicalName = AdoptLogicalName;
        AdoptPtr = &Adopt;
    }
    FMHCompositeOperationResult Result = MHPublishCompositeV4(
        *Asset, Settings->GetSourceRootPath(), AdoptPtr);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    return Result.Succeeded();
}

void UMHSourceImporter::PresentPlan(const FMHSourceAnalysis& Analysis) const
{
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(INVTEXT("Source startup plan"));

    for (const FString& Warning : Analysis.Warnings)
    {
        Log.Warning(FText::FromString(Warning));
    }
    for (const FString& Error : Analysis.Errors)
    {
        Log.Error(FText::FromString(Error));
    }
    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        const FString Line = FString::Printf(
            TEXT("%s  %s  %s"),
            MHSourceChangeLabel(Entry.Change),
            *Entry.Key.ToString(),
            Entry.SourcePath.IsEmpty() ? TEXT("-") : *Entry.SourcePath);
        if (Entry.Errors.IsEmpty())
        {
            Log.Info(FText::FromString(Line));
        }
        else
        {
            Log.Error(FText::FromString(Line));
        }
        for (const FString& Warning : Entry.Warnings)
        {
            Log.Warning(FText::FromString(Warning));
        }
        for (const FString& Error : Entry.Errors)
        {
            Log.Error(FText::FromString(Error));
        }
    }

    const FString Summary = FString::Printf(
        TEXT("Mimir source pass: %d resources, %d blocked. Import order: textures, materials, meshes, composites."),
        Analysis.Entries.Num(),
        Analysis.CountOf(EMHSourceChange::Blocked));
    Log.Info(FText::FromString(Summary));

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings != nullptr && Settings->StartupScanMode == EMHStartupScanMode::Prompt)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Summary));
        Log.Notify(INVTEXT("Mimir source plan is ready"));
    }
}
