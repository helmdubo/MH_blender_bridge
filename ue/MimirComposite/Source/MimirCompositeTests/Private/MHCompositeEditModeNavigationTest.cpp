#include "MHCompositeEditFixture.h"

#include "Composite/MHInstancePool.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "EditorModeManager.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "UI/MHSourceOverwritePolicy.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FNavTestScope
{
    FDelegateHandle ModeChangedHandle;
    int32 ModeTransitions = 0;
    ~FNavTestScope()
    {
        if (ModeChangedHandle.IsValid() && GEditor != nullptr)
            GLevelEditorModeTools().OnEditorModeIDChanged().Remove(ModeChangedHandle);
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
    FNavTestScope Scope;
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
    UMHCompositeEditorMode* const InitialMode = Mode;
    UMHCompositeEditSession* const InitialSession = Subsystem->GetEditSession();
    const FGuid InitialSessionId = InitialSession->GetSessionId();
    Scope.ModeChangedHandle = GLevelEditorModeTools().OnEditorModeIDChanged().AddLambda(
        [&Scope](const FEditorModeID&, bool) { ++Scope.ModeTransitions; });

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
    bPassed &= TestTrue(TEXT("clean switch retains the same session object"), Subsystem->GetEditSession() == InitialSession);
    bPassed &= TestEqual(TEXT("scope switch retains session identity"), Subsystem->GetEditSession()->GetSessionId(), InitialSessionId);
    bPassed &= TestTrue(TEXT("scope epoch advances without replacing session"), Subsystem->GetEditSessionEpoch() > Epoch);
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
    bPassed &= TestTrue(TEXT("discard changes scope in the same session"), Subsystem->GetEditSession() == InitialSession);
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
    bPassed &= TestTrue(TEXT("save changes scope in the same session"), Subsystem->GetEditSession() == InitialSession);
    bPassed &= TestEqual(TEXT("save retains session identity"), Subsystem->GetEditSession()->GetSessionId(), InitialSessionId);
    bPassed &= TestTrue(TEXT("the mode object stays alive throughout navigation"), UMHCompositeEditorMode::GetActive() == InitialMode);
    bPassed &= TestEqual(TEXT("navigation never exits or re-enters the mode"), Scope.ModeTransitions, 0);
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
    const FNavTestScope Scope;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditScopeFailureTest,
    "Mimir.V5.Composite.EditMode.Navigation.InPlaceSwitchPreservesDraftOnFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditScopeFailureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FNavTestScope Scope;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* First = FCompositeEditFixture::Invocation(*F.A, 0);
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("first occurrence"), First) || !TestNotNull(TEXT("second occurrence"), Second)) return false;
    const FString FirstPath = First->NodePath, SecondPath = Second->NodePath;
    FString Error;
    if (!TestTrue(TEXT("begin"), Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditSession* const Session = Subsystem->GetEditSession();
    UMHCompositeEditorMode* const Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditDocument* const Draft = Session->GetDraft();
    const FGuid NodeId = Draft->GetNodeId(0);
    Session->SetSelectedNodeIds({NodeId});
    {
        const FScopedTransaction Transaction(INVTEXT("scope failure fixture edit"));
        if (!TestTrue(TEXT("edit"), Session->SetNodeTransform(NodeId, FTransform(FVector(17, 23, 41)), Error))) return false;
    }
    TArray<uint8> Before;
    if (!TestTrue(TEXT("snapshot draft"), Draft->CanonicalBytes(Before, Error))) return false;
    const uint32 Revision = Draft->GetRevision();
    const uint32 Epoch = Session->GetEpoch();
    // A grammar-valid target can still fail to build its projection (cycle).
    FMHCompositeDocument CyclicTarget = Session->GetOriginalDocument();
    FMHCompositeNode& Cycle = CyclicTarget.Nodes.AddDefaulted_GetRef();
    Cycle.Kind = EMHCompositeNodeKind::Composite;
    Cycle.Resource = F.Child->LogicalName;
    bool bPassed = TestFalse(TEXT("failed target projection rolls back in place"),
        Session->RetargetDefinition(F.Child, FirstPath, CyclicTarget, Epoch + 1, Error));
    bPassed &= TestEqual(TEXT("projection failure restores session epoch"), Session->GetEpoch(), Epoch);
    bPassed &= TestEqual(TEXT("projection failure leaves subsystem epoch"), Subsystem->GetEditSessionEpoch(), Epoch);
    bPassed &= TestTrue(TEXT("projection failure restores original draft and projection"), Session->GetDraft() == Draft && Session->GetProjection() != nullptr && Session->GetProjection()->IsOpen());
    bPassed &= TestEqual(TEXT("projection failure restores selected node"), Session->GetActiveNodeId(), NodeId);
    int32 Publishes = 0;
    Subsystem->SetCommitPublisherForTests([&Publishes](UMHCompositeAsset&, FString& PublishError)
    {
        ++Publishes;
        PublishError = TEXT("MH_E_SCOPE_TEST: injected publish failure");
        return false;
    });
    TArray<FString> Warnings;
    bPassed &= TestFalse(TEXT("missing target refuses before Discard"), Subsystem->SwitchEditComposite(TEXT("missing:nodes[99]"), false, Warnings, Error));
    bPassed &= TestFalse(TEXT("missing target refuses before Save"), Subsystem->SwitchEditComposite(TEXT("missing:nodes[99]"), true, Warnings, Error));
    bPassed &= TestEqual(TEXT("invalid target writes nothing"), Publishes, 0);
    bPassed &= TestEqual(TEXT("invalid target retains epoch"), Session->GetEpoch(), Epoch);
    bPassed &= TestEqual(TEXT("invalid target retains revision"), Draft->GetRevision(), Revision);
    bPassed &= TestFalse(TEXT("failed Save keeps the current scope"), Subsystem->SwitchEditComposite(FirstPath, true, Warnings, Error));
    Subsystem->SetCommitPublisherForTests({});
    TArray<uint8> AfterFailure;
    bPassed &= TestTrue(TEXT("same draft and bytes after failed publish"), Session->GetDraft() == Draft && Draft->CanonicalBytes(AfterFailure, Error) && AfterFailure == Before);
    bPassed &= TestTrue(TEXT("failure retains session and mode"), Subsystem->GetEditSession() == Session && UMHCompositeEditorMode::GetActive() == Mode);
    bPassed &= TestEqual(TEXT("failure retains active occurrence"), Session->GetInvocationPath(), SecondPath);
    bPassed &= TestEqual(TEXT("failure retains logical selection"), Session->GetActiveNodeId(), NodeId);
    bPassed &= TestTrue(TEXT("failed Save remains dirty"), Session->IsDirty());

    Subsystem->SetCommitPublisherForTests([](UMHCompositeAsset& Asset, FString&) { MHNotifyCompositeAssetChanged(Asset); return true; });
    bPassed &= TestTrue(TEXT("Save then switch to another occurrence of the SAME definition"), Subsystem->SwitchEditComposite(FirstPath, true, Warnings, Error));
    Subsystem->SetCommitPublisherForTests({});
    TArray<uint8> AfterSwitch;
    bPassed &= TestTrue(TEXT("new scope reads the saved definition"), Session->GetDraft()->CanonicalBytes(AfterSwitch, Error) && AfterSwitch == Before);
    bPassed &= TestTrue(TEXT("successful retry retains session and mode"), Subsystem->GetEditSession() == Session && UMHCompositeEditorMode::GetActive() == Mode);
    bPassed &= TestEqual(TEXT("first occurrence active"), Session->GetInvocationPath(), FirstPath);
    bPassed &= TestFalse(TEXT("new scope starts clean"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("scope epoch invalidates queued commands"), Session->GetEpoch() > Epoch);
    // Simulate another edit removing the destination during publication. The
    // current occurrence remains open and must suppress its NEW pooled handles.
    bPassed &= TestTrue(TEXT("edit before destination disappears"), Session->SetNodeTransform(Session->GetDraft()->GetNodeId(0), FTransform(FVector(29, 31, 47)), Error));
    Subsystem->SetCommitPublisherForTests([&F](UMHCompositeAsset& Asset, FString& PublishError)
    {
        MHNotifyCompositeAssetChanged(Asset);
        FMHCompositeDocument RootDocument;
        if (!MHExtractCompositeV5(*F.Root, RootDocument, PublishError) || RootDocument.Nodes.Num() != 3) return false;
        RootDocument.Nodes[2].Kind = EMHCompositeNodeKind::Group;
        RootDocument.Nodes[2].Resource.Reset();
        if (!MHApplyCompositeV5(*F.Root, RootDocument, PublishError)) return false;
        MHNotifyCompositeAssetChanged(*F.Root);
        return true;
    });
    bPassed &= TestFalse(TEXT("post-publish missing destination keeps current scope"), Subsystem->SwitchEditComposite(SecondPath, true, Warnings, Error));
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestTrue(TEXT("post-publish failure retains same session and mode"), Subsystem->GetEditSession() == Session && UMHCompositeEditorMode::GetActive() == Mode);
    bPassed &= TestEqual(TEXT("post-publish failure retains current occurrence"), Session->GetInvocationPath(), FirstPath);
    bPassed &= TestFalse(TEXT("successfully written source remains the new baseline"), Session->IsDirty());
    const UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(F.World);
    if (!TestNotNull(TEXT("instance pool"), Pool)) return false;
    int32 Suppressed = 0;
    for (const FMHCompositeLeafMaterialization& Row : F.A->GetLeafMaterializations())
    {
        if (!Row.Handle.IsSet()) continue;
        const bool bCurrentScope = Row.NodePath.StartsWith(FirstPath + TEXT(">"));
        bPassed &= TestEqual(TEXT("suppression follows current handles: ") + Row.NodePath, Pool->IsSuppressed(Row.Handle), bCurrentScope);
        if (bCurrentScope) ++Suppressed;
    }
    bPassed &= TestTrue(TEXT("retained occurrence has suppressed pooled leaves"), Suppressed > 0);
    bPassed &= TestTrue(TEXT("Cancel exits the entire retained session"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
