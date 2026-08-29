#include "MHGoldenRoot.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "Source/MHSourceImportMetrics.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{

constexpr int32 BulkTextureCount = 70;
constexpr int32 BulkMeshCount = 30;

FString BulkGeneratedPackageName(const FMHResourceKey& Key)
{
    const TCHAR* Root = Key.Kind == EMHResourceKind::Texture
        ? TEXT("/Game/MH/Generated/Textures/")
        : TEXT("/Game/MH/Generated/Meshes/");
    return FString(Root) + Key.LogicalName;
}

void DeleteBulkGeneratedPackage(const FMHResourceKey& Key)
{
    const FString PackageName = BulkGeneratedPackageName(Key);
    const FString ObjectPath = PackageName + TEXT(".") + Key.LogicalName;
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

bool MakePngBytes(const FColor Color, TArray<uint8>& OutBytes)
{
    TArray64<FColor> Pixels;
    Pixels.Add(Color);
    TArray64<uint8> Bytes64;
    FImageUtils::PNGCompressImageArray(1, 1, Pixels, Bytes64);
    OutBytes.Reset(Bytes64.Num());
    OutBytes.Append(Bytes64.GetData(), Bytes64.Num());
    return !OutBytes.IsEmpty();
}

struct FBulkImportFixture
{
    FString SourceRoot;
    FMHImportSourcesScope Scope;

    ~FBulkImportFixture()
    {
        for (const FMHResourceKey& Key : Scope.ResourceKeys)
        {
            DeleteBulkGeneratedPackage(Key);
        }
        FString IgnoredError;
        MHRefreshGeneratedAssetProjection(SourceRoot, IgnoredError);
        MHShutdownProjectIndex();
        IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    }

    bool BuildLarge(FAutomationTestBase& Test)
    {
        FString GoldenRoot;
        if (!ResolveGoldenRoot(Test, GoldenRoot))
        {
            return false;
        }
        TArray<uint8> MeshBytes;
        if (!FFileHelper::LoadFileToArray(
                MeshBytes,
                *FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"))))
        {
            Test.AddError(TEXT("could not read axis_probe.fbx"));
            return false;
        }
        TArray<uint8> PngBytes;
        if (!MakePngBytes(FColor(17, 83, 191, 255), PngBytes))
        {
            Test.AddError(TEXT("could not encode bulk texture fixture"));
            return false;
        }

        const FString Token = TEXT("bulk_") +
            FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("MimirCompositeTests/bulk_import"),
            Token));
        FPaths::NormalizeDirectoryName(SourceRoot);
        IFileManager::Get().MakeDirectory(*SourceRoot, true);

        for (int32 Index = 0; Index < BulkTextureCount; ++Index)
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::Texture;
            Key.LogicalName = FString::Printf(TEXT("%s_tex_%03d"), *Token, Index);
            Scope.ResourceKeys.Add(Key);
            if (!FFileHelper::SaveArrayToFile(
                    PngBytes,
                    *FPaths::Combine(SourceRoot, Key.LogicalName + TEXT(".png"))))
            {
                Test.AddError(FString::Printf(TEXT("could not write %s"), *Key.ToString()));
                return false;
            }
        }
        for (int32 Index = 0; Index < BulkMeshCount; ++Index)
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::StaticMesh;
            Key.LogicalName = FString::Printf(TEXT("%s_mesh_%03d"), *Token, Index);
            Scope.ResourceKeys.Add(Key);
            if (!FFileHelper::SaveArrayToFile(
                    MeshBytes,
                    *FPaths::Combine(SourceRoot, Key.LogicalName + TEXT(".mesh.fbx"))))
            {
                Test.AddError(FString::Printf(TEXT("could not write %s"), *Key.ToString()));
                return false;
            }
        }
        return true;
    }

    bool BuildSingleTexture(FAutomationTestBase& Test)
    {
        const FString Token = TEXT("bulk_crash_") +
            FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        SourceRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("MimirCompositeTests/bulk_import"),
            Token));
        FPaths::NormalizeDirectoryName(SourceRoot);
        IFileManager::Get().MakeDirectory(*SourceRoot, true);
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::Texture;
        Key.LogicalName = Token;
        Scope.ResourceKeys.Add(Key);
        TArray<uint8> PngBytes;
        if (!MakePngBytes(FColor::Red, PngBytes) ||
            !FFileHelper::SaveArrayToFile(PngBytes, *TextureSourcePath()))
        {
            Test.AddError(TEXT("could not create crash fixture texture"));
            return false;
        }
        return true;
    }

    FString TextureSourcePath() const
    {
        return FPaths::Combine(SourceRoot, Scope.ResourceKeys[0].LogicalName + TEXT(".png"));
    }
};

double MetricMilliseconds(const FMHSourceImportMetric& Metric)
{
    return FPlatformTime::ToMilliseconds64(Metric.InclusiveCycles);
}

