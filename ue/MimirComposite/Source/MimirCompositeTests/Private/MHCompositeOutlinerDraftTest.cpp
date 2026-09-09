#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "ScopedTransaction.h"
#include "UI/MHCompositeOutliner.h"
#include "UI/MHCompositeOutlinerEditActions.h"
#include "UI/MHCompositeOutlinerModel.h"
#include "Widgets/Text/STextBlock.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Builds the model for the placement and opens the edited occurrence's rows. */
bool BuildOpened(FMHCompositeOutlinerModel& Model, AMHCompositeActor& Actor, const FString& InvocationPath)
{
    if (!Model.BuildFromActor(Actor)) return false;
    if (InvocationPath.IsEmpty()) return true;
    const TSharedPtr<FMHCompositeOutlinerItem> Invocation = Model.FindByNodePath(InvocationPath);
    return Invocation.IsValid() && Model.ExpandItem(Invocation);
}

bool WidgetTreeContainsText(SWidget& Root, const FString& Expected)
{
    TArray<SWidget*> Stack{&Root};
    while (!Stack.IsEmpty())
    {
        SWidget* Widget = Stack.Pop();
        if (Widget->GetType() == FName(TEXT("STextBlock")) &&
            static_cast<STextBlock*>(Widget)->GetText().ToString().Contains(Expected))
        {
            return true;
        }
        FChildren* Children = Widget->GetChildren();
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            Stack.Add(&Children->GetChildAt(Index).Get());
        }
    }
    return false;
}

} // namespace

