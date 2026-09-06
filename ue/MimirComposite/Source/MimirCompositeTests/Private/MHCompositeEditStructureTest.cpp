#include "MHCompositeEditFixture.h"

#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FStructureV2Scope
{
    bool bPrevious = false;
    FStructureV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FStructureV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
    }
};

FString SelectorOf(const UMHCompositeEditDocument& Draft, const FGuid& Id)
{
    return Draft.GetSelector(Draft.FindNodeIndex(Id));
}

FVector WorldOf(const UMHCompositeEditProjection& Projection, const FGuid& Id)
{
    const USceneComponent* Component = Projection.FindComponentForNodeId(Id);
    return Component != nullptr ? Component->GetComponentLocation() : FVector(TNumericLimits<double>::Max());
}

} // namespace

// CE-4b1 (spec CE-4b, A09): structural commands on the draft — add, delete,
// duplicate, reparent — keep pre-order selectors and session ids consistent,
// the projection follows, and Undo restores order, ids and metadata.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditStructureCommandsTest,
    "Mimir.V5.Composite.EditMode.Structure.AddDeleteDuplicateReparentKeepIdsAndOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditStructureCommandsTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FStructureV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("projection"), Projection) || !TestNotNull(TEXT("draft"), Draft) || Draft->Num() != 4) return false;
    const FString Prefix = SecondPath + TEXT(">") + F.Child->LogicalName + TEXT(":");
    const FGuid Plain = Draft->GetNodeId(0);
    const FGuid Group = Draft->GetNodeId(1);
    const FGuid Grouped = Draft->GetNodeId(2);
    const FGuid Random = Draft->GetNodeId(3);
    const TArray<FGuid> IdsBefore = {Plain, Group, Grouped, Random};

    // Add: a mesh under the group becomes its last child and is projected.
    FGuid Added;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b1 test: add"));
        Added = Session->AddNode(Group, EMHCompositeNodeKind::Mesh, F.MeshC, TEXT("added"), FTransform(FVector(0.0, 0.0, 9.0)), Error);
    }
    bool bPassed = TestTrue(TEXT("add: ") + Error, Added.IsValid());
    bPassed &= TestEqual(TEXT("add: five nodes"), Draft->Num(), 5);
    bPassed &= TestEqual(TEXT("add: last child of the group"), SelectorOf(*Draft, Added), FString(TEXT("nodes[1]/children[1]")));
    bPassed &= TestEqual(TEXT("add: parent"), Draft->GetParentId(Added), Group);
    bPassed &= TestEqual(TEXT("add: the random node keeps its id at index 4"), Draft->GetNodeId(4), Random);
    USceneComponent* AddedComponent = Projection->FindComponentForOrigin(Prefix + TEXT("nodes[1]/children[1]"));
    bPassed &= TestTrue(TEXT("add: projected as a mesh component"), AddedComponent != nullptr && AddedComponent->IsA<UStaticMeshComponent>());
    bPassed &= TestTrue(TEXT("add: component maps to the new id"), AddedComponent != nullptr && Projection->GetNodeIdForComponent(AddedComponent) == Added);

    // Delete: the grouped leaf goes, the added one takes its place.
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b1 test: delete"));
        bPassed &= TestTrue(TEXT("delete: ") + Error, Session->DeleteNode(Grouped, Error));
    }
    bPassed &= TestEqual(TEXT("delete: four nodes"), Draft->Num(), 4);
    bPassed &= TestEqual(TEXT("delete: the id is gone"), Draft->FindNodeIndex(Grouped), static_cast<int32>(INDEX_NONE));
    bPassed &= TestEqual(TEXT("delete: the added node moved up"), SelectorOf(*Draft, Added), FString(TEXT("nodes[1]/children[0]")));
    bPassed &= TestNull(TEXT("delete: no second child projected"), Projection->FindComponentForOrigin(Prefix + TEXT("nodes[1]/children[1]")));

    // Duplicate: the group's copy follows it with fresh ids; the random node shifts.
    FGuid Copy;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b1 test: duplicate"));
        Copy = Session->DuplicateNode(Group, Error);
    }
    bPassed &= TestTrue(TEXT("duplicate: ") + Error, Copy.IsValid() && Copy != Group);
    bPassed &= TestEqual(TEXT("duplicate: six nodes"), Draft->Num(), 6);
    bPassed &= TestEqual(TEXT("duplicate: the copy follows the original"), SelectorOf(*Draft, Copy), FString(TEXT("nodes[2]")));
    bPassed &= TestEqual(TEXT("duplicate: the random node shifted"), SelectorOf(*Draft, Random), FString(TEXT("nodes[3]")));
    const TArray<FGuid> CopyChildren = Draft->GetChildIds(Copy);
    bPassed &= TestTrue(TEXT("duplicate: one child with a fresh id"), CopyChildren.Num() == 1 && CopyChildren[0].IsValid() && CopyChildren[0] != Added);
    bPassed &= TestTrue(TEXT("duplicate: the copy is projected"), Projection->FindComponentForOrigin(Prefix + TEXT("nodes[2]/children[0]")) != nullptr);
    bPassed &= TestEqual(TEXT("duplicate: originals keep their ids"), Draft->GetNodeId(0), Plain);

    // Reparent keeping the world: the plain leaf goes under the copy, stays where it renders.
    const FVector PlainWorld = WorldOf(*Projection, Plain);
    const FTransform PlainLocalBefore = Draft->GetNodes()[0].Transform;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b1 test: reparent"));
        bPassed &= TestTrue(TEXT("reparent: ") + Error, Session->ReparentNode(Plain, Copy, 0, true, Error));
    }
    bPassed &= TestEqual(TEXT("reparent: first child of the copy (now nodes[1])"), SelectorOf(*Draft, Plain), FString(TEXT("nodes[1]/children[0]")));
    bPassed &= TestEqual(TEXT("reparent: parent"), Draft->GetParentId(Plain), Copy);
    bPassed &= TestTrue(TEXT("reparent: the world position is kept"), WorldOf(*Projection, Plain).Equals(PlainWorld, 1e-2));
    bPassed &= TestFalse(TEXT("reparent: the local changed to keep the world"), Draft->GetNodes()[Draft->FindNodeIndex(Plain)].Transform.Equals(PlainLocalBefore, 1e-3));
    bPassed &= TestEqual(TEXT("reparent: still six nodes"), Draft->Num(), 6);

    // Undo everything: order, ids and the projection come back.
    for (int32 Step = 0; Step < 4; ++Step) bPassed &= TestTrue(TEXT("undo"), GEditor->UndoTransaction());
    bPassed &= TestTrue(TEXT("undo: the session survived"), Subsystem->GetEditSession() == Session && Session->IsOpen());
    bPassed &= TestEqual(TEXT("undo: four nodes"), Draft->Num(), 4);
    for (int32 Index = 0; Index < IdsBefore.Num(); ++Index) bPassed &= TestEqual(TEXT("undo: ids in order"), Draft->GetNodeId(Index), IdsBefore[Index]);
    bPassed &= TestEqual(TEXT("undo: the grouped leaf is back"), SelectorOf(*Draft, Grouped), FString(TEXT("nodes[1]/children[0]")));
    bPassed &= TestEqual(TEXT("undo: the added id is gone"), Draft->FindNodeIndex(Added), static_cast<int32>(INDEX_NONE));
    bPassed &= TestNull(TEXT("undo: the projection dropped the added node"), Projection->FindComponentForOrigin(Prefix + TEXT("nodes[1]/children[1]")));
    bPassed &= TestFalse(TEXT("undo: clean"), Session->IsDirty());
    for (int32 Step = 0; Step < 4; ++Step) bPassed &= TestTrue(TEXT("redo"), GEditor->RedoTransaction());
    bPassed &= TestEqual(TEXT("redo: six nodes"), Draft->Num(), 6);
    bPassed &= TestEqual(TEXT("redo: the plain leaf is under the copy again"), Draft->GetParentId(Plain), Copy);
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// CE-4b1: commands that would break the grammar or the tree are refused
// before the draft changes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditStructureRefusalTest,
    "Mimir.V5.Composite.EditMode.Structure.CommandsRefuseInvalidStructure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditStructureRefusalTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FStructureV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("draft"), Draft) || Draft->Num() != 4) return false;
    const FGuid Plain = Draft->GetNodeId(0);
    const FGuid Group = Draft->GetNodeId(1);
    const FGuid Grouped = Draft->GetNodeId(2);
    const uint32 Revision = Draft->GetRevision();

    bool bPassed = TestFalse(TEXT("a leaf cannot have children"), Session->AddNode(Plain, EMHCompositeNodeKind::Group, FString(), TEXT("under a mesh"), FTransform::Identity, Error).IsValid());
    bPassed &= TestFalse(TEXT("a mesh needs a resource"), Session->AddNode(Group, EMHCompositeNodeKind::Mesh, FString(), TEXT("no resource"), FTransform::Identity, Error).IsValid());
    bPassed &= TestFalse(TEXT("a group forbids a resource"), Session->AddNode(Group, EMHCompositeNodeKind::Group, F.MeshC, TEXT("group with resource"), FTransform::Identity, Error).IsValid());
    bPassed &= TestFalse(TEXT("a random node needs options (not this command)"), Session->AddNode(Group, EMHCompositeNodeKind::Random, FString(), TEXT("random"), FTransform::Identity, Error).IsValid());
    bPassed &= TestFalse(TEXT("an unknown parent"), Session->AddNode(FGuid::NewGuid(), EMHCompositeNodeKind::Group, FString(), TEXT("orphan"), FTransform::Identity, Error).IsValid());
    bPassed &= TestFalse(TEXT("no cycles: a group under its own child"), Session->ReparentNode(Group, Grouped, INDEX_NONE, false, Error));
    bPassed &= TestFalse(TEXT("no reparent onto a leaf"), Session->ReparentNode(Grouped, Plain, INDEX_NONE, false, Error));
    bPassed &= TestFalse(TEXT("no reparent onto itself"), Session->ReparentNode(Group, Group, INDEX_NONE, false, Error));
    bPassed &= TestFalse(TEXT("delete needs a known node"), Session->DeleteNode(FGuid::NewGuid(), Error));
    bPassed &= TestFalse(TEXT("duplicate needs a known node"), Session->DuplicateNode(FGuid::NewGuid(), Error).IsValid());
    bPassed &= TestEqual(TEXT("nothing changed"), Draft->GetRevision(), Revision);
    bPassed &= TestEqual(TEXT("still four nodes"), Draft->Num(), 4);
    bPassed &= TestFalse(TEXT("still clean"), Session->IsDirty());

    // Reordering among siblings is a reparent under the same parent.
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b1 test: reorder"));
        bPassed &= TestTrue(TEXT("reorder: ") + Error, Session->ReparentNode(Group, FGuid(), 0, false, Error));
    }
    bPassed &= TestEqual(TEXT("reorder: the group is first"), Draft->GetNodeId(0), Group);
    bPassed &= TestEqual(TEXT("reorder: its child follows"), Draft->GetNodeId(1), Grouped);
    bPassed &= TestEqual(TEXT("reorder: the plain leaf is third"), Draft->GetNodeId(2), Plain);
    bPassed &= TestTrue(TEXT("reorder: dirty"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
