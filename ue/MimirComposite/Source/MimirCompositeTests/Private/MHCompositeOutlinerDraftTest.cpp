#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"
#include "UI/MHCompositeOutlinerEditActions.h"
#include "UI/MHCompositeOutlinerModel.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FOutlinerDraftV2Scope
{
    bool bPrevious = false;
    FOutlinerDraftV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FOutlinerDraftV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
    }
};

/** Builds the model for the placement and opens the edited occurrence's rows. */
bool BuildOpened(FMHCompositeOutlinerModel& Model, AMHCompositeActor& Actor, const FString& InvocationPath)
{
    if (!Model.BuildFromActor(Actor)) return false;
    if (InvocationPath.IsEmpty()) return true;
    const TSharedPtr<FMHCompositeOutlinerItem> Invocation = Model.FindByNodePath(InvocationPath);
    return Invocation.IsValid() && Model.ExpandItem(Invocation);
}

} // namespace

// CE-4b2 (spec CE-4b "Outliner отображение только через согласованный API"):
// with a CE-backend session on the placement the Composite Outliner model
// shows the session draft for the edited occurrence (rows carry the session
// node id), binds those rows to the projection's components, and the
// freshness key follows the draft so a command triggers a rebuild.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHOutlinerShowsDraftTest,
    "Mimir.V5.Composite.EditMode.Structure.OutlinerShowsTheDraftAndBindsTheProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHOutlinerShowsDraftTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FOutlinerDraftV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    const FString Prefix = SecondPath + TEXT(">") + F.Child->LogicalName + TEXT(":");
    FString Error;

    // Before the session: the rows come from the child asset, no session ids.
    FMHCompositeOutlinerModel Before;
    bool bPassed = TestTrue(TEXT("model before"), BuildOpened(Before, *F.A, SecondPath));
    const TSharedPtr<FMHCompositeOutlinerItem> PlainBefore = Before.FindByNodePath(Prefix + TEXT("nodes[0]"));
    bPassed &= TestTrue(TEXT("asset rows carry no session id"), PlainBefore.IsValid() && !PlainBefore->DraftNodeId.IsValid());

    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("projection"), Projection) || !TestNotNull(TEXT("draft"), Draft) || Draft->Num() != 4) return false;
    const FGuid Group = Draft->GetNodeId(1);
    const FMHCompositeOutlinerFreshness FreshBefore = FMHCompositeOutlinerFreshness::Capture(*F.A);

    // In the session: the edited occurrence's rows are the draft's nodes.
    FMHCompositeOutlinerModel Model;
    bPassed &= TestTrue(TEXT("model in session"), BuildOpened(Model, *F.A, SecondPath));
    const TSharedPtr<FMHCompositeOutlinerItem> Plain = Model.FindByNodePath(Prefix + TEXT("nodes[0]"));
    bPassed &= TestTrue(TEXT("draft rows carry the session id"), Plain.IsValid() && Plain->DraftNodeId == Draft->GetNodeId(0));
    bPassed &= TestTrue(TEXT("draft rows bind the projection component"), Plain.IsValid() && Plain->PlacementComponent.Get() == Projection->FindComponentForOrigin(Prefix + TEXT("nodes[0]")));
    bPassed &= TestTrue(TEXT("the projection component finds its row"), Model.FindForComponent(Projection->FindComponentForOrigin(Prefix + TEXT("nodes[0]"))) == Plain);
    bPassed &= TestTrue(TEXT("draft rows carry the resolved overlay"), Plain.IsValid() && Plain->bHasResolvedOverlay);
    const TSharedPtr<FMHCompositeOutlinerItem> Other = Model.FindByNodePath(F.Root->LogicalName + TEXT(":nodes[0]"));
    bPassed &= TestTrue(TEXT("rows outside the occurrence stay asset rows"), Other.IsValid() && !Other->DraftNodeId.IsValid());

    // A command: the freshness key moves and the rebuilt model shows the new node, bound and overlaid.
    FGuid Added;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b2 test: add"));
        Added = Session->AddNode(Group, EMHCompositeNodeKind::Mesh, F.MeshC, TEXT("added"), FTransform(FVector(0.0, 0.0, 9.0)), Error);
    }
    if (!TestTrue(TEXT("add: ") + Error, Added.IsValid())) return false;
    bPassed &= TestFalse(TEXT("a draft command changes the freshness key"), FMHCompositeOutlinerFreshness::Capture(*F.A).Matches(FreshBefore));
    FMHCompositeOutlinerModel After;
    bPassed &= TestTrue(TEXT("model after"), BuildOpened(After, *F.A, SecondPath));
    const TSharedPtr<FMHCompositeOutlinerItem> AddedRow = After.FindByNodePath(Prefix + TEXT("nodes[1]/children[1]"));
    bPassed &= TestTrue(TEXT("the new node has a row"), AddedRow.IsValid() && AddedRow->DraftNodeId == Added && AddedRow->Resource == F.MeshC && AddedRow->Kind == EMHRandomSemanticKind::Mesh);
    bPassed &= TestTrue(TEXT("the new row binds its projection component"), AddedRow.IsValid() && AddedRow->PlacementComponent.Get() == Projection->FindComponentForOrigin(Prefix + TEXT("nodes[1]/children[1]")) && AddedRow->PlacementComponent.IsValid());
    bPassed &= TestTrue(TEXT("the new row carries the resolved overlay"), AddedRow.IsValid() && AddedRow->bHasResolvedOverlay);
    const TSharedPtr<FMHCompositeOutlinerItem> GroupRow = After.FindByNodePath(Prefix + TEXT("nodes[1]"));
    bPassed &= TestTrue(TEXT("the group counts two authored children"), GroupRow.IsValid() && GroupRow->AuthoredChildCount == 2);

    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    FMHCompositeOutlinerModel Closed;
    bPassed &= TestTrue(TEXT("model after cancel"), BuildOpened(Closed, *F.A, SecondPath));
    bPassed &= TestNull(TEXT("after cancel the added row is gone"), Closed.FindByNodePath(Prefix + TEXT("nodes[1]/children[1]")).Get());
    return bPassed;
}

