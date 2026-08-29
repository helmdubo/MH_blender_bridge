#include "MHGoldenRoot.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
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

    bool ReimportRootWithEquivalentSource()
    {
        if (Root == nullptr) return false;
        FMHCompositeDocument Document;
        FString Error;
        TArray<uint8> SourceBytes;
        if (!MHExtractCompositeV5(*Root, Document, Error) ||
            !MHWriteCanonicalCompositeV5(Document, SourceBytes, Error))
        {
            Test.AddError(TEXT("cannot prepare equivalent root reimport: ") + Error);
            return false;
        }
        // Source whitespace changes the raw receipt/closure hash while the
        // extracted document and its AppliedHash remain byte-identical.
        SourceBytes.Add(static_cast<uint8>(' '));
        const FString PreviousSourceHash = Root->SourceHash;
        Root->SourceHash = MHRawPayloadHash(SourceBytes);
        if (Root->SourceHash == PreviousSourceHash)
        {
            Test.AddError(TEXT("equivalent reimport did not change the source receipt"));
            return false;
        }
        return true;
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

bool DefinitionDecisionEqual(
    const FMHResolvedCompositeDecision& Left, const FMHResolvedCompositeDecision& Right)
{
    return Left.NodePath == Right.NodePath && Left.OptionIndex == Right.OptionIndex &&
        Left.Weights == Right.Weights && Left.Total == Right.Total &&
        Left.RawU32 == Right.RawU32 && Left.Unit == Right.Unit && Left.Target == Right.Target;
}

bool DefinitionDrawEqual(
    const FMHResolvedCompositeDraw& Left, const FMHResolvedCompositeDraw& Right)
{
    return Left.NodePath == Right.NodePath && Left.Role == Right.Role &&
        Left.RawU32 == Right.RawU32 && Left.Unit == Right.Unit && Left.Sample == Right.Sample;
}

bool DefinitionAppearanceDrawEqual(
    const FMHResolvedCompositeAppearanceDraw& Left,
    const FMHResolvedCompositeAppearanceDraw& Right)
{
    return Left.NodePath == Right.NodePath && Left.BoundaryPath == Right.BoundaryPath &&
        Left.Channel == Right.Channel && Left.RawU32 == Right.RawU32 && Left.Unit == Right.Unit;
}

bool DefinitionPlanParity(
    FAutomationTestBase& Test, const AMHCompositeActor& Actor, const UMHCompositeAsset& Asset)
{
    const FMHResolvedCompositePlan* SharedPlan = Actor.GetResolvedPlan();
    if (!Test.TestNotNull(TEXT("shared placement exposes a current plan"), SharedPlan)) return false;
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (!Test.TestNotNull(TEXT("definition settings"), Settings)) return false;
    FMHRandomSourceGraph FreshGraph;
    TSet<FMHResourceKey> FreshDependencies;
    FString Error;
    if (!MHBuildAppliedCompositeGraph(Asset, *Settings, FreshGraph, FreshDependencies, Error))
    {
        Test.AddError(TEXT("fresh graph rebuild failed: ") + Error);
        return false;
    }
    FMHResolvedCompositePlan FreshPlan;
    if (!MHResolveCompositePlan(
        FreshGraph, Actor.GetSeed(), Actor.GetAppearanceSeed(), FreshPlan, Error))
    {
        Test.AddError(TEXT("fresh plan resolve failed: ") + Error);
        return false;
    }

    bool bPassed = true;
    bPassed &= Test.TestEqual(TEXT("resolved signature matches a fresh build"),
        SharedPlan->ResolvedSignature, FreshPlan.ResolvedSignature);
    bPassed &= Test.TestEqual(TEXT("appearance signature matches a fresh build"),
        SharedPlan->Appearance.AppearanceSignature, FreshPlan.Appearance.AppearanceSignature);
    bPassed &= Test.TestEqual(TEXT("placement signature matches a fresh build"),
        SharedPlan->PlacementSignature, FreshPlan.PlacementSignature);
    bPassed &= Test.TestTrue(TEXT("layout signature bytes match a fresh build"),
        SharedPlan->SignaturePreimage == FreshPlan.SignaturePreimage);
    bPassed &= Test.TestTrue(TEXT("appearance signature bytes match a fresh build"),
        SharedPlan->Appearance.SignaturePreimage == FreshPlan.Appearance.SignaturePreimage);
    bPassed &= Test.TestTrue(TEXT("closure bytes match a fresh build"),
        SharedPlan->Closure.Resources == FreshPlan.Closure.Resources &&
        SharedPlan->Closure.OrderedRawHashes == FreshPlan.Closure.OrderedRawHashes &&
        SharedPlan->Closure.HashPreimage == FreshPlan.Closure.HashPreimage &&
        SharedPlan->Closure.ClosureHash == FreshPlan.Closure.ClosureHash);
    bPassed &= Test.TestEqual(TEXT("decision count matches a fresh build"),
        SharedPlan->Decisions.Num(), FreshPlan.Decisions.Num());
    for (int32 Index = 0; Index < FMath::Min(SharedPlan->Decisions.Num(), FreshPlan.Decisions.Num()); ++Index)
        bPassed &= Test.TestTrue(TEXT("choices match a fresh build"),
            DefinitionDecisionEqual(SharedPlan->Decisions[Index], FreshPlan.Decisions[Index]));
    bPassed &= Test.TestEqual(TEXT("draw count matches a fresh build"),
        SharedPlan->Draws.Num(), FreshPlan.Draws.Num());
    for (int32 Index = 0; Index < FMath::Min(SharedPlan->Draws.Num(), FreshPlan.Draws.Num()); ++Index)
        bPassed &= Test.TestTrue(TEXT("draws match a fresh build"),
            DefinitionDrawEqual(SharedPlan->Draws[Index], FreshPlan.Draws[Index]));
    bPassed &= Test.TestEqual(TEXT("appearance draw count matches a fresh build"),
        SharedPlan->Appearance.Draws.Num(), FreshPlan.Appearance.Draws.Num());
    for (int32 Index = 0;
         Index < FMath::Min(SharedPlan->Appearance.Draws.Num(), FreshPlan.Appearance.Draws.Num()); ++Index)
        bPassed &= Test.TestTrue(TEXT("appearance draws match a fresh build"),
            DefinitionAppearanceDrawEqual(
                SharedPlan->Appearance.Draws[Index], FreshPlan.Appearance.Draws[Index]));
    bPassed &= Test.TestEqual(TEXT("leaf count matches a fresh build"),
        SharedPlan->Leaves.Num(), FreshPlan.Leaves.Num());
    for (int32 Index = 0; Index < FMath::Min(SharedPlan->Leaves.Num(), FreshPlan.Leaves.Num()); ++Index)
    {
        const FMHResolvedCompositeLeaf& Left = SharedPlan->Leaves[Index];
        const FMHResolvedCompositeLeaf& Right = FreshPlan.Leaves[Index];
        bPassed &= Test.TestTrue(TEXT("leaf identity matches a fresh build"),
            Left.Kind == Right.Kind && Left.Resource == Right.Resource && Left.Origin == Right.Origin);
        bPassed &= Test.TestTrue(TEXT("leaf world matrix is byte-identical to a fresh build"),
            FMemory::Memcmp(&Left.WorldMatrix, &Right.WorldMatrix, sizeof(FMatrix)) == 0);
        bPassed &= Test.TestTrue(TEXT("leaf appearance channels are byte-identical to a fresh build"),
            FMemory::Memcmp(
                Left.AppearanceChannels, Right.AppearanceChannels,
                sizeof(Left.AppearanceChannels)) == 0);
    }
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDefinitionPoolSharedHundredTest,
    "Mimir.V5.Composite.DefinitionPool.SharedGraphAcross100Placements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDefinitionPoolSharedHundredTest::RunTest(const FString& Parameters)
{
    if (!DefinitionMetricsIsolatedHost(*this)) return true;
    constexpr int32 PlacementCount = 100;
    FMHDefinitionMetricsFixture Fixture(*this);
    if (!Fixture.BuildSynthetic(3)) return false;
    FMHPlacementStageMetrics Metrics;
    double WallMilliseconds = 0.0;
    bool bPassed = DefinitionMetricsPlaceActors(
        *this, *Fixture.Root, PlacementCount, Metrics, WallMilliseconds);
    AddInfo(DefinitionMetricsLine(TEXT("acceptance_shared100"), PlacementCount, WallMilliseconds, Metrics));
    bPassed &= TestEqual(TEXT("100 placements share one closure build"),
        Metrics.Get(EMHPlacementStage::BuildAppliedGraph).Calls, 1ull);
    bPassed &= TestEqual(TEXT("100 placements still resolve independently"),
        Metrics.Get(EMHPlacementStage::ResolveCompositePlan).Calls,
        static_cast<uint64>(PlacementCount));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDefinitionPoolDragEmulationTest,
    "Mimir.V5.Composite.DefinitionPool.DragPreviewAndDropShareGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDefinitionPoolDragEmulationTest::RunTest(const FString& Parameters)
{
    if (!DefinitionMetricsIsolatedHost(*this)) return true;
    FMHDefinitionMetricsFixture Fixture(*this);
    if (!Fixture.BuildSynthetic(3)) return false;
    FMHPlacementStageMetrics Metrics;
    double WallMilliseconds = 0.0;
    bool bPassed = DefinitionMetricsPlaceActors(*this, *Fixture.Root, 2, Metrics, WallMilliseconds);
    AddInfo(DefinitionMetricsLine(TEXT("acceptance_drag_two_spawns"), 2, WallMilliseconds, Metrics));
    bPassed &= TestEqual(TEXT("drag preview and final drop share one closure build"),
        Metrics.Get(EMHPlacementStage::BuildAppliedGraph).Calls, 1ull);
    bPassed &= TestEqual(TEXT("both drag actors resolve independently"),
        Metrics.Get(EMHPlacementStage::ResolveCompositePlan).Calls, 2ull);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDefinitionPoolTargetedInvalidationTest,
    "Mimir.V5.Composite.DefinitionPool.TargetedReimportInvalidatesOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDefinitionPoolTargetedInvalidationTest::RunTest(const FString& Parameters)
{
    if (!DefinitionMetricsIsolatedHost(*this)) return true;
    FMHDefinitionMetricsFixture ChangedFixture(*this);
    FMHDefinitionMetricsFixture UnrelatedFixture(*this);
    if (!ChangedFixture.BuildSynthetic(3) || !UnrelatedFixture.BuildSynthetic(2)) return false;

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("targeted invalidation preview world"), World)) return false;
    TArray<AMHCompositeActor*> ChangedActors;
    TArray<AMHCompositeActor*> UnrelatedActors;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
        Actor->SetAutoSeed(false);
        Actor->SetCompositeAsset(ChangedFixture.Root);
        ChangedActors.Add(Actor);
    }
    for (int32 Index = 0; Index < 2; ++Index)
    {
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
        Actor->SetAutoSeed(false);
        Actor->SetCompositeAsset(UnrelatedFixture.Root);
        UnrelatedActors.Add(Actor);
    }
    TArray<uint32> ChangedRebuilds;
    TArray<uint32> UnrelatedRebuilds;
    for (const AMHCompositeActor* Actor : ChangedActors)
        ChangedRebuilds.Add(Actor->GetPlacementRebuildCount());
    for (const AMHCompositeActor* Actor : UnrelatedActors)
        UnrelatedRebuilds.Add(Actor->GetPlacementRebuildCount());

    bool bPassed = ChangedFixture.ReimportRootWithEquivalentSource();
    MHResetPlacementStageMetrics();
    MHNotifyCompositeAssetChanged(*ChangedFixture.Root);
    const FMHPlacementStageMetrics Metrics = MHGetPlacementStageMetrics();
    AddInfo(DefinitionMetricsLine(TEXT("acceptance_targeted_reimport"), 3, 0.0, Metrics));
    bPassed &= TestEqual(TEXT("one invalidated definition is rebuilt once"),
        Metrics.Get(EMHPlacementStage::BuildAppliedGraph).Calls, 1ull);
    bPassed &= TestEqual(TEXT("every affected placement resolves again"),
        Metrics.Get(EMHPlacementStage::ResolveCompositePlan).Calls, 3ull);
    for (int32 Index = 0; Index < ChangedActors.Num(); ++Index)
    {
        bPassed &= TestEqual(TEXT("every affected placement rebuilds exactly once"),
            ChangedActors[Index]->GetPlacementRebuildCount(), ChangedRebuilds[Index] + 1);
        bPassed &= DefinitionPlanParity(*this, *ChangedActors[Index], *ChangedFixture.Root);
    }
    for (int32 Index = 0; Index < UnrelatedActors.Num(); ++Index)
        bPassed &= TestEqual(TEXT("unrelated placement is not rebuilt"),
            UnrelatedActors[Index]->GetPlacementRebuildCount(), UnrelatedRebuilds[Index]);

    World->DestroyWorld(true);
    return bPassed;
}
} // namespace UE::MimirComposite::Tests
