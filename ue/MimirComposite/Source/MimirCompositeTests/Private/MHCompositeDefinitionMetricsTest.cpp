#include "MHGoldenRoot.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeProtocol.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{
struct FMHDefinitionMetricsFixture
{
    FAutomationTestBase& Test;
    TArray<UObject*> Assets;
    UMHCompositeAsset* Root = nullptr;

    explicit FMHDefinitionMetricsFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FMHDefinitionMetricsFixture()
    {
        for (UObject* Object : Assets)
        {
            if (!IsValid(Object)) continue;
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->MarkAsGarbage();
        }
    }

    UStaticMesh* AddMesh(const FString& Name)
    {
        UStaticMesh* Mesh = NewObject<UStaticMesh>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + Name)),
            FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Mesh);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = Name;
        Receipt->SourceRelativePath = Name + TEXT(".mesh.fbx");
        Receipt->SourceHash = TEXT("blake3-160:0123456789012345678901234567890123456789");
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);
        return Mesh;
    }

    UMHCompositeAsset* AddComposite(const FString& Name, TConstArrayView<uint8> SourceBytes)
    {
        FMHCompositeDocument Document;
        FString Error;
        if (!MHParseCompositeV5(SourceBytes, Document, Error))
        {
            Test.AddError(Name + TEXT(" parse failed: ") + Error);
            return nullptr;
        }
        UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + Name)),
            FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        if (!MHApplyCompositeV5(*Asset, Document, Error))
        {
            Test.AddError(Name + TEXT(" apply failed: ") + Error);
            return nullptr;
        }
        TArray<uint8> AppliedBytes;
        if (!MHWriteCanonicalCompositeV5(Document, AppliedBytes, Error))
        {
            Test.AddError(Name + TEXT(" canonical write failed: ") + Error);
            return nullptr;
        }
        Asset->LogicalName = Name;
        Asset->SourceRelativePath = Name + TEXT(".composite");
        Asset->SourceHash = MHRawPayloadHash(SourceBytes);
        Asset->AppliedHash = MHRawPayloadHash(AppliedBytes);
        return Asset;
    }

    UMHCompositeAsset* AddComposite(const FString& Name, const FMHCompositeDocument& Document)
    {
        TArray<uint8> Bytes;
        FString Error;
        if (!MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(Name + TEXT(" canonical write failed: ") + Error);
            return nullptr;
        }
        return AddComposite(Name, Bytes);
    }

    bool BuildSynthetic(const int32 TopLevelNodes)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        const FString RootName = TEXT("s65_metrics_") + Suffix;
        const FString MeshName = RootName + TEXT("_mesh");
        AddMesh(MeshName);
        FMHCompositeDocument Document;
        for (int32 Index = 0; Index < TopLevelNodes; ++Index)
        {
            FMHCompositeNode& Group = Document.Nodes.AddDefaulted_GetRef();
            Group.Kind = EMHCompositeNodeKind::Group;
            Group.Name = FString::Printf(TEXT("group_%d"), Index);
            Group.Transform.TranslationCm = FVector(200.0 * Index, 0.0, 0.0);
            FMHCompositeNode& Leaf = Group.Children.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshName;
        }
        Root = AddComposite(RootName, Document);
        return Root != nullptr;
    }

    bool BuildGaz53()
    {
        FString GoldenRoot;
        if (!ResolveGoldenRoot(Test, GoldenRoot)) return false;
        AddMesh(TEXT("gaz53_b_body"));
        for (const TCHAR* Name : {
            TEXT("gaz53_b_random_cmp"), TEXT("gaz53_b_body_cmp"), TEXT("gaz53_body_bc_random_cmp")})
        {
            TArray<uint8> Bytes;
            const FString Path = FPaths::Combine(
                GoldenRoot, TEXT("v5/gaz53"), FString(Name) + TEXT(".composite"));
            if (!FFileHelper::LoadFileToArray(Bytes, *Path))
            {
                Test.AddError(TEXT("cannot read frozen GAZ-53 document: ") + Path);
                return false;
            }
            UMHCompositeAsset* Asset = AddComposite(Name, Bytes);
            if (Asset == nullptr) return false;
            if (FString(Name) == TEXT("gaz53_b_random_cmp")) Root = Asset;
        }
        for (const TCHAR* Name : {
            TEXT("gaz53_bread_b_cmp"), TEXT("gaz53_wooden_b_cmp"), TEXT("gaz53_wooden_c_cmp")})
        {
            FMHCompositeDocument Document;
            FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = TEXT("gaz53_b_body");
            if (AddComposite(Name, Document) == nullptr) return false;
        }
        return Root != nullptr;
    }
};

bool DefinitionMetricsIsolatedHost(FAutomationTestBase& Test)
{
    if (GEditor != nullptr &&
        FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) == TEXT("MimirCompositeV5S6")) return true;
    Test.AddInfo(TEXT("definition metrics NOT RUN: requires the isolated MimirCompositeV5S6 editor host"));
    return false;
}

