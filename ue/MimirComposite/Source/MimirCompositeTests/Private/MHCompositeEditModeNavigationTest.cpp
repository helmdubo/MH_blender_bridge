#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Selection.h"
#include "Settings/MHCompositeSettings.h"
#include "UI/MHSourceOverwritePolicy.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FNavV2Scope
{
    bool bPrevious = false;
    FNavV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FNavV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
        UMHCompositeEditorMode::SetSwitchConfirmForTests({});
        MHSetSourceOverwritePolicyTestHooks(FMHSourceOverwritePolicyTestHooks());
    }
};

FString SessionPath(const UMHCompositeLevelSubsystem& Subsystem)
{
    const UMHCompositeEditSession* Session = Subsystem.GetEditSession();
    return Session != nullptr && Session->IsOpen() ? Session->GetInvocationPath() : TEXT("<none>");
}

} // namespace

// CE-3d (spec §5.2, LI `EditLevelInstanceInternal`): one writable session —
// switching to another definition of the placement resolves the current one
// first (clean: silently; dirty: Save / Discard / stay), then opens the
// target; the breadcrumb lists the chain with the path each crumb opens.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeSwitchTest,
    "Mimir.V5.Composite.EditMode.Navigation.SwitchAsksThenOpensTheTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeSwitchTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FNavV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* First = FCompositeEditFixture::Invocation(*F.A, 0);
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("first"), First) || !TestNotNull(TEXT("second"), Second)) return false;
    const FString FirstPath = First->NodePath;
    const FString SecondPath = Second->NodePath;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child bytes"), FCompositeEditFixture::AssetBytes(*F.Child, ChildBefore))) return false;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    if (!TestNotNull(TEXT("mode"), Mode)) return false;

    // The breadcrumb: root (opens the root definition), then the edited child.
    const TArray<TPair<FString, FString>> Crumbs = UMHCompositeEditorMode::BreadcrumbTargets(Subsystem->GetEditContext());
    bool bPassed = TestEqual(TEXT("two crumbs"), Crumbs.Num(), 2);
    if (Crumbs.Num() == 2)
    {
        bPassed &= TestEqual(TEXT("root crumb label"), Crumbs[0].Key, F.Root->LogicalName);
        bPassed &= TestTrue(TEXT("root crumb opens the root"), Crumbs[0].Value.IsEmpty());
        bPassed &= TestEqual(TEXT("child crumb label"), Crumbs[1].Key, F.Child->LogicalName);
        bPassed &= TestEqual(TEXT("child crumb is the current scope"), Crumbs[1].Value, SecondPath);
    }

    // Same scope: nothing to do.
    int32 Asked = 0;
    UMHCompositeEditorMode::SetSwitchConfirmForTests([&Asked]() { ++Asked; return EAppReturnType::Cancel; });
    const uint32 Epoch = Subsystem->GetEditSessionEpoch();
    bPassed &= TestTrue(TEXT("switching to the current scope is a no-op"), Mode->RequestSwitch(SecondPath));
    bPassed &= TestEqual(TEXT("same session"), Subsystem->GetEditSessionEpoch(), Epoch);

    // Clean: the other invocation opens without a question.
    bPassed &= TestTrue(TEXT("clean switch"), Mode->RequestSwitch(FirstPath));
    bPassed &= TestEqual(TEXT("clean switch asks nothing"), Asked, 0);
    bPassed &= TestEqual(TEXT("the first invocation is edited"), SessionPath(*Subsystem), FirstPath);
    bPassed &= TestTrue(TEXT("mode active after the switch"), UMHCompositeEditorMode::IsActive());
    Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("mode after switch"), Mode) || !TestNotNull(TEXT("session after switch"), Session) || Session->GetDraft() == nullptr) return false;

    // Dirty + stay: nothing changes.
    bPassed &= TestTrue(TEXT("edit"), Session->SetNodeTransform(Session->GetDraft()->GetNodeId(0), FTransform(FVector(100.0, 0.0, 40.0)), Error));
    bPassed &= TestFalse(TEXT("dirty switch with 'stay' keeps the session"), Mode->RequestSwitch(FString()));
    bPassed &= TestEqual(TEXT("it asked once"), Asked, 1);
    bPassed &= TestEqual(TEXT("still the first invocation"), SessionPath(*Subsystem), FirstPath);
    bPassed &= TestTrue(TEXT("still dirty"), Session->IsDirty());

    // Dirty + discard: the root definition opens, the child source is untouched.
    UMHCompositeEditorMode::SetSwitchConfirmForTests([&Asked]() { ++Asked; return EAppReturnType::No; });
    bPassed &= TestTrue(TEXT("dirty switch with 'discard' opens the target"), Mode->RequestSwitch(FString()));
    bPassed &= TestEqual(TEXT("it asked again"), Asked, 2);
    bPassed &= TestEqual(TEXT("the root definition is edited"), SessionPath(*Subsystem), FString());
    TArray<uint8> ChildAfterDiscard;
    bPassed &= TestTrue(TEXT("discard never touches the child"), FCompositeEditFixture::AssetBytes(*F.Child, ChildAfterDiscard) && ChildAfterDiscard == ChildBefore);
    Mode = UMHCompositeEditorMode::GetActive();
    Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("mode on root"), Mode) || !TestNotNull(TEXT("root session"), Session) || Session->GetDraft() == nullptr) return false;

    // Dirty + save: the root is published, then the second invocation opens.
    bPassed &= TestTrue(TEXT("root edit"), Session->SetNodeTransform(Session->GetDraft()->GetNodeId(0), FTransform(FVector(100.0, 0.0, 0.0)), Error));
    int32 Confirmations = 0;
    FMHSourceOverwritePolicyTestHooks Hooks;
    Hooks.Confirm = [&Confirmations](const FText&) { ++Confirmations; return true; };
    Hooks.Notify = [](const FText&) {};
    Hooks.MessageLog = [](const FText&) {};
    MHSetSourceOverwritePolicyTestHooks(Hooks);
    UMHCompositeAsset* Published = nullptr;
    Subsystem->SetCommitPublisherForTests([&Published](UMHCompositeAsset& Asset, FString&) { Published = &Asset; MHNotifyCompositeAssetChanged(Asset); return true; });
    UMHCompositeEditorMode::SetSwitchConfirmForTests([&Asked]() { ++Asked; return EAppReturnType::Yes; });
    bPassed &= TestTrue(TEXT("dirty switch with 'save' publishes and opens the target"), Mode->RequestSwitch(SecondPath));
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestEqual(TEXT("it asked a third time"), Asked, 3);
    bPassed &= TestEqual(TEXT("Save decision needs no second overwrite confirmation"), Confirmations, 0);
    bPassed &= TestTrue(TEXT("the root was published"), Published == F.Root);
    bPassed &= TestEqual(TEXT("the second invocation is edited"), SessionPath(*Subsystem), SecondPath);
    FMHCompositeDocument RootDocument;
    bPassed &= TestTrue(TEXT("the root carries the edit"), MHExtractCompositeV5(*F.Root, RootDocument, Error) && RootDocument.Nodes.Num() == 3 && RootDocument.Nodes[0].Transform.TranslationCm.Equals(FVector(100.0, 0.0, 0.0), 1e-2));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// The projection retains the occurrence frame, but native selection stays
