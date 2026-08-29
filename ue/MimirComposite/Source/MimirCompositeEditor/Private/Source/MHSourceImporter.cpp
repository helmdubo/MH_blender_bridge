#include "Source/MHSourceImporter.h"

#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeProtocol.h"
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
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMesh/MHStaticMeshImporter.h"
#include "Texture/MHTextureImporter.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHSourceImporter)

namespace UE::MimirComposite
{
namespace
{

constexpr double StartupImportDelaySeconds = 1.0;

#if WITH_DEV_AUTOMATION_TESTS
TFunction<void(EMHResourceKind)> GImportStageObserverForTests;
TFunction<void(const FMHResourceKey&)> GProfileFreshnessAssetLoadObserverForTests;
TFunction<bool(EMHSourceBulkImportPhase)> GBulkImportPhaseTestHook;
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

#if WITH_DEV_AUTOMATION_TESTS
bool ContinueAfterBulkImportPhase(const EMHSourceBulkImportPhase Phase)
{
    return !GBulkImportPhaseTestHook || GBulkImportPhaseTestHook(Phase);
}
#endif

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

void BlockProfileFreshnessCheck(
    FMHSourceAnalysisEntry& Entry,
    FString Error)
{
    Entry.Change = EMHSourceChange::Blocked;
    Entry.Errors.Add(Error.IsEmpty()
        ? TEXT("MH_E_SOURCE_INDEX_INVALID: inline profile freshness check failed")
        : MoveTemp(Error));
}

void PromoteStaleInlinedProfileReceipts(
    FMHSourceAnalysisServices& Services,
    FMHSourceAnalysis& Analysis)
{
    check(Services.Index.IsValid());
    check(Services.Resolver.IsValid());
    for (FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::Composite ||
            Entry.Change != EMHSourceChange::NoChange ||
            !Entry.Errors.IsEmpty())
        {
            continue;
        }

        TArray<FMHResourceKey> ProfileKeys;
        FString Error;
        if (!Services.Index->GetPlacementProfileDependencies(Entry.Key, ProfileKeys, Error))
        {
            BlockProfileFreshnessCheck(Entry, MoveTemp(Error));
            continue;
        }
        if (ProfileKeys.IsEmpty())
        {
            continue;
        }

        TArray<FMHProjectIndexGeneratedAssetState> Assets;
        if (!Services.Index->GetGeneratedAssets(Entry.Key, Assets, Error) ||
            Assets.Num() != 1 || Assets[0].UEObjectPath.IsEmpty())
        {
            if (Error.IsEmpty())
            {
                Error = TEXT("MH_E_SOURCE_INDEX_INVALID: NO_CHANGE profiled composite has no unique generated asset");
            }
            BlockProfileFreshnessCheck(Entry, MoveTemp(Error));
            continue;
        }

#if WITH_DEV_AUTOMATION_TESTS
        if (GProfileFreshnessAssetLoadObserverForTests)
        {
            GProfileFreshnessAssetLoadObserverForTests(Entry.Key);
        }
#endif
        const UMHCompositeAsset* Asset = LoadObject<UMHCompositeAsset>(
            nullptr, *Assets[0].UEObjectPath);
        if (Asset == nullptr)
        {
            BlockProfileFreshnessCheck(
                Entry,
                FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_INVALID: cannot load inline profile carrier %s"),
                    *Assets[0].UEObjectPath));
            continue;
        }

        bool bRequiresReimport =
            Asset->InlinedPlacementProfiles.Num() != ProfileKeys.Num();
        TMap<FString, FString> AppliedHashes;
        for (const FMHPlacementProfile& Profile : Asset->InlinedPlacementProfiles)
        {
            if (!MHIsCanonicalCompositeToken(Profile.LogicalName) ||
                AppliedHashes.Contains(Profile.LogicalName) ||
                !MHIsCanonicalRawPayloadHash(Profile.GetAppliedSourceHash()))
            {
                bRequiresReimport = true;
                continue;
            }
            AppliedHashes.Add(Profile.LogicalName, Profile.GetAppliedSourceHash());
        }
        for (const FMHResourceKey& ProfileKey : ProfileKeys)
        {
            const FMHResolveOutcome Outcome = Services.Resolver->Resolve(ProfileKey);
            if (Outcome.Status != EMHResolveStatus::Resolved ||
                !MHIsCanonicalRawPayloadHash(Outcome.RawHash))
            {
                BlockProfileFreshnessCheck(
                    Entry,
                    Outcome.Diagnostic.IsEmpty()
                        ? FString::Printf(
                            TEXT("MH_E_SOURCE_INDEX_INVALID: cannot resolve inline profile receipt for %s"),
                            *ProfileKey.ToString())
                        : Outcome.Diagnostic);
                bRequiresReimport = false;
                break;
            }
            const FString* AppliedHash = AppliedHashes.Find(ProfileKey.LogicalName);
            bRequiresReimport |= AppliedHash == nullptr || *AppliedHash != Outcome.RawHash;
        }
        if (Entry.Change != EMHSourceChange::Blocked && bRequiresReimport)
        {
            Entry.Change = EMHSourceChange::Reimport;
        }
    }
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