FString DefinitionMetricsLine(
    const TCHAR* Scenario, const int32 Placements, const double WallMilliseconds,
    const FMHPlacementStageMetrics& Metrics)
{
    FString Result = FString::Printf(
        TEXT("S65_METRICS scenario=%s placements=%d wall_ms=%.3f"),
        Scenario, Placements, WallMilliseconds);
    for (int32 Index = 0; Index < FMHPlacementStageMetrics::StageCount; ++Index)
    {
        const EMHPlacementStage Stage = static_cast<EMHPlacementStage>(Index);
        const FMHPlacementStageMetric& Metric = Metrics.Get(Stage);
        Result += FString::Printf(
            TEXT(" %s[calls=%llu,inclusive_ms=%.6f,exclusive_ms=%.6f]"),
            MHPlacementStageLabel(Stage), Metric.Calls,
            FPlatformTime::ToMilliseconds64(Metric.InclusiveCycles),
            FPlatformTime::ToMilliseconds64(Metric.ExclusiveCycles));
    }
    return Result;
}

bool DefinitionMetricsPlaceActors(
    FAutomationTestBase& Test, UMHCompositeAsset& Asset, const int32 PlacementCount,
    FMHPlacementStageMetrics& OutMetrics, double& OutWallMilliseconds)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!Test.TestNotNull(TEXT("definition metrics preview world"), World)) return false;
    TArray<AMHCompositeActor*> Actors;
    for (int32 Index = 0; Index < PlacementCount; ++Index)
    {
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
        if (!Test.TestNotNull(TEXT("definition metrics placement"), Actor))
        {
            World->DestroyWorld(true);
            return false;
        }
        Actor->SetAutoSeed(false);
        Actors.Add(Actor);
    }

    MHResetPlacementStageMetrics();
    const double StartSeconds = FPlatformTime::Seconds();
    bool bPassed = true;
    for (AMHCompositeActor* Actor : Actors)
    {
        Actor->SetCompositeAsset(&Asset);
        bPassed &= Test.TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan());
    }
    OutWallMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
    OutMetrics = MHGetPlacementStageMetrics();
    World->DestroyWorld(true);
    return bPassed;
}

bool DefinitionMetricsCommonAssertions(
    FAutomationTestBase& Test, const FMHPlacementStageMetrics& Metrics,
    const uint64 PlacementCount, const uint64 RegisteredComponents)
{
    bool bPassed = true;
    const uint64 GraphBuilds = Metrics.Get(EMHPlacementStage::BuildAppliedGraph).Calls;
    bPassed &= Test.TestTrue(TEXT("at least one applied graph admission is measured"), GraphBuilds >= 1);
    bPassed &= Test.TestTrue(TEXT("graph admissions never exceed placements"), GraphBuilds <= PlacementCount);
    bPassed &= Test.TestEqual(TEXT("each placement resolves its own plan"),
        Metrics.Get(EMHPlacementStage::ResolveCompositePlan).Calls, PlacementCount);
    bPassed &= Test.TestEqual(TEXT("each placement loads its endpoints"),
        Metrics.Get(EMHPlacementStage::LoadEndpoints).Calls, PlacementCount);
    bPassed &= Test.TestEqual(TEXT("each placement compiles its component delta"),
        Metrics.Get(EMHPlacementStage::CompilePlacement).Calls, PlacementCount);
    bPassed &= Test.TestEqual(TEXT("new placement components are registered exactly once"),
        Metrics.Get(EMHPlacementStage::RegisterComponents).Calls, RegisteredComponents);
    bPassed &= Test.TestEqual(TEXT("each placement runs one retirement pass"),
        Metrics.Get(EMHPlacementStage::DestroyRetiredComponents).Calls, PlacementCount);
    return bPassed;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDefinitionMetricsSyntheticTest,
    "Mimir.V5.Composite.DefinitionPool.InstrumentationSynthetic100",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDefinitionMetricsSyntheticTest::RunTest(const FString& Parameters)
{
    if (!DefinitionMetricsIsolatedHost(*this)) return true;
    constexpr int32 PlacementCount = 100;
    constexpr int32 TopLevelNodes = 3;
    FMHDefinitionMetricsFixture Fixture(*this);
    if (!Fixture.BuildSynthetic(TopLevelNodes)) return false;
    FMHPlacementStageMetrics Metrics;
    double WallMilliseconds = 0.0;
    bool bPassed = DefinitionMetricsPlaceActors(
        *this, *Fixture.Root, PlacementCount, Metrics, WallMilliseconds);
    AddInfo(DefinitionMetricsLine(TEXT("synthetic100"), PlacementCount, WallMilliseconds, Metrics));
    bPassed &= DefinitionMetricsCommonAssertions(
        *this, Metrics, PlacementCount, PlacementCount * TopLevelNodes * 2);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDefinitionMetricsGaz53Test,
    "Mimir.V5.Composite.DefinitionPool.InstrumentationGaz53",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDefinitionMetricsGaz53Test::RunTest(const FString& Parameters)
{
    if (!DefinitionMetricsIsolatedHost(*this)) return true;
    constexpr int32 PlacementCount = 2;
    FMHDefinitionMetricsFixture Fixture(*this);
    if (!Fixture.BuildGaz53()) return false;
    FMHPlacementStageMetrics Metrics;
    double WallMilliseconds = 0.0;
    bool bPassed = DefinitionMetricsPlaceActors(
        *this, *Fixture.Root, PlacementCount, Metrics, WallMilliseconds);
    AddInfo(DefinitionMetricsLine(TEXT("gaz53_two_placements"), PlacementCount, WallMilliseconds, Metrics));
    bPassed &= DefinitionMetricsCommonAssertions(*this, Metrics, PlacementCount, 8);
    return bPassed;
}
} // namespace UE::MimirComposite::Tests
