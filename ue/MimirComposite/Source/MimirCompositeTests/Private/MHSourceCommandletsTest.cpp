#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Diagnostics/MHAnalyzeSourcesReport.h"
#include "Diagnostics/MHSourceOperations.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace UE::MimirComposite;

namespace
{

FString Utf8BytesToString(const TArray<uint8>& Bytes)
{
    const FUTF8ToTCHAR Text(
        reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
        Bytes.Num());
    return FString(Text.Length(), Text.Get());
}

FMHSourceAnalysis BuildReportFixture()
{
    FMHSourceAnalysis Analysis;
    Analysis.Warnings = {TEXT("MH_W_Z"), TEXT("MH_W_A")};
    Analysis.Errors = {TEXT("MH_E_Z"), TEXT("MH_E_A")};

    FMHSourceAnalysisEntry& Composite = Analysis.Entries.AddDefaulted_GetRef();
    Composite.Key.Kind = EMHResourceKind::Composite;
    Composite.Key.LogicalName = TEXT("scene_a");
    Composite.Change = EMHSourceChange::Reimport;
    Composite.SourcePath = TEXT("composites/scene_a.composite");
    Composite.RawHash = TEXT("blake3-160:2222222222222222222222222222222222222222");
    Composite.Warnings = {TEXT("MH_W_NODE_Z"), TEXT("MH_W_NODE_A")};

    FMHSourceAnalysisEntry& Material = Analysis.Entries.AddDefaulted_GetRef();
    Material.Key.Kind = EMHResourceKind::Material;
    Material.Key.LogicalName = TEXT("metal_a");
    Material.Change = EMHSourceChange::NoChange;
    Material.SourcePath = TEXT("materials/metal_a.material");
    Material.RawHash = TEXT("blake3-160:1111111111111111111111111111111111111111");
    Material.Errors = {TEXT("MH_E_NODE_Z"), TEXT("MH_E_NODE_A")};
    return Analysis;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAnalyzeSourcesReportExactBytesTest,
    "Mimir.V4.Commandlets.AnalyzeReportExactBytes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAnalyzeSourcesReportExactBytesTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir"),
        TEXT("ReportExactSource"));
    SourceRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(SourceRoot);

    TArray<uint8> Bytes;
    FString Error;
    bool bPassed = TestTrue(
        TEXT("report serializes"),
        MHSerializeAnalyzeSourcesReportV4(SourceRoot, BuildReportFixture(), Bytes, Error));
    const FString Expected = FString::Printf(
        TEXT("{\"tag\":\"mh.analyze_sources:4\",\"source_root\":\"%s\",\"entries\":[")
        TEXT("{\"kind\":\"material\",\"name\":\"metal_a\",\"classification\":\"NO_CHANGE\",\"source_path\":\"materials/metal_a.material\",\"source_hash\":\"blake3-160:1111111111111111111111111111111111111111\",\"warnings\":[],\"errors\":[\"MH_E_NODE_A\",\"MH_E_NODE_Z\"]},")
        TEXT("{\"kind\":\"composite\",\"name\":\"scene_a\",\"classification\":\"REIMPORT\",\"source_path\":\"composites/scene_a.composite\",\"source_hash\":\"blake3-160:2222222222222222222222222222222222222222\",\"warnings\":[\"MH_W_NODE_A\",\"MH_W_NODE_Z\"],\"errors\":[]}"),
        *SourceRoot);
    const FString ExpectedComplete = Expected +
        TEXT("],\"warnings\":[\"MH_W_A\",\"MH_W_Z\"],\"errors\":[\"MH_E_A\",\"MH_E_Z\"]}\n");
    bPassed &= TestEqual(TEXT("report bytes are exact"), Utf8BytesToString(Bytes), ExpectedComplete);
    bPassed &= TestTrue(TEXT("exact bytes pass tag validation"), MHValidateAnalyzeSourcesReportV4(Bytes, Error));

    const FString WrongTag = TEXT("{\"tag\":\"mh.analyze_sources:1\"}\n");
    FTCHARToUTF8 WrongTagUtf8(*WrongTag);
    TArray<uint8> WrongTagBytes;
    WrongTagBytes.Append(
        reinterpret_cast<const uint8*>(WrongTagUtf8.Get()),
        WrongTagUtf8.Length());
    bPassed &= TestFalse(
        TEXT("superseded report tag is rejected"),
        MHValidateAnalyzeSourcesReportV4(WrongTagBytes, Error));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAnalyzeSourcesReportAtomicSafetyTest,
    "Mimir.V4.Commandlets.AnalyzeReportAtomicSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAnalyzeSourcesReportAtomicSafetyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    const FString Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir"),
        TEXT("SourceCommandletTests"),
        Unique,
        TEXT("Source"));
    const FString RequestedPath = FPaths::Combine(
        TEXT("SourceCommandletTests"),
        Unique,
        TEXT("report.json"));
    const FString ExpectedTarget = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mimir"), RequestedPath));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExpectedTarget), true);
    const FString OldContents = TEXT("old-report-must-survive");
    bool bPassed = TestTrue(
        TEXT("old target fixture is written"),
        FFileHelper::SaveStringToFile(OldContents, *ExpectedTarget));

    MHSetBeforeAnalyzeSourcesReportReadBackTestHook([](const FString& TempPath)
    {
        FFileHelper::SaveStringToFile(TEXT("{}"), *TempPath);
    });
    FString AbsolutePath;
    FString Error;
    bPassed &= TestFalse(
        TEXT("corrupt sibling read-back blocks replace"),
        MHWriteAnalyzeSourcesReportV4(
            SourceRoot,
            RequestedPath,
            BuildReportFixture(),
            AbsolutePath,
            Error));
    FString AfterFailure;
    bPassed &= TestTrue(
        TEXT("old target remains readable"),
        FFileHelper::LoadFileToString(AfterFailure, *ExpectedTarget));
    bPassed &= TestEqual(TEXT("old target remains byte-identical"), AfterFailure, OldContents);

    TArray<FString> Temporaries;
    IFileManager::Get().FindFiles(Temporaries, *(ExpectedTarget + TEXT(".tmp.*")), true, false);
    bPassed &= TestEqual(TEXT("failed sibling is removed"), Temporaries.Num(), 0);

    bPassed &= TestTrue(
        TEXT("validated report atomically replaces old target"),
        MHWriteAnalyzeSourcesReportV4(
            SourceRoot,
            RequestedPath,
            BuildReportFixture(),
            AbsolutePath,
            Error));
    TArray<uint8> Written;
    bPassed &= TestTrue(TEXT("new target is readable"), FFileHelper::LoadFileToArray(Written, *ExpectedTarget));
    bPassed &= TestTrue(TEXT("new target has v4 tag"), MHValidateAnalyzeSourcesReportV4(Written, Error));
    IFileManager::Get().Delete(*ExpectedTarget, false, true, true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHVerifyManagedMeshStrictPolicyTest,
    "Mimir.V4.Commandlets.VerifyManagedMeshStrictPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHVerifyManagedMeshStrictPolicyTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UStaticMesh* Mesh = NewObject<UStaticMesh>(
        GetTransientPackage(),
        NAME_None,
        RF_Transient | RF_Transactional);
    UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(
        Mesh,
        NAME_None,
        RF_Transactional);
    Receipt->bLocallyModified = true;
    Mesh->SetAssetImportData(Receipt);

    FMHSourceAnalysisEntry DevelopmentEntry;
    MHVerifyManagedStaticMeshLocalEdit(*Mesh, false, DevelopmentEntry);
    bool bPassed = TestEqual(TEXT("development audit emits one warning"), DevelopmentEntry.Warnings.Num(), 1);
    bPassed &= TestEqual(TEXT("development audit emits no error"), DevelopmentEntry.Errors.Num(), 0);

    FMHSourceAnalysisEntry StrictEntry;
    MHVerifyManagedStaticMeshLocalEdit(*Mesh, true, StrictEntry);
    bPassed &= TestEqual(TEXT("strict audit emits one failure"), StrictEntry.Errors.Num(), 1);
    bPassed &= TestTrue(
        TEXT("strict failure preserves the registered W machine code"),
        StrictEntry.Errors[0].StartsWith(TEXT("MH_W_MANAGED_STATIC_MESH_LOCALLY_MODIFIED:")));
    Mesh->SetAssetImportData(nullptr);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceCommandletExitLevelTest,
    "Mimir.V4.Commandlets.ExitLevelCore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceCommandletExitLevelTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FMHSourceAnalysis Analysis;
    Analysis.Warnings.Add(TEXT("MH_W_TEST"));
    bool bPassed = TestEqual(
        TEXT("warnings remain successful"),
        MHSourceCommandletExitCode(true, true, Analysis),
        0);
    Analysis.Errors.Add(TEXT("MH_E_TEST"));
    bPassed &= TestEqual(
        TEXT("resource errors fail"),
        MHSourceCommandletExitCode(true, true, Analysis),
        1);
    Analysis.Errors.Reset();
    bPassed &= TestEqual(
        TEXT("operation failures fail"),
        MHSourceCommandletExitCode(true, false, Analysis),
        1);
    bPassed &= TestEqual(
        TEXT("usage failures have distinct exit"),
        MHSourceCommandletExitCode(false, true, Analysis),
        2);
    return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