// empty until the user selects an authoring node (no default-mode gizmo).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeEnterSelectionTest,
    "Mimir.V5.Composite.EditMode.Navigation.EnterSelectsTheOccurrenceAndPivotsThere",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeEnterSelectionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FNavV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    const FVector OccurrenceLocation = FTransform(Second->WorldMatrix * F.A->GetActorTransform().ToMatrixWithScale()).GetLocation();
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    AActor* ProjectionActor = Session != nullptr && Session->GetProjection() != nullptr ? Session->GetProjection()->GetProjectionActor() : nullptr;
    if (!TestNotNull(TEXT("projection actor"), ProjectionActor) || GEditor == nullptr) return false;
    bool bPassed = TestFalse(TEXT("the projection frame is not a native transform target on enter"), ProjectionActor->IsSelected());
    bPassed &= TestEqual(TEXT("no native actor target yet"), GEditor->GetSelectedActorCount(), 0);
    bPassed &= TestEqual(TEXT("no component yet"), GEditor->GetSelectedComponentCount(), 0);
    bPassed &= TestTrue(TEXT("the pivot is the occurrence's transform"), ProjectionActor->GetActorLocation().Equals(OccurrenceLocation, 1e-2));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));

    if (!TestTrue(TEXT("root context: ") + Error, Subsystem->BeginEditComposite(F.A, Error))) return false;
    Session = Subsystem->GetEditSession();
    ProjectionActor = Session != nullptr && Session->GetProjection() != nullptr ? Session->GetProjection()->GetProjectionActor() : nullptr;
    if (!TestNotNull(TEXT("root projection actor"), ProjectionActor)) return false;
    bPassed &= TestFalse(TEXT("root: the projection frame is not selected on enter"), ProjectionActor->IsSelected());
    bPassed &= TestTrue(TEXT("root: the pivot is the placement's transform"), ProjectionActor->GetActorLocation().Equals(F.A->GetActorLocation(), 1e-2));
    bPassed &= TestTrue(TEXT("cancel root"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
