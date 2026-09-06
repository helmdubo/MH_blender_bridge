#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditSession.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::MimirComposite::Tests
{
namespace
{

bool AssetBytes(const UMHCompositeAsset& Asset, TArray<uint8>& OutBytes)
{
    FMHCompositeDocument Document;
    FString Error;
    return MHExtractCompositeV5(Asset, Document, Error) && MHWriteCanonicalCompositeV5(Document, OutBytes, Error);
}

} // namespace

// CE-1 (spec §5.1): one owner of the session — identity, epoch, frozen
// context, immutable original, transactional draft — reachable through the
// subsystem facade; the subsystem's draft view reads the session's draft.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditSessionOwnerTest,
    "Mimir.V5.Composite.EditMode.Session.SubsystemOwnsOneSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditSessionOwnerTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    bool bPassed = TestNull(TEXT("no session before Begin"), Subsystem->GetEditSession());
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second invocation"), Second)) return false;
    const FString InvocationPath = Second->NodePath;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child bytes"), AssetBytes(*F.Child, ChildBefore))) return false;

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationPath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("a session exists"), Session)) return false;
    bPassed &= TestTrue(TEXT("session is open and clean"), Session->IsOpen() && Session->GetState() == EMHCompositeEditSessionState::EditingClean);
    bPassed &= TestTrue(TEXT("session id is valid"), Session->GetSessionId().IsValid());
    bPassed &= TestEqual(TEXT("session epoch is the subsystem's"), Session->GetEpoch(), Subsystem->GetEditSessionEpoch());
    bPassed &= TestTrue(TEXT("session knows its placement, asset and invocation"),
        Session->GetRootPlacement() == F.A && Session->GetEditedAsset() == F.Child && Session->GetInvocationPath() == InvocationPath && Session->IsNested());
    bPassed &= TestTrue(TEXT("frozen placement context"), Session->GetFrozenSeed() == F.A->GetSeed() && Session->GetFrozenAppearanceSeed() == F.A->GetAppearanceSeed() && Session->GetEditorWorld() == F.World);
    bPassed &= TestTrue(TEXT("original is the child's source"), Session->GetOriginalBytes() == ChildBefore);
    UMHCompositeEditDocument* Draft = Session->GetDraft();
    if (!TestNotNull(TEXT("draft"), Draft)) return false;
    TArray<uint8> DraftBytes;
    bPassed &= TestTrue(TEXT("draft starts as the original"), Draft->CanonicalBytes(DraftBytes, Error) && DraftBytes == ChildBefore);
    bPassed &= TestEqual(TEXT("draft holds the child's four nodes"), Draft->Num(), 4);
    bPassed &= TestFalse(TEXT("clean session is not dirty"), Session->IsDirty());

    // A legacy handle edit reaches the draft through the facade.
    const TArray<TObjectPtr<USceneComponent>>& Handles = F.A->GetEditScopeHandles();
    if (!TestEqual(TEXT("three handles"), Handles.Num(), 3) || !IsValid(Handles[0])) return false;
    Handles[0]->SetWorldLocation(Handles[0]->GetComponentLocation() + FVector(0.0, 0.0, 30.0));
    F.A->Tick(0.0f);
    const FMHCompositeDocument& View = Subsystem->GetEditingDraft();
    bPassed &= TestTrue(TEXT("the subsystem's draft view shows the edit"), View.Nodes.Num() == 3 && View.Nodes[0].Transform.TranslationCm.Equals(FVector(0.0, 0.0, 70.0), 1e-2));
    bPassed &= TestTrue(TEXT("the session is dirty now"), Session->IsDirty() && Session->GetState() == EMHCompositeEditSessionState::EditingDirty);
    FMHCompositeDocument DraftDocument;
    bPassed &= TestTrue(TEXT("the session draft is the same document"), Draft->Extract(DraftDocument, Error) && DraftDocument.Nodes[0].Transform.TranslationCm.Equals(FVector(0.0, 0.0, 70.0), 1e-2));

    // A command on the draft is refused once the session is closed.
    const TStrongObjectPtr<UMHCompositeEditSession> Kept(Session);
    const FGuid PlainId = Draft->GetNodeId(0);
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestNull(TEXT("the subsystem dropped the session"), Subsystem->GetEditSession());
    bPassed &= TestTrue(TEXT("the old session is closed"), Kept->GetState() == EMHCompositeEditSessionState::Closed && !Kept->IsOpen());
    bPassed &= TestFalse(TEXT("closed session refuses commands"), Kept->SetNodeTransform(PlainId, FTransform::Identity, Error));
    bPassed &= TestTrue(TEXT("the refusal says so"), Error.Contains(TEXT("closed")));
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("no-op begin/cancel wrote nothing"), AssetBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);

    // A root session is a session too.
    if (!TestTrue(TEXT("root session: ") + Error, Subsystem->BeginEditComposite(F.B, Error))) return false;
    UMHCompositeEditSession* RootSession = Subsystem->GetEditSession();
    TArray<uint8> RootBytes;
    bPassed &= TestTrue(TEXT("root session exists"), RootSession != nullptr && !RootSession->IsNested() && RootSession->GetEditedAsset() == F.Root);
    bPassed &= TestTrue(TEXT("root session original is the root's source"), AssetBytes(*F.Root, RootBytes) && RootSession != nullptr && RootSession->GetOriginalBytes() == RootBytes);
    bPassed &= TestTrue(TEXT("cancel root"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// CE-1: a session command edits the draft inside a native transaction and
// Undo restores the draft; the session stays open (the actor's legacy
// PostEditUndo is not involved when only the draft is modified).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditSessionCommandUndoTest,
    "Mimir.V5.Composite.EditMode.Session.CommandIsUndoableWithoutClosing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditSessionCommandUndoTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second invocation"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("session"), Session) || Session->GetDraft() == nullptr) return false;
    const FGuid GroupedId = Session->GetDraft()->GetNodeId(2);
    bool bPassed = TestTrue(TEXT("grouped id"), GroupedId.IsValid());
    bool bCommanded = false;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-1 test: session command"));
        bCommanded = Session->SetNodeTransform(GroupedId, FTransform(FVector(1.0, 2.0, 3.0)), Error);
        bPassed &= TestTrue(TEXT("command: ") + Error, bCommanded);
    }
    if (!bCommanded) return false;
    bPassed &= TestTrue(TEXT("dirty after the command"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("undo runs"), GEditor->UndoTransaction());
    bPassed &= TestTrue(TEXT("the session survived Undo"), Subsystem->GetEditSession() == Session && Session->IsOpen());
    bPassed &= TestFalse(TEXT("undo made the draft clean again"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
