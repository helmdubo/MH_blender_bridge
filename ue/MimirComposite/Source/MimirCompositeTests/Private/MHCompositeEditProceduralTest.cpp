#include "MHCompositeEditFixture.h"

#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FProceduralV2Scope
{
    bool bPrevious = false;
    FProceduralV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FProceduralV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
    }
};

FMHCompositeOption MakeOption(const EMHCompositeOptionKind Kind, const FString& Resource, const float Weight)
{
    FMHCompositeOption Option;
    Option.Kind = Kind;
    Option.Resource = Resource;
    Option.Weight = Weight;
    return Option;
}

const FMHCompositeAssetNode* NodeOf(const UMHCompositeEditDocument& Draft, const FGuid& Id)
{
    const int32 Index = Draft.FindNodeIndex(Id);
    return Index != INDEX_NONE ? &Draft.GetNodes()[Index] : nullptr;
}

UStaticMesh* ProjectedMesh(const UMHCompositeEditProjection& Projection, const FString& Origin)
{
    const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Projection.FindComponentForOrigin(Origin));
    return Mesh != nullptr ? Mesh->GetStaticMesh() : nullptr;
}

} // namespace

// CE-4b3 (spec CE-4b "random option/weight editing", A14): name, resource and
// random options are draft commands under the protocol grammar — validated
// before Modify(), projected after, restored by Undo.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditProceduralCommandsTest,
    "Mimir.V5.Composite.EditMode.Structure.MetadataAndRandomCommandsFollowTheGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditProceduralCommandsTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FProceduralV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
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
    const FGuid Random = Draft->GetNodeId(3);
    const FString PlainNameBefore = NodeOf(*Draft, Plain)->Name;
    bool bPassed = TestTrue(TEXT("the plain leaf shows mesh C"), ProjectedMesh(*Projection, Prefix + TEXT("nodes[0]")) == F.MeshAssetC);

    // Name and resource.
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b3 test: rename"));
        bPassed &= TestTrue(TEXT("rename: ") + Error, Session->SetNodeName(Plain, TEXT("renamed"), Error));
    }
    bPassed &= TestEqual(TEXT("rename: the draft carries the name"), NodeOf(*Draft, Plain)->Name, FString(TEXT("renamed")));
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b3 test: resource"));
        bPassed &= TestTrue(TEXT("resource: ") + Error, Session->SetNodeResource(Plain, F.MeshA, Error));
    }
    bPassed &= TestEqual(TEXT("resource: the draft carries it"), NodeOf(*Draft, Plain)->Resource, F.MeshA);
    bPassed &= TestTrue(TEXT("resource: the projection shows mesh A now"), ProjectedMesh(*Projection, Prefix + TEXT("nodes[0]")) == F.MeshAssetA);

    // Refusals leave the draft alone.
    const uint32 Revision = Draft->GetRevision();
    bPassed &= TestFalse(TEXT("a group takes no resource"), Session->SetNodeResource(Group, F.MeshA, Error));
    bPassed &= TestFalse(TEXT("a resource must be canonical"), Session->SetNodeResource(Plain, TEXT("Bad Name"), Error));
    bPassed &= TestFalse(TEXT("an unknown node cannot be renamed"), Session->SetNodeName(FGuid::NewGuid(), TEXT("x"), Error));
    bPassed &= TestFalse(TEXT("options belong to random nodes only"), Session->SetNodeOptions(Plain, {MakeOption(EMHCompositeOptionKind::Empty, FString(), 1.0f)}, Error));
    bPassed &= TestFalse(TEXT("no options"), Session->SetNodeOptions(Random, {}, Error));
    bPassed &= TestFalse(TEXT("all weights zero"), Session->SetNodeOptions(Random, {MakeOption(EMHCompositeOptionKind::Empty, FString(), 0.0f)}, Error));
    bPassed &= TestFalse(TEXT("an empty option forbids a resource"), Session->SetNodeOptions(Random, {MakeOption(EMHCompositeOptionKind::Empty, F.MeshA, 1.0f)}, Error));
    bPassed &= TestFalse(TEXT("a mesh option needs a resource"), Session->SetNodeOptions(Random, {MakeOption(EMHCompositeOptionKind::Mesh, FString(), 1.0f)}, Error));
    bPassed &= TestFalse(TEXT("a negative weight"), Session->SetNodeOptions(Random, {MakeOption(EMHCompositeOptionKind::Mesh, F.MeshA, -1.0f)}, Error));
    bPassed &= TestFalse(TEXT("a random node needs valid options to be added"), Session->AddRandomNode(FGuid(), TEXT("bad"), FTransform::Identity, {}, Error).IsValid());
    bPassed &= TestEqual(TEXT("refusals changed nothing"), Draft->GetRevision(), Revision);

    // A new random node at the root, then its options replaced by one mesh option.
    FGuid Pick;
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b3 test: add random"));
        Pick = Session->AddRandomNode(FGuid(), TEXT("pick2"), FTransform(FVector(0.0, 0.0, 120.0)), {MakeOption(EMHCompositeOptionKind::Mesh, F.MeshC, 1.0f), MakeOption(EMHCompositeOptionKind::Empty, FString(), 1.0f)}, Error);
    }
    // No node of ours: nothing below (nor the Undo at the end) is about us.
    if (!TestTrue(TEXT("add random: ") + Error, Pick.IsValid())) return false;
    bPassed &= TestEqual(TEXT("add random: five nodes"), Draft->Num(), 5);
    bPassed &= TestTrue(TEXT("add random: a random node with two options at the root"), NodeOf(*Draft, Pick) != nullptr && NodeOf(*Draft, Pick)->Kind == EMHCompositeNodeKind::Random && NodeOf(*Draft, Pick)->Options.Num() == 2 && NodeOf(*Draft, Pick)->ParentIndex == INDEX_NONE);
    // Three roots before (plain, group, random): the new root is nodes[3].
    const FString PickSelector = Draft->GetSelector(Draft->FindNodeIndex(Pick));
    bPassed &= TestEqual(TEXT("add random: selector"), PickSelector, FString(TEXT("nodes[3]")));
    bPassed &= TestNotNull(TEXT("add random: the node has a handle"), Projection->FindComponentForOrigin(Prefix + PickSelector));
    {
        const FScopedTransaction Transaction(INVTEXT("CE-4b3 test: options"));
        bPassed &= TestTrue(TEXT("options: ") + Error, Session->SetNodeOptions(Pick, {MakeOption(EMHCompositeOptionKind::Mesh, F.MeshA, 2.0f)}, Error));
    }
    bPassed &= TestTrue(TEXT("options: the draft carries one option"), NodeOf(*Draft, Pick)->Options.Num() == 1 && FMath::IsNearlyEqual(NodeOf(*Draft, Pick)->Options[0].Weight, 2.0f));
    bPassed &= TestTrue(TEXT("options: the only option is picked and projected as mesh A"), ProjectedMesh(*Projection, Prefix + PickSelector + TEXT("/options[0]")) == F.MeshAssetA);

    // Undo everything.
    for (int32 Step = 0; Step < 4; ++Step) bPassed &= TestTrue(TEXT("undo"), GEditor->UndoTransaction());
    bPassed &= TestEqual(TEXT("undo: four nodes"), Draft->Num(), 4);
    bPassed &= TestTrue(TEXT("undo: the name is back"), NodeOf(*Draft, Plain) != nullptr && NodeOf(*Draft, Plain)->Name == PlainNameBefore);
    bPassed &= TestTrue(TEXT("undo: the resource is back"), NodeOf(*Draft, Plain) != nullptr && NodeOf(*Draft, Plain)->Resource == F.MeshC);
    bPassed &= TestTrue(TEXT("undo: the projection shows mesh C again"), ProjectedMesh(*Projection, Prefix + TEXT("nodes[0]")) == F.MeshAssetC);
    bPassed &= TestFalse(TEXT("undo: clean"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