// CE-4b2 (spec CE-4b "Outliner отображение только через согласованный API"):
// with an edit session on the placement the Composite Outliner model
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
// every authored node can be an exact parent; root is an explicit destination.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHOutlinerAddDescribeTest,
    "Mimir.V5.Composite.EditMode.Structure.OutlinerDropDescribesTheCommand",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHOutlinerAddDescribeTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
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

    FGuid ParentId;
    bool bPassed = TestTrue(TEXT("a group is the exact parent: ") + Error,
        MHResolveOutlinerAddParent(GroupRow.Get(), false, *Draft, ParentId, Error) && ParentId == Draft->GetNodeId(1));
    bPassed &= TestTrue(TEXT("a resource node is also the exact parent: ") + Error,
        MHResolveOutlinerAddParent(PlainRow.Get(), false, *Draft, ParentId, Error) && ParentId == Draft->GetNodeId(0));
    bPassed &= TestTrue(TEXT("a nested resource node is the exact parent: ") + Error,
        MHResolveOutlinerAddParent(GroupedRow.Get(), false, *Draft, ParentId, Error) && ParentId == Draft->GetNodeId(2));
    bPassed &= TestFalse(TEXT("no row is not silently treated as root"),
        MHResolveOutlinerAddParent(nullptr, false, *Draft, ParentId, Error));
    bPassed &= TestTrue(TEXT("Current Composite / Root is explicit: ") + Error,
        MHResolveOutlinerAddParent(nullptr, true, *Draft, ParentId, Error) && !ParentId.IsValid());
    int32 SiblingIndex = INDEX_NONE;
    bPassed &= TestTrue(TEXT("above root row inserts at its exact index: ") + Error,
        MHResolveOutlinerSiblingInsertion(PlainRow.Get(), false, *Draft, ParentId, SiblingIndex, Error) &&
        !ParentId.IsValid() && SiblingIndex == 0);
    bPassed &= TestTrue(TEXT("below root row inserts after its exact index: ") + Error,
        MHResolveOutlinerSiblingInsertion(PlainRow.Get(), true, *Draft, ParentId, SiblingIndex, Error) &&
        !ParentId.IsValid() && SiblingIndex == 1);
    bPassed &= TestTrue(TEXT("below nested row keeps its authored parent: ") + Error,
        MHResolveOutlinerSiblingInsertion(GroupedRow.Get(), true, *Draft, ParentId, SiblingIndex, Error) &&
        ParentId == Draft->GetNodeId(1) && SiblingIndex == 1);

    TSharedRef<FMHCompositeOutlinerItem> Locked = MakeShared<FMHCompositeOutlinerItem>();
    bPassed &= TestFalse(TEXT("locked context is refused"),
        MHResolveOutlinerAddParent(&Locked.Get(), false, *Draft, ParentId, Error));
    TSharedRef<FMHCompositeOutlinerItem> Option = MakeShared<FMHCompositeOutlinerItem>();
    Option->ItemType = EMHCompositeOutlinerItemType::Option;
    Option->DraftNodeId = Draft->GetNodeId(1);
    bPassed &= TestFalse(TEXT("an entity row is refused"),
        MHResolveOutlinerAddParent(&Option.Get(), false, *Draft, ParentId, Error));

    FMHOutlinerAddRequest Request;
    bPassed &= TestTrue(TEXT("a managed static mesh: ") + Error, MHDescribeOutlinerAssetAdd(F.MeshAssetC, GroupRow.Get(), *Draft, Request, Error));
    bPassed &= TestTrue(TEXT("mesh node under the group"), Request.Kind == EMHCompositeNodeKind::Mesh && Request.Resource == F.MeshC && Request.ParentId == Draft->GetNodeId(1));
    bPassed &= TestTrue(TEXT("a managed composite: ") + Error, MHDescribeOutlinerAssetAdd(F.Child, PlainRow.Get(), *Draft, Request, Error));
    bPassed &= TestTrue(TEXT("composite node is a child of the selected resource node"), Request.Kind == EMHCompositeNodeKind::Composite && Request.Resource == F.Child->LogicalName && Request.ParentId == Draft->GetNodeId(0));
    bPassed &= TestFalse(TEXT("a foreign object is refused"), MHDescribeOutlinerAssetAdd(GetTransientPackage(), GroupRow.Get(), *Draft, Request, Error));
    bPassed &= TestFalse(TEXT("nothing is refused"), MHDescribeOutlinerAssetAdd(nullptr, GroupRow.Get(), *Draft, Request, Error));

    TArray<UObject*> ValidAssets{F.MeshAssetC, F.Child};
    TArray<FMHOutlinerAddRequest> Batch;
    bPassed &= TestTrue(TEXT("root batch validates as a whole: ") + Error,
        MHDescribeOutlinerAssetAddBatch(ValidAssets, nullptr, true, *Draft, Batch, Error));
    bPassed &= TestEqual(TEXT("root batch keeps every selected asset"), Batch.Num(), 2);
    bPassed &= TestTrue(TEXT("root batch has the explicit root destination"),
        Batch.Num() == 2 && !Batch[0].ParentId.IsValid() && !Batch[1].ParentId.IsValid());
    TArray<UObject*> MixedAssets{F.MeshAssetC, GetTransientPackage()};
    bPassed &= TestFalse(TEXT("a mixed invalid batch is refused"),
        MHDescribeOutlinerAssetAddBatch(MixedAssets, GroupRow.Get(), false, *Draft, Batch, Error));
    bPassed &= TestTrue(TEXT("a refused batch exposes no partial requests"), Batch.IsEmpty());

    FMHCompositeOption DescribedOption;
    bPassed &= TestTrue(TEXT("managed composite becomes a weight-one variant: ") + Error,
        MHDescribeOutlinerAssetOption(F.Child, DescribedOption, Error));
    bPassed &= TestTrue(TEXT("variant descriptor"), DescribedOption.Kind == EMHCompositeOptionKind::Composite &&
        DescribedOption.Resource == F.Child->LogicalName && DescribedOption.Weight == 1.0f);

    const FMHOutlinerCommandStamp Stamp = FMHOutlinerCommandStamp::Capture(*Session);
    bPassed &= TestTrue(TEXT("fresh deferred command stamp matches"), Stamp.Matches(*Session));
    bPassed &= TestTrue(TEXT("change for stale stamp: ") + Error,
        Session->SetNodeName(Draft->GetNodeId(0), TEXT("serial_advanced"), Error));
    bPassed &= TestFalse(TEXT("a draft change rejects the stale callback"), Stamp.Matches(*Session));
    const FMHOutlinerCommandStamp ClosingStamp = FMHOutlinerCommandStamp::Capture(*Session);
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("a closed session rejects its captured callback"), ClosingStamp.Matches(*Session));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHOutlinerPinsActiveSessionRootTest,
    "Mimir.V5.Composite.EditMode.Structure.OutlinerPinsActiveSessionRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHOutlinerPinsActiveSessionRootTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Invocation = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("nested invocation"), Invocation)) return false;

    GEditor->SelectNone(false, true, false);
    GEditor->SelectActor(F.A, true, true, true);
    TSharedRef<SWidget> Outliner = MHCreateCompositeOutlinerWidget();
    bool bPassed = TestTrue(TEXT("without Edit the Outliner shows no session"),
        WidgetTreeContainsText(Outliner.Get(), TEXT("No composite edit session")));
    bPassed &= TestFalse(TEXT("native placement selection does not populate an Outliner outside Edit"),
        WidgetTreeContainsText(Outliner.Get(), F.Root->LogicalName));

    FString Error;
    if (!TestTrue(TEXT("begin nested edit: ") + Error,
            Subsystem->BeginEditNestedComposite(F.A, Invocation->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    if (!TestNotNull(TEXT("session"), Session) || !TestNotNull(TEXT("projection"), Projection) ||
        !TestNotNull(TEXT("mode"), Mode)) return false;

    // Enter has no logical node selection, so the native editor selection is empty.
    bPassed &= TestEqual(TEXT("enter selects no infrastructure actor"), GEditor->GetSelectedActorCount(), 0);
    bPassed &= TestTrue(TEXT("empty native selection keeps the active session tree"),
        WidgetTreeContainsText(Outliner.Get(), F.Root->LogicalName));
    bPassed &= TestTrue(TEXT("authoring toolbar exposes Add Nodes"),
        WidgetTreeContainsText(Outliner.Get(), TEXT("Add Nodes")));
    bPassed &= TestTrue(TEXT("authoring toolbar exposes Add Entities"),
        WidgetTreeContainsText(Outliner.Get(), TEXT("Add Entities")));
    bPassed &= TestTrue(TEXT("first attach identifies the nested edited definition"),
        WidgetTreeContainsText(Outliner.Get(), TEXT("Composite Edit — ") + F.Child->LogicalName));

    const FGuid NodeId = Session->GetDraft()->GetNodeId(0);
    Mode->SelectNodeIds({NodeId}, NodeId);
    bPassed &= TestTrue(TEXT("a node selection selects the projection actor"),
        Projection->GetProjectionActor()->IsSelected());
    bPassed &= TestTrue(TEXT("projection actor/component selection keeps the active session tree"),
        WidgetTreeContainsText(Outliner.Get(), F.Root->LogicalName));

    // Replaces standalone InstanceSelectionRetention: editor selection can no
    // longer switch this panel to a different composite or a pooled owner.
    F.B->SetCompositeAsset(F.Child);
    GEditor->SelectNone(false, true, false);
    GEditor->SelectActor(F.B, true, true, true);
    bPassed &= TestTrue(TEXT("another native actor cannot replace the active session tree"),
        WidgetTreeContainsText(Outliner.Get(), F.Root->LogicalName));

    const uint32 NestedEpoch = Session->GetEpoch();
    bPassed &= TestTrue(TEXT("switch to root in the same panel"), Mode->RequestSwitch(FString()));
    bPassed &= TestTrue(TEXT("switch keeps the session object"), Subsystem->GetEditSession() == Session);
    bPassed &= TestTrue(TEXT("switch advances the session epoch"), Session->GetEpoch() > NestedEpoch);
    bPassed &= TestTrue(TEXT("scope change refreshes the retained panel"),
        WidgetTreeContainsText(Outliner.Get(), TEXT("Composite Edit — ") + F.Root->LogicalName));

    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestTrue(TEXT("closed session empties the Outliner"),
        WidgetTreeContainsText(Outliner.Get(), TEXT("No composite edit session")));
    GEditor->SelectActor(F.A, true, true, true);
    bPassed &= TestFalse(TEXT("native placement selection does not repopulate the Outliner after cancel"),
        WidgetTreeContainsText(Outliner.Get(), F.Root->LogicalName));

    // Closing with no logical or native selection must also notify the panel;
    // there may be no actor-selection event at all on this path.
    if (!TestTrue(TEXT("reopen root edit"), Subsystem->BeginEditComposite(F.A, Error))) return false;
    bPassed &= TestTrue(TEXT("mode re-entry repopulates the retained widget"),
        WidgetTreeContainsText(Outliner.Get(), F.Root->LogicalName));
    bPassed &= TestTrue(TEXT("reopened session has no selected node"), Subsystem->GetEditSession()->GetSelectedNodeIds().IsEmpty());
    bPassed &= TestTrue(TEXT("cancel without selecting any node"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestTrue(TEXT("empty-selection close immediately empties the Outliner"),
        WidgetTreeContainsText(Outliner.Get(), TEXT("No composite edit session")));
    bPassed &= TestFalse(TEXT("empty-selection close removes the previous root"),
        WidgetTreeContainsText(Outliner.Get(), F.Root->LogicalName));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
