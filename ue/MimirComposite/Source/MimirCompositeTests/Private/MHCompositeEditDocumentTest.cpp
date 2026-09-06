#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** child = [mesh @ (0,0,40)] [group @ (10,0,0) { mesh @ (0,0,5) }] [random @ (0,0,80) { mesh w1 | empty w0 }], with provenance fields set. */
FMHCompositeDocument EditDocumentSample()
{
    FMHCompositeDocument Document;
    FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
    Leaf.Kind = EMHCompositeNodeKind::Mesh;
    Leaf.Resource = TEXT("mesh_c");
    Leaf.Name = TEXT("plain");
    Leaf.Transform.TranslationCm = FVector(0.0, 0.0, 40.0);
    Leaf.PlaceType = 3;
    FMHCompositeNode& Group = Document.Nodes.AddDefaulted_GetRef();
    Group.Kind = EMHCompositeNodeKind::Group;
    Group.Name = TEXT("grp");
    Group.Transform.TranslationCm = FVector(10.0, 0.0, 0.0);
    Group.bAppearanceSeedBoundary = true;
    FMHCompositeNode& Grouped = Group.Children.AddDefaulted_GetRef();
    Grouped.Kind = EMHCompositeNodeKind::Mesh;
    Grouped.Resource = TEXT("mesh_c");
    Grouped.Transform.TranslationCm = FVector(0.0, 0.0, 5.0);
    Grouped.Transform.RotationQuat = FQuat(FRotator(0.0, 30.0, 0.0));
    FMHCompositeNode& Random = Document.Nodes.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Name = TEXT("pick");
    Random.Transform.TranslationCm = FVector(0.0, 0.0, 80.0);
    FMHCompositeOption& MeshOption = Random.Options.AddDefaulted_GetRef();
    MeshOption.Kind = EMHCompositeOptionKind::Mesh;
    MeshOption.Resource = TEXT("mesh_a");
    MeshOption.Weight = 1.0f;
    FMHCompositeOption& EmptyOption = Random.Options.AddDefaulted_GetRef();
    EmptyOption.Kind = EMHCompositeOptionKind::Empty;
    EmptyOption.Weight = 0.0f;
    return Document;
}

bool Canonical(const FMHCompositeDocument& Document, TArray<uint8>& OutBytes)
{
    FString Error;
    return MHWriteCanonicalCompositeV5(Document, OutBytes, Error);
}

} // namespace

