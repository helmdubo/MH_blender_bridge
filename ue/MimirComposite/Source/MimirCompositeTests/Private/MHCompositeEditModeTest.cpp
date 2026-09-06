#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Settings/MHCompositeSettings.h"
#include "UI/MHSourceOverwritePolicy.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FModeV2Scope
{
    bool bPrevious = false;
    FModeV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FModeV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
        UMHCompositeEditorMode::SetDiscardConfirmForTests({});
    }
};

bool AssetBytes(const UMHCompositeAsset& Asset, TArray<uint8>& OutBytes)
{
    FMHCompositeDocument Document;
    FString Error;
    return MHExtractCompositeV5(Asset, Document, Error) && MHWriteCanonicalCompositeV5(Document, OutBytes, Error);
}

} // namespace

// CE-3a (spec CE-ADR-2, CE-3): the Composite Edit Mode follows the CE-backend
// session on the level editor's mode manager and locks the context to the
// session's projection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeFollowsSessionTest,
    "Mimir.V5.Composite.EditMode.Mode.FollowsTheSessionAndLocksTheContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeFollowsSessionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FModeV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    bool bPassed = TestFalse(TEXT("no session: mode inactive"), UMHCompositeEditorMode::IsActive());
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    bPassed &= TestTrue(TEXT("the mode is active with the session"), UMHCompositeEditorMode::IsActive());
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    AActor* ProjectionActor = Session != nullptr && Session->GetProjection() != nullptr ? Session->GetProjection()->GetProjectionActor() : nullptr;
    if (!TestNotNull(TEXT("active mode"), Mode) || !TestNotNull(TEXT("projection actor"), ProjectionActor)) return false;
    bPassed &= TestFalse(TEXT("the projection can be selected"), Mode->IsSelectionDisallowed(ProjectionActor, true));
    bPassed &= TestTrue(TEXT("another placement cannot be selected"), Mode->IsSelectionDisallowed(F.B, true));
    bPassed &= TestTrue(TEXT("the root placement cannot be edited"), Mode->IsEditingDisallowed(F.A));
    bPassed &= TestFalse(TEXT("deselecting is always allowed"), Mode->IsSelectionDisallowed(F.B, false));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("the mode leaves with the session"), UMHCompositeEditorMode::IsActive());
    return bPassed;
}

// CE-3a: the two buttons. Cancel asks only when the draft is dirty and keeps
// the session when the user stays; Save applies the shared definition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeSaveCancelTest,
    "Mimir.V5.Composite.EditMode.Mode.CancelAsksWhenDirtyAndSaveApplies",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeSaveCancelTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FModeV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child bytes"), AssetBytes(*F.Child, ChildBefore))) return false;
    FString Error;

    // Clean: Cancel leaves at once.
    if (!TestTrue(TEXT("session 1: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    if (!TestNotNull(TEXT("mode"), Mode)) return false;
    int32 Asked = 0;
    UMHCompositeEditorMode::SetDiscardConfirmForTests([&Asked]() { ++Asked; return false; });
    bool bPassed = TestTrue(TEXT("clean cancel leaves"), Mode->RequestCancel());
    bPassed &= TestEqual(TEXT("clean cancel asks nothing"), Asked, 0);
    bPassed &= TestFalse(TEXT("session gone"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("mode gone"), UMHCompositeEditorMode::IsActive());

    // Dirty: Cancel asks; staying keeps everything; discarding leaves.
    if (!TestTrue(TEXT("session 2: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("mode 2"), Mode) || !TestNotNull(TEXT("session 2"), Session) || Session->GetDraft() == nullptr) return false;
    bPassed &= TestTrue(TEXT("edit"), Session->SetNodeTransform(Session->GetDraft()->GetNodeId(0), FTransform(FVector(100.0, 0.0, 40.0)), Error));
    bPassed &= TestFalse(TEXT("dirty cancel with 'stay' keeps the session"), Mode->RequestCancel());
    bPassed &= TestEqual(TEXT("it asked once"), Asked, 1);
    bPassed &= TestTrue(TEXT("session still open"), Subsystem->IsEditingComposite() && UMHCompositeEditorMode::IsActive());
    UMHCompositeEditorMode::SetDiscardConfirmForTests([&Asked]() { ++Asked; return true; });
    bPassed &= TestTrue(TEXT("dirty cancel with 'discard' leaves"), Mode->RequestCancel());
    bPassed &= TestEqual(TEXT("it asked again"), Asked, 2);
    bPassed &= TestFalse(TEXT("session gone after discard"), Subsystem->IsEditingComposite());
    TArray<uint8> ChildAfterDiscard;
    bPassed &= TestTrue(TEXT("discard never touches the source"), AssetBytes(*F.Child, ChildAfterDiscard) && ChildAfterDiscard == ChildBefore);

    // Save: the usual overwrite confirmation, then the shared definition is published.
    if (!TestTrue(TEXT("session 3: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    Mode = UMHCompositeEditorMode::GetActive();
    Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("mode 3"), Mode) || !TestNotNull(TEXT("session 3"), Session) || Session->GetDraft() == nullptr) return false;
    bPassed &= TestTrue(TEXT("edit 3"), Session->SetNodeTransform(Session->GetDraft()->GetNodeId(0), FTransform(FVector(100.0, 0.0, 40.0)), Error));
    int32 Confirmations = 0;
    FMHSourceOverwritePolicyTestHooks Hooks;
    Hooks.Confirm = [&Confirmations](const FText&) { ++Confirmations; return true; };
    Hooks.Notify = [](const FText&) {};
    Hooks.MessageLog = [](const FText&) {};
    MHSetSourceOverwritePolicyTestHooks(Hooks);
    UMHCompositeAsset* Published = nullptr;
    Subsystem->SetCommitPublisherForTests([&Published](UMHCompositeAsset& Asset, FString&) { Published = &Asset; MHNotifyCompositeAssetChanged(Asset); return true; });
    Mode->RequestSave();
    Subsystem->SetCommitPublisherForTests({});
    MHSetSourceOverwritePolicyTestHooks(FMHSourceOverwritePolicyTestHooks());
    bPassed &= TestEqual(TEXT("save asked the overwrite confirmation once"), Confirmations, 1);
    bPassed &= TestTrue(TEXT("save published the shared child"), Published == F.Child);
    bPassed &= TestFalse(TEXT("session gone after save"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("mode gone after save"), UMHCompositeEditorMode::IsActive());
    FMHCompositeDocument ChildDocument;
    bPassed &= TestTrue(TEXT("the child carries the edit"), MHExtractCompositeV5(*F.Child, ChildDocument, Error) && ChildDocument.Nodes.Num() == 3 && ChildDocument.Nodes[0].Transform.TranslationCm.Equals(FVector(100.0, 0.0, 40.0), 1e-2));
    return bPassed;
}

// CE-3a: the legacy backend never activates the mode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeLegacyTest,
    "Mimir.V5.Composite.EditMode.Mode.LegacyBackendNeverActivatesIt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeLegacyTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("legacy nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    bool bPassed = TestFalse(TEXT("legacy session: mode inactive"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("legacy session: root in its own edit mode"), F.A->IsPlacementEditMode());
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