// CE-4b2: what a drop / menu add becomes — a managed static mesh is a mesh
// node, a managed composite a composite node, anything else is refused;
// a group takes the node as a child, any other row as a sibling.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHOutlinerAddDescribeTest,
    "Mimir.V5.Composite.EditMode.Structure.OutlinerDropDescribesTheCommand",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHOutlinerAddDescribeTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FOutlinerDraftV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    const FString Prefix = SecondPath + TEXT(">") + F.Child->LogicalName + TEXT(":");
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("draft"), Draft) || Draft->Num() != 4) return false;
    FMHCompositeOutlinerModel Model;
    if (!TestTrue(TEXT("model"), BuildOpened(Model, *F.A, SecondPath))) return false;
    const TSharedPtr<FMHCompositeOutlinerItem> PlainRow = Model.FindByNodePath(Prefix + TEXT("nodes[0]"));
    const TSharedPtr<FMHCompositeOutlinerItem> GroupRow = Model.FindByNodePath(Prefix + TEXT("nodes[1]"));
    const TSharedPtr<FMHCompositeOutlinerItem> GroupedRow = Model.FindByNodePath(Prefix + TEXT("nodes[1]/children[0]"));
    if (!TestTrue(TEXT("rows"), PlainRow.IsValid() && GroupRow.IsValid() && GroupedRow.IsValid())) return false;

    bool bPassed = TestFalse(TEXT("a group takes the node as a child"), MHOutlinerAddParentFor(GroupRow.Get(), *Draft) != Draft->GetNodeId(1));
    bPassed &= TestTrue(TEXT("a leaf at the root takes it as a root sibling"), !MHOutlinerAddParentFor(PlainRow.Get(), *Draft).IsValid());
    bPassed &= TestTrue(TEXT("a grouped leaf takes it as a sibling under the group"), MHOutlinerAddParentFor(GroupedRow.Get(), *Draft) == Draft->GetNodeId(1));
    bPassed &= TestTrue(TEXT("no row: a root"), !MHOutlinerAddParentFor(nullptr, *Draft).IsValid());

    FMHOutlinerAddRequest Request;
    bPassed &= TestTrue(TEXT("a managed static mesh: ") + Error, MHDescribeOutlinerAssetAdd(F.MeshAssetC, GroupRow.Get(), *Draft, Request, Error));
    bPassed &= TestTrue(TEXT("mesh node under the group"), Request.Kind == EMHCompositeNodeKind::Mesh && Request.Resource == F.MeshC && Request.ParentId == Draft->GetNodeId(1));
    bPassed &= TestTrue(TEXT("a managed composite: ") + Error, MHDescribeOutlinerAssetAdd(F.Child, PlainRow.Get(), *Draft, Request, Error));
    bPassed &= TestTrue(TEXT("composite node at the root"), Request.Kind == EMHCompositeNodeKind::Composite && Request.Resource == F.Child->LogicalName && !Request.ParentId.IsValid());
    bPassed &= TestFalse(TEXT("a foreign object is refused"), MHDescribeOutlinerAssetAdd(GetTransientPackage(), GroupRow.Get(), *Draft, Request, Error));
    bPassed &= TestFalse(TEXT("nothing is refused"), MHDescribeOutlinerAssetAdd(nullptr, GroupRow.Get(), *Draft, Request, Error));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
