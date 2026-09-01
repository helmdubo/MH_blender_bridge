#include "Composite/MHCompositeActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UI/MHCompositeOutlinerModel.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeOutlinerInstanceSelectionRetentionTest,
    "Mimir.V5.Composite.Outliner.InstanceSelectionRetention",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeOutlinerInstanceSelectionRetentionTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* First = World->SpawnActor<AMHCompositeActor>();
    AMHCompositeActor* Second = World->SpawnActor<AMHCompositeActor>();
    AActor* Foreign = World->SpawnActor<AActor>();
    UInstancedStaticMeshComponent* FirstInstance =
        NewObject<UInstancedStaticMeshComponent>(First);
    UInstancedStaticMeshComponent* FirstInstanceDuplicate =
        NewObject<UInstancedStaticMeshComponent>(First);
    UInstancedStaticMeshComponent* SecondInstance =
        NewObject<UInstancedStaticMeshComponent>(Second);
    UInstancedStaticMeshComponent* ForeignInstance =
        NewObject<UInstancedStaticMeshComponent>(Foreign);

    bool bPassed = true;
    bPassed &= TestEqual(TEXT("actor selection remains authoritative"),
        MHResolveCompositeOutlinerActor({First}, {SecondInstance}), First);
    bPassed &= TestNull(TEXT("multiple selected composite actors stay fail-closed"),
        MHResolveCompositeOutlinerActor({First, Second}, {FirstInstance}));
    bPassed &= TestEqual(TEXT("one selected SMInstance retains its composite owner"),
        MHResolveCompositeOutlinerActor({}, {FirstInstance}), First);
    bPassed &= TestEqual(TEXT("multiple instances of one owner remain unambiguous"),
        MHResolveCompositeOutlinerActor({}, {FirstInstance, FirstInstanceDuplicate}), First);
    bPassed &= TestNull(TEXT("instances from multiple composite owners stay fail-closed"),
        MHResolveCompositeOutlinerActor({}, {FirstInstance, SecondInstance}));
    bPassed &= TestNull(TEXT("foreign instance ownership does not claim the Outliner"),
        MHResolveCompositeOutlinerActor({}, {ForeignInstance}));

    First->Destroy();
    Second->Destroy();
    Foreign->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
