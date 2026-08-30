#include "Composite/MHCompositeActor.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UI/MHCompositeActorDetails.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeActorDetailsSeedActionsTest,
    "Mimir.V5.Composite.Seed.DetailsActions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeActorDetailsSeedActionsTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("editor transaction service exists"), GEditor))
    {
        return false;
    }

    GEditor->ResetTransaction(INVTEXT("MH Details seed action test setup"));
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* First = World != nullptr ? World->SpawnActor<AMHCompositeActor>() : nullptr;
    AMHCompositeActor* Second = World != nullptr ? World->SpawnActor<AMHCompositeActor>() : nullptr;
    bool bPassed = TestNotNull(TEXT("preview world exists"), World) &&
        TestNotNull(TEXT("first selected actor exists"), First) &&
        TestNotNull(TEXT("second selected actor exists"), Second);
    if (First == nullptr || Second == nullptr)
    {
        if (World != nullptr) World->DestroyWorld(false);
        return false;
    }

    First->SetSeed(101);
    Second->SetSeed(202);
    First->SetAppearanceSeed(303);
    Second->SetAppearanceSeed(404);
    const TArray<TWeakObjectPtr<AMHCompositeActor>> Actors = {First, Second};

    FString Error;
    bPassed &= TestTrue(
        TEXT("Layout Generate executes for the edited selection"),
        MHGenerateCompositeSeedsForDetails(Actors, EMHCompositeSeedTarget::Layout, Error));
    bPassed &= TestTrue(TEXT("Layout Generate reports no action error"), Error.IsEmpty());
    bPassed &= TestNotEqual(TEXT("first layout seed changes"), First->GetSeed(), 101);
    bPassed &= TestNotEqual(TEXT("second layout seed changes"), Second->GetSeed(), 202);
    bPassed &= TestTrue(TEXT("one Undo reverses the multi-select layout action"), GEditor->UndoTransaction());
    bPassed &= TestEqual(TEXT("Undo restores first layout seed"), First->GetSeed(), 101);
    bPassed &= TestEqual(TEXT("Undo restores second layout seed"), Second->GetSeed(), 202);

    Error.Reset();
    bPassed &= TestTrue(
        TEXT("Appearance Generate executes for the edited selection"),
        MHGenerateCompositeSeedsForDetails(Actors, EMHCompositeSeedTarget::Appearance, Error));
    bPassed &= TestTrue(TEXT("Appearance Generate reports no action error"), Error.IsEmpty());
    bPassed &= TestNotEqual(TEXT("first appearance seed changes"), First->GetAppearanceSeed(), 303);
    bPassed &= TestNotEqual(TEXT("second appearance seed changes"), Second->GetAppearanceSeed(), 404);
    bPassed &= TestTrue(TEXT("one Undo reverses the multi-select appearance action"), GEditor->UndoTransaction());
    bPassed &= TestEqual(TEXT("Undo restores first appearance seed"), First->GetAppearanceSeed(), 303);
    bPassed &= TestEqual(TEXT("Undo restores second appearance seed"), Second->GetAppearanceSeed(), 404);

    World->DestroyWorld(false);
    GEditor->ResetTransaction(INVTEXT("MH Details seed action test cleanup"));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
