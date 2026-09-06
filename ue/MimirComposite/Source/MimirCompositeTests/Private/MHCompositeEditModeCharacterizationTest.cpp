#include "MHCompositeEditFixture.h"

#include "Composite/MHInstancePool.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"

namespace UE::MimirComposite::Tests
{

// CE-0 (docs/contracts/composite_edit_ce0.md §4): characterization of the
// edit path CE replaces. These tests pin today's behaviour on the reference
// fixture; later CE slices rewrite the expectations they flip (noted per test).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeFixtureTest,
    "Mimir.V5.Composite.EditMode.Characterization.FixtureResolvesBothInvocations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeFixtureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    bool bPassed = true;
    for (AMHCompositeActor* Actor : {F.A, F.B})
    {
        for (int32 Index = 0; Index < 2; ++Index)
        {
            const FMHResolvedCompositeNode* Invocation = FCompositeEditFixture::Invocation(*Actor, Index);
            if (!TestNotNull(*FString::Printf(TEXT("invocation %d"), Index), Invocation)) return false;
            const TArray<FVector> Leaves = FCompositeEditFixture::LeafWorldsUnder(*Actor, Invocation->NodePath + TEXT(">"));
            // plain C + grouped C, plus mesh A when the random node picks it.
            bPassed &= TestTrue(*FString::Printf(TEXT("invocation %d renders two or three leaves"), Index), Leaves.Num() == 2 || Leaves.Num() == 3);
        }
    }
    bPassed &= TestEqual(TEXT("foreign ISM holds its two instances"), F.ForeignWorlds().Num(), 2);
    return bPassed;
}

