#include "Composite/MHCompositeActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UI/MHCompositeOutlinerModel.h"

namespace UE::MimirComposite::Tests
{
namespace
{
FMHCompositeOutlinerFreshness MakeFreshness()
{
    FMHCompositeOutlinerFreshness Result;
    Result.Seed = 1729;
    Result.AppearanceSeed = 2718;
    Result.ResolvedSignature = TEXT("blake3-160:resolved");
    Result.AppearanceSignature = TEXT("blake3-160:appearance");
    Result.PlacementSignature = TEXT("blake3-160:placement");
    return Result;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeOutlinerStaleRebuildSkipTest,
    "Mimir.V5.Composite.Outliner.StaleRebuildSkip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeOutlinerStaleRebuildSkipTest::RunTest(const FString& Parameters)
{
    const FMHCompositeOutlinerFreshness Baseline = MakeFreshness();
    bool bPassed = TestTrue(TEXT("complete freshness matches itself"),
        Baseline.Matches(Baseline));

    const auto TestStaleAxis = [this, &Baseline, &bPassed](
        const TCHAR* Label,
        TFunctionRef<void(FMHCompositeOutlinerFreshness&)> Mutate)
    {
        FMHCompositeOutlinerFreshness Changed = Baseline;
        Mutate(Changed);
        bPassed &= TestFalse(Label, Baseline.Matches(Changed));
    };
    TestStaleAxis(TEXT("layout seed invalidates freshness"),
        [](FMHCompositeOutlinerFreshness& Value) { ++Value.Seed; });
    TestStaleAxis(TEXT("appearance seed invalidates freshness"),
        [](FMHCompositeOutlinerFreshness& Value) { ++Value.AppearanceSeed; });
    TestStaleAxis(TEXT("resolved signature invalidates freshness"),
        [](FMHCompositeOutlinerFreshness& Value) { Value.ResolvedSignature += TEXT("x"); });
    TestStaleAxis(TEXT("appearance signature invalidates freshness"),
        [](FMHCompositeOutlinerFreshness& Value) { Value.AppearanceSignature += TEXT("x"); });
    TestStaleAxis(TEXT("placement signature invalidates freshness"),
        [](FMHCompositeOutlinerFreshness& Value) { Value.PlacementSignature += TEXT("x"); });
    const FMHCompositeOutlinerFreshness Empty;
    bPassed &= TestFalse(TEXT("two empty states never claim freshness"), Empty.Matches(Empty));
    bPassed &= TestFalse(TEXT("incomplete state never claims freshness"),
        Baseline.Matches(FMHCompositeOutlinerFreshness()));

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    FMHCompositeOutlinerRefreshState State;
    int32 RebuildCount = 0;
    const auto Refresh = [&]()
    {
        if (!State.NeedsRebuild(Actor, Baseline)) return;
        ++RebuildCount;
        State.RecordRebuild(Actor, Baseline, true);
    };
    Refresh();
    Refresh();
    bPassed &= TestEqual(TEXT("two unchanged selection refreshes rebuild once"),
        RebuildCount, 1);

    State.RecordRebuild(Actor, Baseline, false);
    bPassed &= TestTrue(TEXT("failed rebuild invalidates the skip state"),
        State.NeedsRebuild(Actor, Baseline));
    bPassed &= TestTrue(TEXT("empty current state stays fail-closed"),
        State.NeedsRebuild(Actor, Empty));

    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
