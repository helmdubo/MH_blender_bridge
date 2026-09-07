#include "Composite/MHCompositeProtocol.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditSession.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"

#include <limits>

namespace UE::MimirComposite::Tests
{
namespace
{

FMHCompositeNode CommandMesh(const FString& Resource, const double X)
{
    FMHCompositeNode Node;
    Node.Kind = EMHCompositeNodeKind::Mesh;
    Node.Resource = Resource;
    Node.Transform.TranslationCm = FVector(X, 0.0, 0.0);
    return Node;
}

FMHCompositeDocument CommandDocument()
{
    FMHCompositeDocument Document;
    Document.Nodes.Add(CommandMesh(TEXT("mesh_a"), 0.0));
    Document.Nodes.Add(CommandMesh(TEXT("mesh_b"), 10.0));
    Document.Nodes.Add(CommandMesh(TEXT("mesh_c"), 20.0));
    return Document;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeAtomicTransformCommandsTest,
    "Mimir.V5.Composite.EditMode.Commands.TransformBatchIsAtomicValidatedAndNoOpAware",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeAtomicTransformCommandsTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeEditDocument* Draft = NewObject<UMHCompositeEditDocument>(GetTransientPackage(), NAME_None, RF_Transactional);
    Draft->Load(CommandDocument());
    const FGuid First = Draft->GetNodeId(0);
    const FGuid Second = Draft->GetNodeId(1);
    const FTransform FirstBefore = Draft->GetNodes()[0].Transform;
    const uint32 RevisionBefore = Draft->GetRevision();
    FString Error;

    const FTransform MovedFirst(FVector(100.0, 0.0, 0.0));
    const FTransform InvalidSecond(FQuat::Identity, FVector(200.0, 0.0, 0.0), FVector(1.0, 0.0, 1.0));
    bool bPassed = TestFalse(
        TEXT("second invalid target rejects the whole batch"),
        Draft->SetNodeTransforms({First, Second}, {MovedFirst, InvalidSecond}, Error));
    bPassed &= TestTrue(TEXT("invalid scale diagnostic"), Error.Contains(TEXT("MH_E_INVALID_SCALE")));
    bPassed &= TestTrue(TEXT("the first target was not partially changed"), Draft->GetNodes()[0].Transform.Equals(FirstBefore, 0.0));
    bPassed &= TestEqual(TEXT("rejected batch does not advance revision"), Draft->GetRevision(), RevisionBefore);

    const FTransform MovedSecond(FVector(200.0, 0.0, 0.0));
    bPassed &= TestTrue(
        TEXT("valid batch admitted: ") + Error,
        Draft->SetNodeTransforms({First, Second}, {MovedFirst, MovedSecond}, Error));
    bPassed &= TestEqual(TEXT("one batch advances revision once"), Draft->GetRevision(), RevisionBefore + 1);
    const uint32 RevisionAfterMove = Draft->GetRevision();
    bPassed &= TestTrue(
        TEXT("identical batch is a no-op: ") + Error,
        Draft->SetNodeTransforms({First, Second}, {MovedFirst, MovedSecond}, Error));
    bPassed &= TestEqual(TEXT("no-op does not advance revision"), Draft->GetRevision(), RevisionAfterMove);

    UMHCompositeEditSession* Session = NewObject<UMHCompositeEditSession>(GetTransientPackage());
    Session->Open(nullptr, nullptr, FString(), CommandDocument(), 1);
    int32 ChangedEvents = 0;
    Session->OnChanged.AddLambda([&ChangedEvents]() { ++ChangedEvents; });
    const TArray<FGuid> SessionIds = {Session->GetDraft()->GetNodeId(0), Session->GetDraft()->GetNodeId(1)};
    bPassed &= TestTrue(
        TEXT("session batch admitted: ") + Error,
        Session->SetNodeTransforms(SessionIds, {MovedFirst, MovedSecond}, Error));
    bPassed &= TestEqual(TEXT("session emits one change for the batch"), ChangedEvents, 1);
    bPassed &= TestTrue(
        TEXT("session no-op batch admitted: ") + Error,
        Session->SetNodeTransforms(SessionIds, {MovedFirst, MovedSecond}, Error));
    bPassed &= TestEqual(TEXT("session no-op emits no change"), ChangedEvents, 1);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeProceduralTransformRefusalTest,
    "Mimir.V5.Composite.EditMode.Commands.OrdinaryTransformRefusesProceduralNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeProceduralTransformRefusalTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FMHCompositeDocument Document;
    FMHCompositeNode Profile = CommandMesh(TEXT("mesh_a"), 0.0);
    Profile.Profile = TEXT("scatter");
    Document.Nodes.Add(Profile);
    FMHCompositeNode Inline = CommandMesh(TEXT("mesh_b"), 10.0);
    Inline.bHasInlinePlacement = true;
    Document.Nodes.Add(Inline);
    UMHCompositeEditDocument* Draft = NewObject<UMHCompositeEditDocument>(GetTransientPackage());
    Draft->Load(Document);
    const uint32 Revision = Draft->GetRevision();
    FString Error;
    bool bPassed = TestFalse(
        TEXT("named profile target refused"),
        Draft->SetNodeTransform(Draft->GetNodeId(0), FTransform(FVector(1.0, 0.0, 0.0)), Error));
    bPassed &= TestTrue(TEXT("profile refusal explains procedural policy"), Error.Contains(TEXT("procedural")));
    bPassed &= TestFalse(
        TEXT("inline p2 target refused"),
        Draft->SetNodeTransform(Draft->GetNodeId(1), FTransform(FVector(11.0, 0.0, 0.0)), Error));
    bPassed &= TestTrue(TEXT("inline refusal explains procedural policy"), Error.Contains(TEXT("procedural")));
    bPassed &= TestEqual(TEXT("refusals do not advance revision"), Draft->GetRevision(), Revision);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSessionSelectionIdentityTest,
    "Mimir.V5.Composite.EditMode.Commands.SelectionUsesGuidsAcrossStructureAndUndo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSessionSelectionIdentityTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (GEditor == nullptr || GEditor->Trans == nullptr) return false;
    UMHCompositeEditSession* Session = NewObject<UMHCompositeEditSession>(GetTransientPackage());
    Session->Open(nullptr, nullptr, FString(), CommandDocument(), 1);
    UMHCompositeEditDocument* Draft = Session->GetDraft();
    if (!TestNotNull(TEXT("draft"), Draft) || Draft->Num() != 3) return false;
    const FGuid Preceding = Draft->GetNodeId(0);
    const FGuid Selected = Draft->GetNodeId(1);
    const FGuid Active = Draft->GetNodeId(2);
    int32 SelectionEvents = 0;
    int32 ChangedEvents = 0;
    Session->OnSelectionChanged.AddLambda([&SelectionEvents]() { ++SelectionEvents; });
    Session->OnChanged.AddLambda([&ChangedEvents]() { ++ChangedEvents; });
    Session->SetSelectedNodeIds({Selected, Active, Selected, FGuid::NewGuid()}, Active);
    bool bPassed = TestEqual(TEXT("invalid and duplicate ids are filtered"), Session->GetSelectedNodeIds().Num(), 2);
    if (Session->GetSelectedNodeIds().Num() != 2) return false;
    bPassed &= TestEqual(TEXT("selection keeps requested order"), Session->GetSelectedNodeIds()[0], Selected);
    bPassed &= TestEqual(TEXT("active id is retained"), Session->GetActiveNodeId(), Active);

    FString Error;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-I selection identity delete preceding"));
        bPassed &= TestTrue(TEXT("delete preceding: ") + Error, Session->DeleteNode(Preceding, Error));
    }
    bPassed &= TestEqual(TEXT("selection did not move to an index neighbor"), Session->GetSelectedNodeIds()[0], Selected);
    bPassed &= TestEqual(TEXT("active survived preceding delete"), Session->GetActiveNodeId(), Active);
    bPassed &= TestEqual(TEXT("unrelated structure did not emit selection"), SelectionEvents, 1);
    bPassed &= TestTrue(TEXT("undo preceding delete"), GEditor->UndoTransaction());
    bPassed &= TestEqual(TEXT("selection keeps the same GUID after Undo"), Session->GetSelectedNodeIds()[0], Selected);
    bPassed &= TestEqual(TEXT("active keeps the same GUID after Undo"), Session->GetActiveNodeId(), Active);