// CE-1 (spec §5.2): the draft round-trips the whole authoring model and takes
// part in native transactions — Undo restores content and session ids alike.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditDocumentRoundTripTest,
    "Mimir.V5.Composite.EditMode.Document.RoundTripKeepsEveryField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditDocumentRoundTripTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FMHCompositeDocument Sample = EditDocumentSample();
    TArray<uint8> SampleBytes;
    if (!TestTrue(TEXT("sample is canonical"), Canonical(Sample, SampleBytes))) return false;
    UMHCompositeEditDocument* Draft = NewObject<UMHCompositeEditDocument>(GetTransientPackage());
    Draft->Load(Sample);
    bool bPassed = TestEqual(TEXT("four nodes in pre-order"), Draft->Num(), 4);
    TSet<FGuid> Ids;
    for (int32 Index = 0; Index < Draft->Num(); ++Index) Ids.Add(Draft->GetNodeId(Index));
    bPassed &= TestTrue(TEXT("every node has a distinct valid id"), Ids.Num() == 4 && !Ids.Contains(FGuid()));
    bPassed &= TestEqual(TEXT("grouped mesh is node 2"), Draft->FindNodeIndexBySelector(TEXT("nodes[1]/children[0]")), 2);
    bPassed &= TestEqual(TEXT("random node is node 3"), Draft->FindNodeIndexBySelector(TEXT("nodes[2]")), 3);
    bPassed &= TestEqual(TEXT("selectors round-trip"), Draft->GetSelector(2), FString(TEXT("nodes[1]/children[0]")));
    bPassed &= TestEqual(TEXT("an option is not a node"), Draft->FindNodeIndexBySelector(TEXT("nodes[2]/options[1]")), static_cast<int32>(INDEX_NONE));
    bPassed &= TestEqual(TEXT("ids resolve back to indices"), Draft->FindNodeIndex(Draft->GetNodeId(3)), 3);
    FMHCompositeDocument Extracted;
    FString Error;
    TArray<uint8> ExtractedBytes;
    bPassed &= TestTrue(TEXT("extract: ") + Error, Draft->Extract(Extracted, Error) && Canonical(Extracted, ExtractedBytes));
    bPassed &= TestTrue(TEXT("the draft is byte-identical to the sample"), ExtractedBytes == SampleBytes);
    if (Extracted.Nodes.Num() != 3 || Extracted.Nodes[2].Options.Num() != 2) return false;
    bPassed &= TestTrue(TEXT("options, weights and empties survive"),
        Extracted.Nodes.Num() == 3 && Extracted.Nodes[2].Options.Num() == 2 &&
        Extracted.Nodes[2].Options[1].Kind == EMHCompositeOptionKind::Empty && Extracted.Nodes[2].Options[1].Weight == 0.0f &&
        Extracted.Nodes[0].PlaceType == 3 && Extracted.Nodes[1].bAppearanceSeedBoundary);
    TArray<uint8> DraftBytes;
    bPassed &= TestTrue(TEXT("canonical bytes helper agrees"), Draft->CanonicalBytes(DraftBytes, Error) && DraftBytes == SampleBytes);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditDocumentUndoTest,
    "Mimir.V5.Composite.EditMode.Document.TransformCommandIsUndoable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditDocumentUndoTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (GEditor == nullptr || GEditor->Trans == nullptr) return false;
    const FMHCompositeDocument Sample = EditDocumentSample();
    TArray<uint8> SampleBytes;
    if (!TestTrue(TEXT("sample is canonical"), Canonical(Sample, SampleBytes))) return false;
    UMHCompositeEditDocument* Draft = NewObject<UMHCompositeEditDocument>(GetTransientPackage(), NAME_None, RF_Transactional);
    Draft->Load(Sample);
    const FGuid GroupedId = Draft->GetNodeId(2);
    const uint64 SerialBefore = Draft->GetChangeSerial();
    const uint32 RevisionBefore = Draft->GetRevision();
    FString Error;
    bool bPassed = TestFalse(TEXT("unknown id is refused"), Draft->SetNodeTransform(FGuid::NewGuid(), FTransform::Identity, Error));
    bool bMoved = false;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-1 test: move grouped mesh"));
        bMoved = Draft->SetNodeTransform(GroupedId, FTransform(FVector(0.0, 0.0, 105.0)), Error);
        bPassed &= TestTrue(TEXT("move: ") + Error, bMoved);
    }
    // An empty transaction would make Undo roll back an unrelated earlier one.
    if (!bMoved) return false;
    bPassed &= TestTrue(TEXT("change serial and revision advanced"), Draft->GetChangeSerial() != SerialBefore && Draft->GetRevision() == RevisionBefore + 1);
    FMHCompositeDocument Moved;
    bPassed &= TestTrue(TEXT("extract moved"), Draft->Extract(Moved, Error) && Moved.Nodes[1].Children[0].Transform.TranslationCm.Equals(FVector(0.0, 0.0, 105.0), 1e-3));
    bPassed &= TestTrue(TEXT("undo runs"), GEditor->UndoTransaction());
    TArray<uint8> AfterUndo;
    bPassed &= TestTrue(TEXT("undo restores the draft byte-for-byte"), Draft->CanonicalBytes(AfterUndo, Error) && AfterUndo == SampleBytes);
    bPassed &= TestTrue(TEXT("undo keeps the session ids"), Draft->GetNodeId(2) == GroupedId && Draft->FindNodeIndex(GroupedId) == 2);
    bPassed &= TestEqual(TEXT("undo restores the revision"), Draft->GetRevision(), RevisionBefore);
    bPassed &= TestTrue(TEXT("redo runs"), GEditor->RedoTransaction());
    FMHCompositeDocument Redone;
    bPassed &= TestTrue(TEXT("redo re-applies the move"), Draft->Extract(Redone, Error) && Redone.Nodes[1].Children[0].Transform.TranslationCm.Equals(FVector(0.0, 0.0, 105.0), 1e-3));
    bPassed &= TestTrue(TEXT("undo again"), GEditor->UndoTransaction());
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
