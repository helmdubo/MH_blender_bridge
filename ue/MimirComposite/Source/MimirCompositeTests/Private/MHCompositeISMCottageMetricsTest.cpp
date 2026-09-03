#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

namespace UE::MimirComposite::Tests
{
namespace
{
UMHCompositeAsset* FindCottageMetricsAsset()
{
    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FAssetData> Assets;
    Registry.GetAssetsByClass(UMHCompositeAsset::StaticClass()->GetClassPathName(), Assets, true);
    const FAssetData* Cottage = Assets.FindByPredicate([](const FAssetData& Asset)
    {
        return Asset.AssetName == TEXT("sovmod_cottage_i_cmp");
    });
    return Cottage != nullptr ? Cast<UMHCompositeAsset>(Cottage->GetAsset()) : nullptr;
}

FString CottageMetricsLine(
    const TCHAR* Phase, const AMHCompositeActor& Actor, const double WallMilliseconds,
    const FMHPlacementStageMetrics& Metrics)
{
    int32 SceneComponents = 0;
    int32 StaticMaterializers = 0;
    int32 OrdinaryStaticMeshes = 0;
    int32 ISMBuckets = 0;
    int32 ISMInstances = 0;
    for (UActorComponent* Component : Actor.GetDerivedComponents())
    {
        if (Cast<USceneComponent>(Component) != nullptr) ++SceneComponents;
        if (const UInstancedStaticMeshComponent* Bucket =
                Cast<UInstancedStaticMeshComponent>(Component))
        {
            ++StaticMaterializers;
            ++ISMBuckets;
            ISMInstances += Bucket->GetInstanceCount();
        }
        else if (Cast<UStaticMeshComponent>(Component) != nullptr)
        {
            ++StaticMaterializers;
            ++OrdinaryStaticMeshes;
        }
    }
    FString Result = FString::Printf(
        TEXT("U5_COTTAGE_METRICS phase=%s wall_ms=%.6f leaves=%d derived=%d scene=%d ")
        TEXT("handles=%d leaf_view=%d static_materializers=%d ordinary_smc=%d ")
        TEXT("ism_buckets=%d ism_instances=%d resident_plan=%d"),
        Phase, WallMilliseconds, Actor.GetResolvedPlan() != nullptr ? Actor.GetResolvedPlan()->Leaves.Num() : 0,
        Actor.GetDerivedComponents().Num(), SceneComponents,
        Actor.GetTopLevelPlacementComponents().Num(),
        Actor.GetLeafPlacementComponents().Num(), StaticMaterializers,
        OrdinaryStaticMeshes, ISMBuckets, ISMInstances,
        Actor.GetResolvedPlan() != nullptr ? 1 : 0);
    for (int32 Index = 0; Index < FMHPlacementStageMetrics::StageCount; ++Index)
    {
        const EMHPlacementStage Stage = static_cast<EMHPlacementStage>(Index);
        const FMHPlacementStageMetric& Metric = Metrics.Get(Stage);
        Result += FString::Printf(TEXT(" %s[calls=%llu,inclusive_ms=%.6f]"),
            MHPlacementStageLabel(Stage), Metric.Calls,
            FPlatformTime::ToMilliseconds64(Metric.InclusiveCycles));
    }
    return Result;
}

AMHCompositeActor* TimedCottagePlacement(
    UWorld& World, UMHCompositeAsset& Asset, double& OutWallMilliseconds,
    FMHPlacementStageMetrics& OutMetrics)
{
    AMHCompositeActor* Actor = World.SpawnActor<AMHCompositeActor>();
    if (Actor == nullptr) return nullptr;
    Actor->SetAutoSeed(false);
    Actor->SetAutoAppearanceSeed(false);
    Actor->SetSeed(1729);
    Actor->SetAppearanceSeed(2718);
    MHResetPlacementStageMetrics();
    const double Start = FPlatformTime::Seconds();
    Actor->SetCompositeAsset(&Asset);
    OutWallMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
    OutMetrics = MHGetPlacementStageMetrics();
    return Actor;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeISMCottageMetricsTest,
    "Mimir.V5.Composite.ISM.CottageMetrics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeISMCottageMetricsTest::RunTest(const FString& Parameters)
{
    UMHCompositeAsset* Asset = FindCottageMetricsAsset();
    if (Asset == nullptr)
    {
        AddInfo(TEXT("U5 cottage metrics NOT RUN: sovmod_cottage_i_cmp is not installed in this host"));
        return true;
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("cottage metrics preview world"), World)) return false;
    double ColdWall = 0.0;
    FMHPlacementStageMetrics ColdMetrics;
    AMHCompositeActor* Cold = TimedCottagePlacement(
        *World, *Asset, ColdWall, ColdMetrics);
    bool bPassed = TestNotNull(TEXT("cold cottage placement actor"), Cold);
    if (Cold != nullptr)
    {
        AddInfo(CottageMetricsLine(TEXT("cold"), *Cold, ColdWall, ColdMetrics));
        bPassed &= TestNotNull(*Cold->GetLastPlacementError(), Cold->GetResolvedPlan());
    }

    double WarmWall = 0.0;
    FMHPlacementStageMetrics WarmMetrics;
    AMHCompositeActor* Warm = TimedCottagePlacement(
        *World, *Asset, WarmWall, WarmMetrics);
    bPassed &= TestNotNull(TEXT("warm cottage placement actor"), Warm);
    if (Warm != nullptr)
    {
        AddInfo(CottageMetricsLine(TEXT("warm"), *Warm, WarmWall, WarmMetrics));
        bPassed &= TestNotNull(*Warm->GetLastPlacementError(), Warm->GetResolvedPlan());
    }
    World->DestroyWorld(true);
    return bPassed;
}
} // namespace UE::MimirComposite::Tests