    {
        const FScopedTransaction Transaction(INVTEXT("CE-I selection identity reorder preceding"));
        bPassed &= TestTrue(TEXT("reorder preceding: ") + Error, Session->ReparentNode(Preceding, FGuid(), INDEX_NONE, false, Error));
    }
    bPassed &= TestEqual(TEXT("selection survives preceding reorder"), Session->GetSelectedNodeIds()[0], Selected);
    bPassed &= TestTrue(TEXT("undo preceding reorder"), GEditor->UndoTransaction());
    bPassed &= TestEqual(TEXT("selection survives reorder Undo"), Session->GetSelectedNodeIds()[0], Selected);
    bPassed &= TestTrue(TEXT("authoring and restore notifications fired"), ChangedEvents >= 4);
    Session->Close();
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSessionCanonicalFailureDirtyTest,
    "Mimir.V5.Composite.EditMode.Commands.CanonicalFailureIsDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSessionCanonicalFailureDirtyTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FMHCompositeDocument Invalid = CommandDocument();
    Invalid.Nodes[0].Transform.TranslationCm.X = std::numeric_limits<double>::quiet_NaN();
    UMHCompositeEditSession* Session = NewObject<UMHCompositeEditSession>(GetTransientPackage());
    Session->Open(nullptr, nullptr, FString(), Invalid, 1);
    return TestTrue(TEXT("failed canonicalization cannot report a clean session"), Session->IsDirty());
}

} // namespace UE::MimirComposite::Tests