// Flipped by CE-3 (exact inline-node selection).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeAncestorHandleTest,
    "Mimir.V5.Composite.EditMode.Characterization.InlineDescendantResolvesToTopAncestorHandle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeAncestorHandleTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second invocation"), Second)) return false;
    const FString InvocationPath = Second->NodePath;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationPath, Error))) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = F.A->GetEditScopeHandles();
    bool bPassed = TestEqual(TEXT("one handle per top-level child node"), Handles.Num(), 3);
    if (Handles.Num() != 3) return false;
    const FString ChildPrefix = InvocationPath + TEXT(">") + F.Child->LogicalName + TEXT(":");
    bPassed &= TestTrue(TEXT("today: the grouped mesh resolves to the group's handle, not its own"),
        F.A->FindSessionHandleForNodePath(ChildPrefix + TEXT("nodes[1]/children[0]")) == Handles[1]);
    bPassed &= TestTrue(TEXT("today: the random pick resolves to the random node's handle"),
        F.A->FindSessionHandleForNodePath(ChildPrefix + TEXT("nodes[2]/options[0]")) == Handles[2]);
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// Flipped by CE-2 (local preview of the selected occurrence, CE-ADR-4).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeSharedDraftPreviewTest,
    "Mimir.V5.Composite.EditMode.Characterization.DraftPreviewFollowsEveryInvocationInThisPlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeSharedDraftPreviewTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* First = FCompositeEditFixture::Invocation(*F.A, 0);
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("first"), First) || !TestNotNull(TEXT("second"), Second)) return false;
    const FString FirstPrefix = First->NodePath + TEXT(">");
    const FString SecondPath = Second->NodePath;
    const FString SecondPrefix = SecondPath + TEXT(">");
    const TArray<FVector> FirstBefore = FCompositeEditFixture::LeafWorldsUnder(*F.A, FirstPrefix);
    const TArray<FVector> SecondBefore = FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix);
    const TArray<FVector> BBefore = FCompositeEditFixture::LeafWorldsUnder(*F.B, F.Root->LogicalName);
    const TArray<FVector> ForeignBefore = F.ForeignWorlds();

    FString Error;
    if (!TestTrue(TEXT("edit the second invocation: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = F.A->GetEditScopeHandles();
    if (!TestEqual(TEXT("three handles"), Handles.Num(), 3) || !IsValid(Handles[0])) return false;
    // Move the plain mesh +100 X in the second invocation (no rotation there:
    // the authored delta is +100 X in child space).
    Handles[0]->SetWorldLocation(Handles[0]->GetComponentLocation() + FVector(100.0, 0.0, 0.0));
    F.A->Tick(0.0f);
    bool bPassed = TestTrue(TEXT("preview survives the edit: ") + F.A->GetLastPlacementError(), F.A->GetLastPlacementError().IsEmpty());
    const TArray<FVector> SecondAfter = FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix);
    const TArray<FVector> FirstAfter = FCompositeEditFixture::LeafWorldsUnder(*F.A, FirstPrefix);
    bPassed &= TestTrue(TEXT("the edited invocation moved"), !FCompositeEditFixture::SameLocations(SecondBefore, SecondAfter));
    // Today the draft is the shared definition inside this placement: the
    // first invocation (yaw 90) shows the same authored delta, i.e. +100 Y.
    bPassed &= TestTrue(TEXT("today: the other invocation of the same child in this placement follows the draft"),
        !FCompositeEditFixture::SameLocations(FirstBefore, FirstAfter) &&
        FirstAfter.ContainsByPredicate([&FirstBefore](const FVector& After)
        {
            return FirstBefore.ContainsByPredicate([&After](const FVector& Before) { return After.Equals(Before + FVector(0.0, 100.0, 0.0), 1e-2); });
        }));
    bPassed &= TestTrue(TEXT("the other placement never follows a draft"), FCompositeEditFixture::SameLocations(BBefore, FCompositeEditFixture::LeafWorldsUnder(*F.B, F.Root->LogicalName)));
    bPassed &= TestTrue(TEXT("foreign instances never move"), FCompositeEditFixture::SameLocations(ForeignBefore, F.ForeignWorlds()));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestTrue(TEXT("cancel restores the first invocation"), FCompositeEditFixture::SameLocations(FirstBefore, FCompositeEditFixture::LeafWorldsUnder(*F.A, FirstPrefix)));
    bPassed &= TestTrue(TEXT("cancel restores the second invocation"), FCompositeEditFixture::SameLocations(SecondBefore, FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix)));
    bPassed &= TestTrue(TEXT("foreign instances still untouched"), FCompositeEditFixture::SameLocations(ForeignBefore, F.ForeignWorlds()));
    return bPassed;
}

// Flipped by CE-4a (Undo inside the session edits the draft and keeps the session).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeUndoEndsSessionTest,
    "Mimir.V5.Composite.EditMode.Characterization.UndoInsideSessionEndsIt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeUndoEndsSessionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second invocation"), Second)) return false;
    const FString SecondPrefix = Second->NodePath + TEXT(">");
    const TArray<FVector> Before = FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix);
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = F.A->GetEditScopeHandles();
    if (!TestEqual(TEXT("three handles"), Handles.Num(), 3) || !IsValid(Handles[0])) return false;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-0 characterization: move a node"));
        F.A->Modify();
        Handles[0]->Modify();
        Handles[0]->SetWorldLocation(Handles[0]->GetComponentLocation() + FVector(0.0, 0.0, 25.0));
    }
    F.A->Tick(0.0f);
    bool bPassed = TestTrue(TEXT("the node moved"), !FCompositeEditFixture::SameLocations(Before, FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix)));
    bPassed &= TestTrue(TEXT("undo runs"), GEditor->UndoTransaction());
    // Today (R5-F): Undo during an edit ends the session and restores the sealed placement.
    bPassed &= TestFalse(TEXT("today: undo ends the edit session"), F.A->IsPlacementEditMode());
    bPassed &= TestTrue(TEXT("the sealed placement is back"), FCompositeEditFixture::SameLocations(Before, FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix)));
    if (Subsystem->IsEditingComposite()) Subsystem->CancelEditComposite(Error);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
