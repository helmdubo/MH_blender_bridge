#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeProtocol.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "Source/MHPayloadHashes.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<uint8> LifecycleUtf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value, Value.Len());
    TArray<uint8> Bytes;
    Bytes.Append(
        reinterpret_cast<const uint8*>(Converted.Get()),
        Converted.Length());
    return Bytes;
}

void DeleteLifecycleAssetPackage(const FString& PackageName)
{
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
    const FString ObjectPath = PackageName + TEXT(".") + AssetName;
    if (UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
    {
        ObjectTools::DeleteSingleObject(Asset, false);
    }
    if (UPackage* Package = FindPackage(nullptr, *PackageName))
    {
        Package->SetDirtyFlag(false);
    }
    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName,
        FPackageName::GetAssetPackageExtension());
    IFileManager::Get().Delete(*Filename, false, true, true);
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceLifecycleDebouncePIETest,
    "Mimir.V4.SourceLifecycle.DebounceAndPIEQueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceLifecycleDebouncePIETest::RunTest(const FString& Parameters)
{
    UMHSourceImporter* Importer = NewObject<UMHSourceImporter>();
    TArray<TArray<FString>> Batches;
    TArray<bool> FullScanFlags;
    Importer->SetBatchExecutorForTests(
        [&Batches, &FullScanFlags](const TArray<FString>& Paths, const bool bFullScan)
        {
            Batches.Add(Paths);
            FullScanFlags.Add(bFullScan);
            return true;
        });

    Importer->SetLifecycleTimeForTests(10.0);
    const FString PathA = FPaths::ConvertRelativePathToFull(TEXT("SourceLifecycle/a.material"));
    const FString PathB = FPaths::ConvertRelativePathToFull(TEXT("SourceLifecycle/b.composite"));
    Importer->QueueSourcePathsForTests({PathB, PathA, PathA});
    Importer->SetLifecycleTimeForTests(10.999);
    Importer->TickSourceLifecycleForTests();
    bool bPassed = TestEqual(TEXT("no batch before one second"), Batches.Num(), 0);

    Importer->SetLifecycleTimeForTests(11.0);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("one debounced batch"), Batches.Num(), 1);
    if (Batches.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("batch paths deduplicated"), Batches[0].Num(), 2);
        bPassed &= TestTrue(
            TEXT("batch paths sorted"),
            Batches[0].Num() == 2 && Batches[0][0] < Batches[0][1]);
    }

    Importer->SetPIEActiveForTests(true);
    Importer->SetLifecycleTimeForTests(20.0);
    Importer->QueueSourcePathsForTests({PathA}, true);
    Importer->SetLifecycleTimeForTests(30.0);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("PIE holds mature batch"), Batches.Num(), 1);
    Importer->SetPIEActiveForTests(false);
    bPassed &= TestEqual(TEXT("EndPIE delegate never flushes queued batch"), Batches.Num(), 1);
    Importer->SetLifecycleTimeForTests(30.999);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("post-PIE batch waits for quiet period"), Batches.Num(), 1);
    Importer->SetLifecycleTimeForTests(31.0);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("ticker flushes queued batch after post-PIE quiet period"), Batches.Num(), 2);
    bPassed &= TestTrue(
        TEXT("rescan-required survives PIE queue"),
        FullScanFlags.Num() == 2 && FullScanFlags[1]);
    bPassed &= TestEqual(TEXT("pending paths consumed"), Importer->GetPendingPathCountForTests(), 0);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceLifecycleStartupAndOrderTest,
    "Mimir.V4.SourceLifecycle.StartupRetryAndImportOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceLifecycleStartupAndOrderTest::RunTest(const FString& Parameters)
{
    UMHSourceImporter* Importer = NewObject<UMHSourceImporter>();
    int32 StartupAttempts = 0;
    int32 StartupCompositeRefreshes = 0;
    Importer->SetStartupCompositeRefreshExecutorForTests([&StartupCompositeRefreshes]()
    {
        ++StartupCompositeRefreshes;
    });
    Importer->SetStartupExecutorForTests([&StartupAttempts]()
    {
        ++StartupAttempts;
        return StartupAttempts >= 2;
    });
    Importer->SetLifecycleTimeForTests(100.0);
    Importer->SetAssetRegistryReadyForTests(false);
    Importer->TickSourceLifecycleForTests();
    bool bPassed = TestEqual(TEXT("startup waits for Asset Registry"), StartupAttempts, 0);
    Importer->SetAssetRegistryReadyForTests(true);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("startup never runs from Asset Registry readiness"), StartupAttempts, 0);
    Importer->SetLifecycleTimeForTests(100.999);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("startup waits for one-second editor quiet period"), StartupAttempts, 0);
    Importer->SetLifecycleTimeForTests(101.0);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("first startup attempt is retryable"), StartupAttempts, 1);
    bPassed &= TestEqual(TEXT("failed startup does not refresh composite instances"), StartupCompositeRefreshes, 0);
    bPassed &= TestFalse(TEXT("failed attempt is not marked complete"), Importer->HasStartupPlanRunForTests());
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("startup retries once"), StartupAttempts, 2);
    bPassed &= TestTrue(TEXT("successful startup is complete"), Importer->HasStartupPlanRunForTests());
    bPassed &= TestEqual(TEXT("successful startup refreshes composite instances once"), StartupCompositeRefreshes, 1);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("completed startup is exactly once"), StartupAttempts, 2);
    bPassed &= TestEqual(TEXT("completed startup does not refresh twice"), StartupCompositeRefreshes, 1);

    UMHSourceImporter* PIEStartupImporter = NewObject<UMHSourceImporter>();
    int32 PIEStartupAttempts = 0;
    PIEStartupImporter->SetStartupExecutorForTests([&PIEStartupAttempts]()
    {
        ++PIEStartupAttempts;
        return true;
    });
    PIEStartupImporter->SetLifecycleTimeForTests(200.0);
    PIEStartupImporter->SetAssetRegistryReadyForTests(true);
    PIEStartupImporter->SetPIEActiveForTests(true);
    PIEStartupImporter->SetLifecycleTimeForTests(202.0);
    PIEStartupImporter->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("PIE holds mature startup"), PIEStartupAttempts, 0);
    PIEStartupImporter->SetPIEActiveForTests(false);
    bPassed &= TestEqual(TEXT("EndPIE delegate never starts startup"), PIEStartupAttempts, 0);
    PIEStartupImporter->SetLifecycleTimeForTests(202.999);
    PIEStartupImporter->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("post-PIE startup waits for quiet period"), PIEStartupAttempts, 0);
    PIEStartupImporter->SetLifecycleTimeForTests(203.0);
    PIEStartupImporter->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("ticker starts startup after post-PIE quiet period"), PIEStartupAttempts, 1);

    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests/lifecycle_order"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    TArray<EMHResourceKind> Stages;
    MHSetImportStageObserverForTests([&Stages](const EMHResourceKind Kind)
    {
        Stages.Add(Kind);
    });
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    MHShutdownProjectIndex();
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        Analysis,
        bExecuted);
    MHSetImportStageObserverForTests(TFunction<void(EMHResourceKind)>());
    const TArray<EMHResourceKind> Expected = {
        EMHResourceKind::PlacementProfile,
        EMHResourceKind::Texture,
        EMHResourceKind::Material,
        EMHResourceKind::StaticMesh,
        EMHResourceKind::Composite};
    bPassed &= TestTrue(TEXT("coordinator stage order is P-T-M-SM-C"), Stages == Expected);
    MHShutdownProjectIndex();
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceLifecycleSelfPublishEchoTest,
    "Mimir.V4.SourceLifecycle.SelfPublishEchoNoChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceLifecycleSelfPublishEchoTest::RunTest(const FString& Parameters)
{
    const FString Token = FString::Printf(
        TEXT("s6_echo_%08x"),
        FPlatformTime::Cycles());
    FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests/lifecycle_echo"),
        Token));
    FPaths::NormalizeDirectoryName(SourceRoot);
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString SourcePath = FPaths::Combine(SourceRoot, Token + TEXT(".composite"));
    const TArray<uint8> Initial = LifecycleUtf8(TEXT("{\n  \"v\": 5,\n  \"nodes\": []\n}\n"));
    bool bPassed = TestTrue(
        TEXT("write initial composite"),
        FFileHelper::SaveArrayToFile(Initial, *SourcePath));

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHSourceAnalysis InitialAnalysis;
    bool bInitialExecuted = false;
    MHShutdownProjectIndex();
    const bool bInitialImport = MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        InitialAnalysis,
        bInitialExecuted);
    // The shared Automation host can retain unrelated managed claims from
    // earlier tests. Their diagnostics may make the all-project operation
    // return false without affecting this resource's completed import.
    (void)bInitialImport;
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Token;
    const FMHSourceAnalysisEntry* InitialEntry = InitialAnalysis.Find(Key);
    bPassed &= TestNotNull(TEXT("initial plan contains the echo resource"), InitialEntry);
    if (InitialEntry != nullptr)
    {
        bPassed &= TestTrue(TEXT("echo resource initial import has no errors"), InitialEntry->Errors.IsEmpty());
    }
    bPassed &= TestTrue(TEXT("initial import executes"), bInitialExecuted);

    const FString PackageName = FString(TEXT("/Game/MH/Generated/Composites/")) + Token;
    const FString ObjectPath = PackageName + TEXT(".") + Token;
    UMHCompositeAsset* Asset = LoadObject<UMHCompositeAsset>(nullptr, *ObjectPath);
    bPassed &= TestNotNull(TEXT("managed composite exists"), Asset);
    if (Asset == nullptr)
    {
        MHShutdownProjectIndex();
        return false;
    }

    FMHCompositeDocument Edited;
    FMHCompositeNode& Group = Edited.Nodes.AddDefaulted_GetRef();
    Group.Kind = EMHCompositeNodeKind::Group;
    Group.Name = TEXT("echo");
    FString Error;
    bPassed &= TestTrue(TEXT("apply local source-shaped edit"), MHApplyCompositeV5(*Asset, Edited, Error));
    const FMHCompositeOperationResult Published = MHPublishCompositeV5(*Asset, SourceRoot);
    bPassed &= TestTrue(TEXT("publish succeeds"), Published.Succeeded());
    if (!Published.Succeeded())
    {
        AddError(Published.Error);
    }
    UMHCompositeAsset* const PublishedIdentity = Asset;

    FMHSourceAnalysisServices EchoServices;
    FMHProjectIndexUpdateResult EchoUpdate;
    bool bUsedFullScan = true;
    bPassed &= TestTrue(
        TEXT("watcher echo upserts"),
        MHCreateIncrementalSourceAnalysisServices(
            SourceRoot,
            {SourcePath},
            EchoServices,
            EchoUpdate,
            bUsedFullScan,
            Error));
    bPassed &= TestFalse(TEXT("watcher echo does not full scan"), bUsedFullScan);
    bPassed &= TestTrue(TEXT("publish token is single-shot before echo"), EchoUpdate.SessionEvents.IsEmpty());

    FMHSourceAnalysis EchoAnalysis;
    bool bEchoExecuted = false;
    if (EchoServices.Resolver && EchoServices.ChangeDetector)
    {
        // As above, the all-project result can carry unrelated test-host
        // diagnostics; the loop invariant is the targeted NO_CHANGE row and
        // the absence of a builder execution for the watcher echo.
        MHBuildSourceImportPlan(
            *EchoServices.ChangeDetector,
            *EchoServices.Resolver,
            SourceRoot,
            FMHImportSourcesScope::All(),
            EchoAnalysis,
            bEchoExecuted);
        const FMHSourceAnalysisEntry* EchoEntry = EchoAnalysis.Find(Key);
        bPassed &= TestNotNull(TEXT("echo plan contains composite"), EchoEntry);
        if (EchoEntry != nullptr)
        {
            bPassed &= TestEqual(
                TEXT("watcher echo is NO_CHANGE"),
                EchoEntry->Change,
                EMHSourceChange::NoChange);
        }
    }
    bPassed &= TestFalse(TEXT("echo plan executes no builder"), bEchoExecuted);
    bPassed &= TestFalse(
        TEXT("NO_CHANGE watcher echo does not present the all-project plan"),
        MHShouldPresentWatcherAnalysis({SourcePath}, EchoAnalysis));
    FMHSourceAnalysis RelevantWatcherAnalysis;
    FMHSourceAnalysisEntry& RelevantEntry = RelevantWatcherAnalysis.Entries.AddDefaulted_GetRef();
    RelevantEntry.Key = Key;
    RelevantEntry.Change = EMHSourceChange::Reimport;
    bPassed &= TestTrue(
        TEXT("real watcher change remains visible"),
        MHShouldPresentWatcherAnalysis({SourcePath}, RelevantWatcherAnalysis));
    bPassed &= TestTrue(
        TEXT("publish preserves managed UObject identity"),
        LoadObject<UMHCompositeAsset>(nullptr, *ObjectPath) == PublishedIdentity);

    MHShutdownProjectIndex();
    DeleteLifecycleAssetPackage(PackageName);
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceLifecyclePlacementProfileFreshnessTest,
    "Mimir.V5.SourceLifecycle.PlacementProfileFreshness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceLifecyclePlacementProfileFreshnessTest::RunTest(const FString& Parameters)
{
    const FString Token = FString::Printf(
        TEXT("v5_profile_freshness_%08x"),
        FPlatformTime::Cycles());
    FString SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests/profile_freshness"),
        Token));
    FPaths::NormalizeDirectoryName(SourceRoot);
    IFileManager::Get().MakeDirectory(*SourceRoot, true);

    const FString ProfileName = Token + TEXT("_scatter");
    const FString ProfiledName = Token + TEXT("_profiled");
    const FString PlainName = Token + TEXT("_plain");
    const FString PlacementPath = FPaths::Combine(SourceRoot, ProfileName + TEXT(".placement"));
    const FString ProfiledPath = FPaths::Combine(SourceRoot, ProfiledName + TEXT(".composite"));
    const FString PlainPath = FPaths::Combine(SourceRoot, PlainName + TEXT(".composite"));
    const TArray<uint8> ProfileH1 = LifecycleUtf8(
        TEXT("{\n  \"v\": 1,\n  \"kind\": \"placement_profile\",\n  \"uniform_scale\": [1, 0.125]\n}\n"));
    const TArray<uint8> ProfileH2SameValue = LifecycleUtf8(
        TEXT("{\"v\":1,\"kind\":\"placement_profile\",\"uniform_scale\":[1,0.125]}"));
    const TArray<uint8> ProfileH3 = LifecycleUtf8(
        TEXT("{\n  \"v\": 1,\n  \"kind\": \"placement_profile\",\n  \"uniform_scale\": [1, 0.25]\n}\n"));
    const TArray<uint8> ProfiledBytes = LifecycleUtf8(FString::Printf(
        TEXT("{\n  \"v\": 5,\n  \"nodes\": [\n    {\"kind\": \"group\", \"profile\": \"%s\"}\n  ]\n}\n"),
        *ProfileName));
    const TArray<uint8> PlainBytes = LifecycleUtf8(
        TEXT("{\n  \"v\": 5,\n  \"nodes\": [{\"kind\": \"group\"}]\n}\n"));
    bool bPassed = TestTrue(
        TEXT("write initial placement profile"),
        FFileHelper::SaveArrayToFile(ProfileH1, *PlacementPath));
    bPassed &= TestTrue(
        TEXT("write profiled composite"),
        FFileHelper::SaveArrayToFile(ProfiledBytes, *ProfiledPath));
    bPassed &= TestTrue(
        TEXT("write profile-free composite"),
        FFileHelper::SaveArrayToFile(PlainBytes, *PlainPath));

    FMHResourceKey ProfiledKey;
    ProfiledKey.Kind = EMHResourceKind::Composite;
    ProfiledKey.LogicalName = ProfiledName;
    FMHResourceKey PlainKey;
    PlainKey.Kind = EMHResourceKind::Composite;
    PlainKey.LogicalName = PlainName;
    const FString ProfiledPackage = FString(TEXT("/Game/MH/Generated/Composites/")) + ProfiledName;
    const FString PlainPackage = FString(TEXT("/Game/MH/Generated/Composites/")) + PlainName;
    const FString ProfiledObjectPath = ProfiledPackage + TEXT(".") + ProfiledName;
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();

    FMHSourceAnalysis InitialAnalysis;
    bool bInitialExecuted = false;
    MHShutdownProjectIndex();
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        InitialAnalysis,
        bInitialExecuted);
    const FMHSourceAnalysisEntry* InitialEntry = InitialAnalysis.Find(ProfiledKey);
    bPassed &= TestNotNull(TEXT("initial plan contains profiled composite"), InitialEntry);
    bPassed &= TestTrue(
        TEXT("initial profiled composite import executes"),
        bInitialExecuted && InitialEntry != nullptr && InitialEntry->Errors.IsEmpty());
    UMHCompositeAsset* ProfiledAsset = LoadObject<UMHCompositeAsset>(nullptr, *ProfiledObjectPath);
    bPassed &= TestNotNull(TEXT("profiled carrier exists"), ProfiledAsset);
    if (ProfiledAsset != nullptr)
    {
        bPassed &= TestEqual(
            TEXT("initial exact raw profile receipt persisted"),
            ProfiledAsset->InlinedPlacementProfiles.Num(),
            1);
        if (ProfiledAsset->InlinedPlacementProfiles.Num() == 1)
        {
            bPassed &= TestEqual(
                TEXT("initial profile receipt is H1"),
                ProfiledAsset->InlinedPlacementProfiles[0].GetAppliedSourceHash(),
                MHRawPayloadHash(ProfileH1));
        }
    }

    TArray<FMHResourceKey> LoadedForFreshness;
    MHSetProfileFreshnessAssetLoadObserverForTests(
        [&LoadedForFreshness](const FMHResourceKey& Key)
        {
            LoadedForFreshness.Add(Key);
        });
    FMHSourceAnalysis MatchingAnalysis;
    bool bMatchingExecuted = false;
    MHShutdownProjectIndex();
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        MatchingAnalysis,
        bMatchingExecuted);
    const FMHSourceAnalysisEntry* MatchingEntry = MatchingAnalysis.Find(ProfiledKey);
    bPassed &= TestTrue(
        TEXT("matching profile receipt stays NO_CHANGE"),
        MatchingEntry != nullptr &&
        MatchingEntry->Change == EMHSourceChange::NoChange &&
        MatchingEntry->Errors.IsEmpty());
    bPassed &= TestFalse(TEXT("matching receipt executes no import"), bMatchingExecuted);
    bPassed &= TestTrue(
        TEXT("freshness loader touches only the profiled NO_CHANGE composite"),
        LoadedForFreshness.Num() == 1 &&
        LoadedForFreshness[0] == ProfiledKey &&
        !LoadedForFreshness.Contains(PlainKey));
    MHSetProfileFreshnessAssetLoadObserverForTests(
        TFunction<void(const FMHResourceKey&)>());

    if (ProfiledAsset != nullptr && ProfiledAsset->InlinedPlacementProfiles.Num() == 1)
    {
        ProfiledAsset->InlinedPlacementProfiles[0].SetAppliedSourceHash(FString());
    }
    MHShutdownProjectIndex();
    FMHSourceAnalysis EmptyReceiptAnalysis;
    bool bEmptyReceiptExecuted = false;
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        EmptyReceiptAnalysis,
        bEmptyReceiptExecuted);
    const FMHSourceAnalysisEntry* EmptyReceiptEntry = EmptyReceiptAnalysis.Find(ProfiledKey);
    bPassed &= TestTrue(
        TEXT("legacy empty profile receipt forces REIMPORT"),
        EmptyReceiptEntry != nullptr &&
        EmptyReceiptEntry->Change == EMHSourceChange::Reimport &&
        EmptyReceiptEntry->Errors.IsEmpty() &&
        bEmptyReceiptExecuted);
    ProfiledAsset = LoadObject<UMHCompositeAsset>(nullptr, *ProfiledObjectPath);
    if (ProfiledAsset != nullptr && ProfiledAsset->InlinedPlacementProfiles.Num() == 1)
    {
        const FMHPlacementProfile DuplicateProfile =
            ProfiledAsset->InlinedPlacementProfiles[0];
        ProfiledAsset->InlinedPlacementProfiles.Add(DuplicateProfile);
    }
    MHShutdownProjectIndex();
    FMHSourceAnalysis DuplicateReceiptAnalysis;
    bool bDuplicateReceiptExecuted = false;
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        DuplicateReceiptAnalysis,
        bDuplicateReceiptExecuted);
    const FMHSourceAnalysisEntry* DuplicateReceiptEntry = DuplicateReceiptAnalysis.Find(ProfiledKey);
    bPassed &= TestTrue(
        TEXT("duplicate stored profile receipt forces REIMPORT"),
        DuplicateReceiptEntry != nullptr &&
        DuplicateReceiptEntry->Change == EMHSourceChange::Reimport &&
        DuplicateReceiptEntry->Errors.IsEmpty() &&
        bDuplicateReceiptExecuted);

    bPassed &= TestTrue(
        TEXT("rewrite profile with different raw bytes and same typed value"),
        FFileHelper::SaveArrayToFile(ProfileH2SameValue, *PlacementPath));
    bPassed &= TestNotEqual(
        TEXT("raw-only edit changes the receipt domain"),
        MHRawPayloadHash(ProfileH1),
        MHRawPayloadHash(ProfileH2SameValue));
    MHShutdownProjectIndex();
    FMHSourceAnalysisServices ProjectionServices;
    FString Error;
    bPassed &= TestTrue(
        TEXT("rebuild pure projection after profile edit"),
        MHCreateDefaultSourceAnalysisServices(SourceRoot, ProjectionServices, Error));
    TArray<FMHProjectIndexGeneratedAssetState> ProjectedAssets;
    if (ProjectionServices.Index.IsValid())
    {
        bPassed &= TestTrue(
            TEXT("read projected profiled carrier"),
            ProjectionServices.Index->GetGeneratedAssets(ProfiledKey, ProjectedAssets, Error));
        bPassed &= TestTrue(
            TEXT("profile-only edit leaves GeneratedAssets applied"),
            ProjectedAssets.Num() == 1 &&
            ProjectedAssets[0].Status == EMHGeneratedAssetStatus::Applied);
    }
    FMHSourceAnalysis ProjectionAnalysis;
    bool bProjectionExecuted = false;
    if (ProjectionServices.ChangeDetector && ProjectionServices.Resolver)
    {
        MHBuildSourceImportPlan(
            *ProjectionServices.ChangeDetector,
            *ProjectionServices.Resolver,
            SourceRoot,
            FMHImportSourcesScope::All(),
            ProjectionAnalysis,
            bProjectionExecuted);
        const FMHSourceAnalysisEntry* ProjectionEntry = ProjectionAnalysis.Find(ProfiledKey);
        bPassed &= TestTrue(
            TEXT("pure index plan remains NO_CHANGE before importer freshness"),
            ProjectionEntry != nullptr &&
            ProjectionEntry->Change == EMHSourceChange::NoChange);
    }
    bPassed &= TestFalse(TEXT("pure projection executes no builder"), bProjectionExecuted);

    MHShutdownProjectIndex();
    FMHSourceAnalysis ChangedAnalysis;
    bool bChangedExecuted = false;
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        ChangedAnalysis,
        bChangedExecuted);
    const FMHSourceAnalysisEntry* ChangedEntry = ChangedAnalysis.Find(ProfiledKey);
    bPassed &= TestTrue(
        TEXT("importer promotes changed profile receipt to REIMPORT"),
        ChangedEntry != nullptr &&
        ChangedEntry->Change == EMHSourceChange::Reimport &&
        ChangedEntry->Errors.IsEmpty());
    bPassed &= TestTrue(TEXT("profile receipt reimport executes"), bChangedExecuted);
    ProfiledAsset = LoadObject<UMHCompositeAsset>(nullptr, *ProfiledObjectPath);
    bPassed &= TestTrue(
        TEXT("successful reimport advances exact profile receipt to H2"),
        ProfiledAsset != nullptr &&
        ProfiledAsset->InlinedPlacementProfiles.Num() == 1 &&
        ProfiledAsset->InlinedPlacementProfiles[0].GetAppliedSourceHash() ==
            MHRawPayloadHash(ProfileH2SameValue));

    const FString DuplicateDirectory = FPaths::Combine(SourceRoot, TEXT("duplicate"));
    IFileManager::Get().MakeDirectory(*DuplicateDirectory, true);
    const FString DuplicatePath = FPaths::Combine(
        DuplicateDirectory,
        ProfileName + TEXT(".placement"));
    bPassed &= TestTrue(
        TEXT("create ambiguous placement profile"),
        FFileHelper::SaveArrayToFile(ProfileH1, *DuplicatePath));
    MHShutdownProjectIndex();
    FMHSourceAnalysis AmbiguousAnalysis;
    bool bAmbiguousExecuted = false;
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        AmbiguousAnalysis,
        bAmbiguousExecuted);
    const FMHSourceAnalysisEntry* AmbiguousEntry = AmbiguousAnalysis.Find(ProfiledKey);
    bPassed &= TestTrue(
        TEXT("ambiguous profile blocks dependent composite"),
        AmbiguousEntry != nullptr &&
        AmbiguousEntry->Change == EMHSourceChange::Blocked &&
        !AmbiguousEntry->Errors.IsEmpty());

    bPassed &= TestTrue(
        TEXT("change profile while its key is ambiguous"),
        FFileHelper::SaveArrayToFile(ProfileH3, *PlacementPath));
    bPassed &= TestTrue(
        TEXT("remove duplicate to recover unique profile"),
        IFileManager::Get().Delete(*DuplicatePath, false, true, true));
    MHShutdownProjectIndex();
    FMHSourceAnalysis RecoveredAnalysis;
    bool bRecoveredExecuted = false;
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        RecoveredAnalysis,
        bRecoveredExecuted);
    const FMHSourceAnalysisEntry* RecoveredEntry = RecoveredAnalysis.Find(ProfiledKey);
    bPassed &= TestTrue(
        TEXT("unique recovery rechecks receipt and promotes to REIMPORT"),
        RecoveredEntry != nullptr &&
        RecoveredEntry->Change == EMHSourceChange::Reimport &&
        RecoveredEntry->Errors.IsEmpty());
    bPassed &= TestTrue(TEXT("recovered profile reimport executes"), bRecoveredExecuted);
    ProfiledAsset = LoadObject<UMHCompositeAsset>(nullptr, *ProfiledObjectPath);
    bPassed &= TestTrue(
        TEXT("recovery advances exact profile receipt to H3"),
        ProfiledAsset != nullptr &&
        ProfiledAsset->InlinedPlacementProfiles.Num() == 1 &&
        ProfiledAsset->InlinedPlacementProfiles[0].GetAppliedSourceHash() ==
            MHRawPayloadHash(ProfileH3));

    MHShutdownProjectIndex();
    FMHSourceAnalysis FinalAnalysis;
    bool bFinalExecuted = false;
    MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *Settings,
        FinalAnalysis,
        bFinalExecuted);
    const FMHSourceAnalysisEntry* FinalEntry = FinalAnalysis.Find(ProfiledKey);
    bPassed &= TestTrue(
        TEXT("fresh final receipt returns to NO_CHANGE"),
        FinalEntry != nullptr &&
        FinalEntry->Change == EMHSourceChange::NoChange &&
        FinalEntry->Errors.IsEmpty());
    bPassed &= TestFalse(TEXT("fresh final receipt executes no import"), bFinalExecuted);

    MHSetProfileFreshnessAssetLoadObserverForTests(
        TFunction<void(const FMHResourceKey&)>());
    MHShutdownProjectIndex();
    DeleteLifecycleAssetPackage(ProfiledPackage);
    DeleteLifecycleAssetPackage(PlainPackage);
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