bool EntriesHaveNoErrors(
    FAutomationTestBase& Test,
    const FMHSourceAnalysis& Analysis,
    const TCHAR* Label)
{
    bool bClean = true;
    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        for (const FString& Error : Entry.Errors)
        {
            Test.AddError(FString::Printf(
                TEXT("%s %s: %s"),
                Label,
                *Entry.Key.ToString(),
                *Error));
            bClean = false;
        }
    }
    return bClean;
}

void ReportMetrics(FAutomationTestBase& Test, const TCHAR* Label, const FMHSourceImportMetrics& Metrics)
{
    for (int32 ResourceIndex = 0;
         ResourceIndex < static_cast<int32>(EMHSourceImportMetricResource::Count);
         ++ResourceIndex)
    {
        const EMHSourceImportMetricResource Resource =
            static_cast<EMHSourceImportMetricResource>(ResourceIndex);
        for (int32 StageIndex = 0;
             StageIndex < static_cast<int32>(EMHSourceImportMetricStage::Count);
             ++StageIndex)
        {
            const EMHSourceImportMetricStage Stage =
                static_cast<EMHSourceImportMetricStage>(StageIndex);
            const FMHSourceImportMetric& Metric = Metrics.Get(Resource, Stage);
            if (Metric.Calls == 0)
            {
                continue;
            }
            Test.AddInfo(FString::Printf(
                TEXT("BULK_IMPORT_METRIC %s %s.%s calls=%llu inclusive_ms=%.3f exclusive_ms=%.3f"),
                Label,
                MHSourceImportMetricResourceLabel(Resource),
                MHSourceImportMetricStageLabel(Stage),
                Metric.Calls,
                MetricMilliseconds(Metric),
                FPlatformTime::ToMilliseconds64(Metric.ExclusiveCycles)));
        }
    }
    Test.AddInfo(FString::Printf(
        TEXT("BULK_IMPORT_PROGRESS %s scopes=%llu resource_ticks=%llu"),
        Label,
        Metrics.ProgressScopes,
        Metrics.ProgressResourceTicks));
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceBulkImportLargeBatchTest,
    "Mimir.V4.BulkImport.LargeBatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceBulkImportLargeBatchTest::RunTest(const FString& Parameters)
{
    FBulkImportFixture Fixture;
    if (!Fixture.BuildLarge(*this))
    {
        return false;
    }

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHSourceAnalysis FirstAnalysis;
    bool bFirstExecuted = false;
    MHShutdownProjectIndex();
    MHResetSourceImportMetrics();
    const double FirstStart = FPlatformTime::Seconds();
    const bool bFirstSucceeded = MHImportSourcesHeadless(
        Fixture.SourceRoot,
        Fixture.Scope,
        *Settings,
        FirstAnalysis,
        bFirstExecuted);
    const double FirstWallSeconds = FPlatformTime::Seconds() - FirstStart;
    const FMHSourceImportMetrics FirstMetrics = MHGetSourceImportMetrics();
    AddInfo(FString::Printf(
        TEXT("BULK_IMPORT_WALL before resources=%d textures=%d meshes=%d seconds=%.3f"),
        Fixture.Scope.ResourceKeys.Num(),
        BulkTextureCount,
        BulkMeshCount,
        FirstWallSeconds));
    ReportMetrics(*this, TEXT("before"), FirstMetrics);

    // The shared host intentionally retains unrelated generated claims from
    // earlier protocol tests. Those can populate Analysis.Errors and make the
    // all-project return false; only the explicitly scoped rows are the oracle.
    (void)bFirstSucceeded;
    bool bPassed = TestTrue(TEXT("first bulk import executes"), bFirstExecuted);
    bPassed &= TestEqual(TEXT("scoped plan has 100 resources"), FirstAnalysis.Entries.Num(), 100);
    bPassed &= EntriesHaveNoErrors(*this, FirstAnalysis, TEXT("first import"));
    bPassed &= TestEqual(
        TEXT("texture create timers cover every resource"),
        FirstMetrics.Get(
            EMHSourceImportMetricResource::Texture,
            EMHSourceImportMetricStage::Create).Calls,
        static_cast<uint64>(BulkTextureCount));
    bPassed &= TestEqual(
        TEXT("mesh create timers cover every resource"),
        FirstMetrics.Get(
            EMHSourceImportMetricResource::StaticMesh,
            EMHSourceImportMetricStage::Create).Calls,
        static_cast<uint64>(BulkMeshCount));

    // Acceptance targets. These assertions are deliberately red before the
    // three-pass implementation: today every asset waits and saves alone.
    bPassed &= TestEqual(
        TEXT("one compilation wait for the whole batch"),
        FirstMetrics.CallsForStage(EMHSourceImportMetricStage::BuildWait),
        1ull);
    bPassed &= TestEqual(
        TEXT("one package save call for the whole batch"),
        FirstMetrics.CallsForStage(EMHSourceImportMetricStage::SavePackage),
        1ull);
    bPassed &= TestEqual(TEXT("one progress scope"), FirstMetrics.ProgressScopes, 1ull);
    bPassed &= TestEqual(
        TEXT("one progress tick per resource"),
        FirstMetrics.ProgressResourceTicks,
        100ull);

    FMHSourceAnalysis SecondAnalysis;
    bool bSecondExecuted = false;
    MHResetSourceImportMetrics();
    const double SecondStart = FPlatformTime::Seconds();
    const bool bSecondSucceeded = MHImportSourcesHeadless(
        Fixture.SourceRoot,
        Fixture.Scope,
        *Settings,
        SecondAnalysis,
        bSecondExecuted);
    const double SecondWallSeconds = FPlatformTime::Seconds() - SecondStart;
    const FMHSourceImportMetrics SecondMetrics = MHGetSourceImportMetrics();
    AddInfo(FString::Printf(
        TEXT("BULK_IMPORT_WALL no_change resources=%d seconds=%.3f"),
        Fixture.Scope.ResourceKeys.Num(),
        SecondWallSeconds));
    ReportMetrics(*this, TEXT("no_change"), SecondMetrics);
    (void)bSecondSucceeded;
    bPassed &= EntriesHaveNoErrors(*this, SecondAnalysis, TEXT("no-change import"));
    bPassed &= TestFalse(TEXT("no-change bulk import executes no mutations"), bSecondExecuted);
    bPassed &= TestEqual(
        TEXT("no-change bulk import has no waits"),
        SecondMetrics.CallsForStage(EMHSourceImportMetricStage::BuildWait),
        0ull);
    bPassed &= TestEqual(
        TEXT("no-change bulk import has no saves"),
        SecondMetrics.CallsForStage(EMHSourceImportMetricStage::SavePackage),
        0ull);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceBulkImportCrashRecoveryTest,
    "Mimir.V4.BulkImport.CrashBetweenPassesRetries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceBulkImportCrashRecoveryTest::RunTest(const FString& Parameters)
{
    FBulkImportFixture Fixture;
    if (!Fixture.BuildSingleTexture(*this))
    {
        return false;
    }

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    MHShutdownProjectIndex();
    FMHSourceAnalysis InitialAnalysis;
    bool bInitialExecuted = false;
    MHImportSourcesHeadless(
        Fixture.SourceRoot,
        Fixture.Scope,
        *Settings,
        InitialAnalysis,
        bInitialExecuted);
    bool bPassed = EntriesHaveNoErrors(*this, InitialAnalysis, TEXT("initial import"));
    bPassed &= TestTrue(TEXT("initial texture import executes"), bInitialExecuted);

    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        BulkGeneratedPackageName(Fixture.Scope.ResourceKeys[0]),
        FPackageName::GetAssetPackageExtension());
    TArray<uint8> BeforePackageBytes;
    bPassed &= TestTrue(
        TEXT("read package before injected interruption"),
        FFileHelper::LoadFileToArray(BeforePackageBytes, *PackageFilename));

    TArray<uint8> ReplacementPng;
    bPassed &= TestTrue(TEXT("encode replacement PNG"), MakePngBytes(FColor::Green, ReplacementPng));
    bPassed &= TestTrue(
        TEXT("write changed texture source"),
        FFileHelper::SaveArrayToFile(ReplacementPng, *Fixture.TextureSourcePath()));
    MHShutdownProjectIndex();
    bool bInterruptionObserved = false;
    MHSetBulkImportPhaseTestHook([&bInterruptionObserved](const EMHSourceBulkImportPhase Phase)
    {
        bInterruptionObserved |= Phase == EMHSourceBulkImportPhase::AssetsPrepared;
        return Phase != EMHSourceBulkImportPhase::AssetsPrepared;
    });
    FMHSourceAnalysis InterruptedAnalysis;
    bool bInterruptedExecuted = false;
    const bool bInterruptedSucceeded = MHImportSourcesHeadless(
        Fixture.SourceRoot,
        Fixture.Scope,
        *Settings,
        InterruptedAnalysis,
        bInterruptedExecuted);
    MHSetBulkImportPhaseTestHook(TFunction<bool(EMHSourceBulkImportPhase)>());
    (void)bInterruptedSucceeded;
    bPassed &= TestTrue(TEXT("injected pass boundary was observed"), bInterruptionObserved);

    TArray<uint8> AfterInterruptedPackageBytes;
    bPassed &= TestTrue(
        TEXT("read package after injected interruption"),
        FFileHelper::LoadFileToArray(AfterInterruptedPackageBytes, *PackageFilename));
    bPassed &= TestTrue(
        TEXT("interruption before save leaves package bytes unchanged"),
        BeforePackageBytes == AfterInterruptedPackageBytes);

    FMHSourceAnalysis RetryAnalysis;
    bool bRetryExecuted = false;
    const bool bRetrySucceeded = MHImportSourcesHeadless(
        Fixture.SourceRoot,
        Fixture.Scope,
        *Settings,
        RetryAnalysis,
        bRetryExecuted);
    (void)bRetrySucceeded;
    bPassed &= EntriesHaveNoErrors(*this, RetryAnalysis, TEXT("retry import"));
    bPassed &= TestTrue(TEXT("unfinished resource is reimported on retry"), bRetryExecuted);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