    // §13.4.1: index status remains a pure six-tag projection. Only otherwise
    // NO_CHANGE composites with an indexed profile edge pay the UObject load
    // needed to compare durable inlined-profile receipts.
    PromoteStaleInlinedProfileReceipts(Services, OutAnalysis);

    // placement_profile is a source-only leaf. It has no generated path or
    // UObject; the dependent composite importer parses and inlines its typed
    // value into UMHCompositeAsset.
    ObserveImportStage(EMHResourceKind::PlacementProfile);

    TSet<FString> FailedTextures;
    ObserveImportStage(EMHResourceKind::Texture);
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        const bool bNoChangePolicyCheck =
            Entry.Change == EMHSourceChange::NoChange && Entry.Errors.IsEmpty();
        if (Entry.Key.Kind != EMHResourceKind::Texture ||
            (!ShouldExecuteEntry(Entry) && !bNoChangePolicyCheck))
        {
            continue;
        }
        FMHTextureOperationResult TextureResult = MHEnsureTextureV4(
            Entry,
            SourceRoot,
            !bNoChangePolicyCheck);
        Entry.Warnings.Append(TextureResult.Warnings);
        if (!TextureResult.Succeeded())
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(TextureResult.Error);
            FailedTextures.Add(Entry.Key.LogicalName);
        }
        else
        {
            if (bNoChangePolicyCheck && TextureResult.bImported)
            {
                Entry.Change = EMHSourceChange::Reimport;
            }
            bOutExecuted |= TextureResult.bImported;
        }
    }

    ObserveImportStage(EMHResourceKind::Material);
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::Material || !Entry.Errors.IsEmpty())
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
        if (!ShouldExecuteEntry(Entry))
        {
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
        FMHCompositeOperationResult CompositeResult = MHImportCompositeV5(
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
#if WITH_DEV_AUTOMATION_TESTS
    if (!ContinueAfterBulkImportPhase(EMHSourceBulkImportPhase::AssetsPrepared))
    {
        return false;
    }
#endif
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

bool MHShouldPresentWatcherAnalysis(
    const TArray<FString>& Paths,
    const FMHSourceAnalysis& Analysis)
{
    TSet<FMHResourceKey> ObservedKeys;
    for (const FString& Path : Paths)
    {
        FMHResourceKey Key;
        FString ClassificationError;
        if (MHResourceKeyFromSourceFile(Path, Key, ClassificationError))
        {
            ObservedKeys.Add(MoveTemp(Key));
        }
        else if (!ClassificationError.IsEmpty())
        {
            return true;
        }
    }

    for (const FMHResourceKey& Key : ObservedKeys)
    {
        const FMHSourceAnalysisEntry* Entry = Analysis.Find(Key);
        if (Entry != nullptr &&
            (Entry->Change != EMHSourceChange::NoChange ||
             !Entry->Warnings.IsEmpty() ||
             !Entry->Errors.IsEmpty()))
        {
            return true;
        }
    }
    return false;
}

#if WITH_DEV_AUTOMATION_TESTS
void MHSetImportStageObserverForTests(TFunction<void(EMHResourceKind)> Observer)
{
    GImportStageObserverForTests = MoveTemp(Observer);
}

void MHSetProfileFreshnessAssetLoadObserverForTests(
    TFunction<void(const FMHResourceKey&)> Observer)
{
    GProfileFreshnessAssetLoadObserverForTests = MoveTemp(Observer);
}

void MHSetBulkImportPhaseTestHook(
    TFunction<bool(EMHSourceBulkImportPhase)> Hook)
{
    GBulkImportPhaseTestHook = MoveTemp(Hook);
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
        if (WeakThis.IsValid() && WeakThis->LifecycleTickerHandle.IsValid())
        {
            WeakThis->bAssetRegistryReady = true;
            WeakThis->AssetRegistryReadySeconds = WeakThis->LifecycleNowSeconds();
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
        if (bAttempted)
        {
            RefreshLoadedCompositeActorsAfterStartup();
        }
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
    RefreshLoadedCompositeActorsAfterStartup();
    return true;
}

void UMHSourceImporter::RefreshLoadedCompositeActorsAfterStartup()
{
#if WITH_DEV_AUTOMATION_TESTS
    if (StartupCompositeRefreshExecutorForTests)
    {
        StartupCompositeRefreshExecutorForTests();
        return;
    }
#endif
    const int32 RebuiltCount = MHRebuildAllLoadedCompositeActors();
    if (RebuiltCount > 0)
    {
        FMessageLog(TEXT("Mimir")).Info(FText::Format(
            INVTEXT("Startup source pass settled; rebuilt {0} loaded composite instance(s)"),
            FText::AsNumber(RebuiltCount)));
    }
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
    if (bAssetRegistryReady &&
        !bStartupPlanRan &&
        !bPIEActive &&
        !bImportInProgress &&
        LifecycleNowSeconds() - AssetRegistryReadySeconds >= StartupImportDelaySeconds)
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
    const double ResumeSeconds = LifecycleNowSeconds();
    LastSourceChangeSeconds = ResumeSeconds;
    if (!bStartupPlanRan)
    {
        AssetRegistryReadySeconds = ResumeSeconds;
    }
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
        if (WeakThis.IsValid() && WeakThis->LifecycleTickerHandle.IsValid())
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
    if (bUsedFullScan || !Update.SessionEvents.IsEmpty() ||
        MHShouldPresentWatcherAnalysis(Paths, Analysis))
    {
        PresentPlan(Analysis);
    }
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
    if (bReady && !bAssetRegistryReady)
    {
        AssetRegistryReadySeconds = LifecycleNowSeconds();
    }
    bAssetRegistryReady = bReady;
}

void UMHSourceImporter::SetPIEActiveForTests(const bool bActive)
{
    if (bActive)
    {
        bPIEActive = true;
        return;
    }
    OnEndPIE(false);
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

void UMHSourceImporter::SetStartupCompositeRefreshExecutorForTests(
    TFunction<void()> Executor)
{
    StartupCompositeRefreshExecutorForTests = MoveTemp(Executor);
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
    (void)TargetPackageName;
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

    FMHCompositeOperationResult Result = MHImportCompositeV5(
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

bool UMHSourceImporter::AdoptCompositeFile(
    const FString& Filename,
    const FString& AdoptFolder,
    const FString& AdoptLogicalName,
    UMHCompositeAsset*& OutAsset,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutAsset = nullptr;
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread())
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: AdoptCompositeFile must run on the game thread");
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
        FPaths::IsUnderDirectory(AbsoluteFile, AbsoluteRoot))
    {
        OutError = FString::Printf(
            TEXT("%s: MH_E_INVALID_RESOURCE_SOURCE: Adopt expects an external file with exact .composite suffix"),
            *AbsoluteFile);
        return false;
    }

    FString TargetPath;
    FString TargetRelativePath;
    const FMHCompositeAdoptTarget AdoptTarget{AdoptFolder, AdoptLogicalName};
    if (!MHValidateCompositeAdoptTarget(
            SourceRoot,
            AdoptTarget,
            TargetPath,
            TargetRelativePath,
            OutError))
    {
        OutError = FString::Printf(TEXT("%s: %s"), *AbsoluteFile, *OutError);
        return false;
    }

    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = AdoptTarget.LogicalName;
    FMHSourceAnalysisServices Services;
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, Services, OutError))
    {
        OutError = FString::Printf(TEXT("%s: %s"), *AbsoluteFile, *OutError);
        return false;
    }
    const FMHResolveOutcome Existing = Services.Resolver->Resolve(Key);
    if (Existing.Status != EMHResolveStatus::Unresolved || FPaths::FileExists(TargetPath))
    {
        OutError = FString::Printf(
            TEXT("%s: MH_E_AMBIGUOUS_RESOURCE_NAME: composite:%s already exists in source_root; Adopt never overwrites"),
            *AbsoluteFile,
            *Key.LogicalName);
        return false;
    }

    TArray<uint8> SourceBytes;
    FMHCompositeDocument Document;
    if (!FFileHelper::LoadFileToArray(SourceBytes, *AbsoluteFile) ||
        !MHParseCompositeV5(SourceBytes, Document, OutError))
    {
        if (OutError.IsEmpty())
        {
            OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: cannot read external composite payload");
        }
        OutError = FString::Printf(TEXT("%s: %s"), *AbsoluteFile, *OutError);
        return false;
    }

    const FString TargetFolder = FPaths::GetPath(TargetPath);
    if (!IFileManager::Get().MakeDirectory(*TargetFolder, true))
    {
        OutError = FString::Printf(
            TEXT("%s: MH_E_INVALID_RESOURCE_SOURCE: cannot create Adopt target folder '%s'"),
            *AbsoluteFile,
            *TargetFolder);
        return false;
    }
    const FString TempPath = TargetPath + FString::Printf(
        TEXT(".tmp.%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TArray<uint8> ReadBack;
    FMHCompositeDocument ReadBackDocument;
    FString ValidationError;
    if (!FFileHelper::SaveArrayToFile(SourceBytes, *TempPath) ||
        !FFileHelper::LoadFileToArray(ReadBack, *TempPath) ||
        ReadBack != SourceBytes ||
        !MHParseCompositeV5(ReadBack, ReadBackDocument, ValidationError))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("%s: MH_E_COMPOSITE_GRAMMAR: Adopt temporary read-back failed: %s"),
            *AbsoluteFile,
            *ValidationError);
        return false;
    }
    if (FPaths::FileExists(TargetPath) ||
        !IFileManager::Get().Move(*TargetPath, *TempPath, false, false, false, true))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("%s: MH_E_AMBIGUOUS_RESOURCE_NAME: Adopt target appeared concurrently; no file was overwritten"),
            *AbsoluteFile);
        return false;
    }

    return ImportCompositeFile(
        TargetPath,
        FString(),
        OutAsset,
        OutWarnings,
        OutError);
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
    FMHCompositeOperationResult Result = MHPublishCompositeV5(
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
        Log.Notify(FText::FromString(Summary));
    }
}
