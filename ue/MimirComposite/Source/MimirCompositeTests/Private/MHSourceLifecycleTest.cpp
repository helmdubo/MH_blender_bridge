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
    bPassed &= TestEqual(TEXT("EndPIE flushes queued batch"), Batches.Num(), 2);
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
    Importer->SetStartupExecutorForTests([&StartupAttempts]()
    {
        ++StartupAttempts;
        return StartupAttempts >= 2;
    });
    Importer->SetAssetRegistryReadyForTests(false);
    Importer->TickSourceLifecycleForTests();
    bool bPassed = TestEqual(TEXT("startup waits for Asset Registry"), StartupAttempts, 0);
    Importer->SetAssetRegistryReadyForTests(true);
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("first startup attempt is retryable"), StartupAttempts, 1);
    bPassed &= TestFalse(TEXT("failed attempt is not marked complete"), Importer->HasStartupPlanRunForTests());
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("startup retries once"), StartupAttempts, 2);
    bPassed &= TestTrue(TEXT("successful startup is complete"), Importer->HasStartupPlanRunForTests());
    Importer->TickSourceLifecycleForTests();
    bPassed &= TestEqual(TEXT("completed startup is exactly once"), StartupAttempts, 2);

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
        EMHResourceKind::Texture,
        EMHResourceKind::Material,
        EMHResourceKind::StaticMesh,
        EMHResourceKind::Composite};
    bPassed &= TestTrue(TEXT("coordinator stage order is T-M-SM-C"), Stages == Expected);
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
    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests/lifecycle_echo"),
        Token);
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString SourcePath = FPaths::Combine(SourceRoot, Token + TEXT(".composite"));
    const TArray<uint8> Initial = LifecycleUtf8(TEXT("{\n  \"nodes\": []\n}\n"));
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
    bPassed &= TestTrue(TEXT("apply local source-shaped edit"), MHApplyCompositeV4(*Asset, Edited, Error));
    const FMHCompositeOperationResult Published = MHPublishCompositeV4(*Asset, SourceRoot);
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
    bPassed &= TestTrue(
        TEXT("publish preserves managed UObject identity"),
        LoadObject<UMHCompositeAsset>(nullptr, *ObjectPath) == PublishedIdentity);

    MHShutdownProjectIndex();
    DeleteLifecycleAssetPackage(PackageName);
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
