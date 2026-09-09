#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"

#include <limits>

namespace UE::MimirComposite::Tests
{
namespace
{

FMHCompositeOption AuthoringOption(const EMHCompositeOptionKind Kind, const FString& Resource, const float Weight)
{
    FMHCompositeOption Option;
    Option.Kind = Kind;
    Option.Resource = Resource;
    Option.Weight = Weight;
    return Option;
}

FMHCompositeNodeAdd AuthoringNode(
    const EMHCompositeNodeKind Kind,
    const FString& Resource,
    const FString& Name,
    const FTransform& Transform = FTransform::Identity)
{
    FMHCompositeNodeAdd Request;
    Request.Kind = Kind;
    Request.Resource = Resource;
    Request.Name = Name;
    Request.LocalTransform = Transform;
    return Request;
}

const FMHCompositeAssetNode* AuthoringNodeOf(const UMHCompositeEditDocument& Draft, const FGuid& Id)
{
    const int32 Index = Draft.FindNodeIndex(Id);
    return Index != INDEX_NONE ? &Draft.GetNodes()[Index] : nullptr;
}

bool SameIds(const UMHCompositeEditDocument& Draft, const TArray<FGuid>& Expected)
{
    if (Draft.Num() != Expected.Num()) return false;
    for (int32 Index = 0; Index < Expected.Num(); ++Index)
    {
        if (Draft.GetNodeId(Index) != Expected[Index]) return false;
    }
    return true;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditAuthoringAtomicBatchTest,
    "Mimir.V5.Composite.EditMode.Authoring.AtomicNodeBatchValidatesBeforeMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditAuthoringAtomicBatchTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    FMHCompositeDocument Source;
    Source.Nodes.AddDefaulted_GetRef().Kind = EMHCompositeNodeKind::Group;
    UMHCompositeEditDocument* Draft = NewObject<UMHCompositeEditDocument>(GetTransientPackage(), NAME_None, RF_Transactional);
    Draft->Load(Source);
    const FGuid Parent = Draft->GetNodeId(0);
    const TArray<FGuid> IdsBefore = {Parent};
    const uint32 RevisionBefore = Draft->GetRevision();
    TArray<uint8> BytesBefore;
    FString Error;
    if (!TestTrue(TEXT("initial canonical bytes: ") + Error, Draft->CanonicalBytes(BytesBefore, Error))) return false;

    TArray<FMHCompositeNodeAdd> Mixed = {
        AuthoringNode(EMHCompositeNodeKind::Mesh, TEXT("mesh_a"), TEXT("valid"), FTransform(FVector(1.0, 2.0, 3.0))),
        AuthoringNode(EMHCompositeNodeKind::Composite, TEXT("Bad Resource"), TEXT("invalid"))};
    TArray<FGuid> Added = {FGuid::NewGuid()};
    bool bPassed = TestFalse(TEXT("one invalid resource refuses the whole batch"), Draft->AddNodes(Parent, Mixed, Added, Error));
    TArray<uint8> BytesAfter;
    bPassed &= TestTrue(TEXT("canonical bytes remain writable"), Draft->CanonicalBytes(BytesAfter, Error));
    bPassed &= TestTrue(TEXT("invalid batch preserves bytes"), BytesAfter == BytesBefore);
    bPassed &= TestTrue(TEXT("invalid batch returns no ids"), Added.IsEmpty());
    bPassed &= TestTrue(TEXT("invalid batch preserves ids"), SameIds(*Draft, IdsBefore));
    bPassed &= TestEqual(TEXT("invalid batch preserves revision"), Draft->GetRevision(), RevisionBefore);

    FTransform NonFinite = FTransform::Identity;
    NonFinite.SetTranslation(FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0));
    Mixed = {
        AuthoringNode(EMHCompositeNodeKind::Mesh, TEXT("mesh_a"), TEXT("valid")),
        AuthoringNode(EMHCompositeNodeKind::Group, FString(), TEXT("bad transform"), NonFinite)};
    bPassed &= TestFalse(TEXT("one invalid transform refuses the whole batch"), Draft->AddNodes(Parent, Mixed, Added, Error));
    bPassed &= TestEqual(TEXT("transform refusal preserves revision"), Draft->GetRevision(), RevisionBefore);
    bPassed &= TestTrue(TEXT("transform refusal preserves ids"), SameIds(*Draft, IdsBefore));

    TArray<FMHCompositeNodeAdd> Valid = {
        AuthoringNode(EMHCompositeNodeKind::Mesh, TEXT("mesh_a"), TEXT("first"), FTransform(FVector(10.0, 0.0, 0.0))),
        AuthoringNode(EMHCompositeNodeKind::Composite, TEXT("nested_a"), TEXT("second"), FTransform(FVector(20.0, 0.0, 0.0)))};
    bPassed &= TestTrue(TEXT("valid batch: ") + Error, Draft->AddNodes(Parent, Valid, Added, Error));
    bPassed &= TestEqual(TEXT("one batch advances one revision"), Draft->GetRevision(), RevisionBefore + 1);
    if (!TestEqual(TEXT("two ids returned"), Added.Num(), 2)) return false;
    bPassed &= TestTrue(TEXT("both nodes are children"), Draft->GetChildIds(Parent) == Added);
    bPassed &= TestTrue(TEXT("first transform admitted exactly"),
        AuthoringNodeOf(*Draft, Added[0]) != nullptr && AuthoringNodeOf(*Draft, Added[0])->Transform.Equals(Valid[0].LocalTransform, 0.0));
    bPassed &= TestTrue(TEXT("second transform admitted exactly"),
        AuthoringNodeOf(*Draft, Added[1]) != nullptr && AuthoringNodeOf(*Draft, Added[1])->Transform.Equals(Valid[1].LocalTransform, 0.0));

    const uint32 AfterBatch = Draft->GetRevision();
    bPassed &= TestFalse(TEXT("invalid sibling index refuses the whole batch"),
        Draft->AddNodes(Parent, Valid, Added, Error, 3));
    bPassed &= TestFalse(TEXT("negative sibling index except append is refused"),
        Draft->AddNodes(Parent, Valid, Added, Error, -2));
    bPassed &= TestTrue(TEXT("refused insertion returns no ids"), Added.IsEmpty());
    bPassed &= TestFalse(TEXT("single add shares transform admission"),
        Draft->AddNode(Parent, EMHCompositeNodeKind::Group, FString(), TEXT("bad"), NonFinite, Error).IsValid());
    bPassed &= TestFalse(TEXT("random add shares transform admission"),
        Draft->AddRandomNode(Parent, TEXT("bad random"), NonFinite,
            {AuthoringOption(EMHCompositeOptionKind::Empty, FString(), 1.0f)}, Error).IsValid());
    bPassed &= TestEqual(TEXT("invalid single adds preserve revision"), Draft->GetRevision(), AfterBatch);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditAuthoringAnyKindParentTest,
    "Mimir.V5.Composite.EditMode.Authoring.AnyOrdinaryNodeCanOwnChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditAuthoringAnyKindParentTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;

    FMHCompositeDocument ReferencedDocument;
    FMHCompositeNode& ReferencedLeaf = ReferencedDocument.Nodes.AddDefaulted_GetRef();
    ReferencedLeaf.Kind = EMHCompositeNodeKind::Mesh;
    ReferencedLeaf.Resource = F.MeshA;
    UMHCompositeAsset* Referenced = F.Recipe.Composite(F.Recipe.Name(TEXT("ce_authoring_ref")), ReferencedDocument, {});
    if (!TestNotNull(TEXT("referenced composite"), Referenced)) return false;

    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    if (!TestNotNull(TEXT("draft"), Draft) || !TestNotNull(TEXT("projection"), Projection)) return false;

    const FGuid MeshParent = Draft->GetNodeId(0);
    const uint32 RevisionBefore = Draft->GetRevision();
    int32 ChangedCount = 0;
    const FDelegateHandle ChangedHandle = Session->OnChanged.AddLambda([&ChangedCount]() { ++ChangedCount; });
    TArray<FGuid> MeshChildren;
    {
        const FScopedTransaction Transaction(INVTEXT("authoring test: atomic children"));
        const TArray<FMHCompositeNodeAdd> Requests = {
            AuthoringNode(EMHCompositeNodeKind::Mesh, F.MeshA, TEXT("mesh child")),
            AuthoringNode(EMHCompositeNodeKind::Group, FString(), TEXT("group child"))};
        if (!TestTrue(TEXT("batch under mesh: ") + Error, Session->AddNodes(MeshParent, Requests, MeshChildren, Error)))
        {
            Session->OnChanged.Remove(ChangedHandle);
            Subsystem->CancelEditComposite(Error);
            return false;
        }
        if (!TestEqual(TEXT("batch returned both ids"), MeshChildren.Num(), 2))
        {
            Session->OnChanged.Remove(ChangedHandle);
            Subsystem->CancelEditComposite(Error);
            return false;
        }
    }

    bool bPassed = TestEqual(TEXT("one batch sends one authoring change"), ChangedCount, 1);
    bPassed &= TestEqual(TEXT("one batch advances one revision"), Draft->GetRevision(), RevisionBefore + 1);
    bPassed &= TestTrue(TEXT("created ids become selection"), Session->GetSelectedNodeIds() == MeshChildren);
    bPassed &= TestEqual(TEXT("last created id is active"), Session->GetActiveNodeId(), MeshChildren.Last());
    bPassed &= TestTrue(TEXT("mesh accepts both children"), Draft->GetChildIds(MeshParent) == MeshChildren);
    bPassed &= TestNotNull(TEXT("mesh child is projected"), Projection->FindComponentForNodeId(MeshChildren[0]));

    bPassed &= TestTrue(TEXT("one undo removes the whole batch"), GEditor->UndoTransaction());
    bPassed &= TestEqual(TEXT("undo sends one restored change"), ChangedCount, 2);
    bPassed &= TestEqual(TEXT("both ids are absent after undo"), Draft->FindNodeIndex(MeshChildren[0]), static_cast<int32>(INDEX_NONE));
    bPassed &= TestTrue(TEXT("one redo restores the whole batch"), GEditor->RedoTransaction());
    bPassed &= TestEqual(TEXT("redo sends one restored change"), ChangedCount, 3);
    bPassed &= TestTrue(TEXT("redo restores ordered ids"), Draft->GetChildIds(MeshParent) == MeshChildren);

    TArray<FGuid> Inserted;
    {
        const FScopedTransaction Transaction(INVTEXT("authoring test: insert sibling batch"));
        const TArray<FMHCompositeNodeAdd> Requests = {
            AuthoringNode(EMHCompositeNodeKind::Mesh, F.MeshA, TEXT("inserted first")),
            AuthoringNode(EMHCompositeNodeKind::Mesh, F.MeshC, TEXT("inserted second"))};
        bPassed &= TestTrue(TEXT("insert batch between siblings: ") + Error,
            Session->AddNodes(MeshParent, Requests, Inserted, Error, 1));
    }
    if (Inserted.Num() == 2)
    {
        const TArray<FGuid> Expected = {MeshChildren[0], Inserted[0], Inserted[1], MeshChildren[1]};
        bPassed &= TestTrue(TEXT("batch preserves asset order between existing siblings"), Draft->GetChildIds(MeshParent) == Expected);
        bPassed &= TestTrue(TEXT("undo removes ordered insertion as one command"), GEditor->UndoTransaction());
        bPassed &= TestTrue(TEXT("undo restores original sibling order"), Draft->GetChildIds(MeshParent) == MeshChildren);
        bPassed &= TestTrue(TEXT("redo restores ordered insertion as one command"), GEditor->RedoTransaction());
        bPassed &= TestTrue(TEXT("redo restores batch ids and sibling order"), Draft->GetChildIds(MeshParent) == Expected);
    }
    else
    {
        bPassed &= TestEqual(TEXT("ordered insertion creates both nodes"), Inserted.Num(), 2);
    }

    FGuid CompositeParent;
    FGuid CompositeChild;
    FGuid EmptyRandom;
    FGuid RandomChild;
    {
        const FScopedTransaction Transaction(INVTEXT("authoring test: composite parent"));
        CompositeParent = Session->AddNode(FGuid(), EMHCompositeNodeKind::Composite, Referenced->LogicalName,
            TEXT("composite parent"), FTransform(FVector(0.0, 100.0, 0.0)), Error);
        CompositeChild = Session->AddNode(CompositeParent, EMHCompositeNodeKind::Mesh, F.MeshC,
            TEXT("composite child"), FTransform(FVector(0.0, 0.0, 5.0)), Error);
    }
    bPassed &= TestTrue(TEXT("composite node accepts a child: ") + Error, CompositeParent.IsValid() && CompositeChild.IsValid());
    bPassed &= TestEqual(TEXT("composite owns child"), Draft->GetParentId(CompositeChild), CompositeParent);
    bPassed &= TestNotNull(TEXT("child after composite expansion is projected"), Projection->FindComponentForNodeId(CompositeChild));
    {
        const FScopedTransaction Transaction(INVTEXT("authoring test: reparent under composite"));
        bPassed &= TestTrue(TEXT("reparent under a non-group node: ") + Error,
            Session->ReparentNode(MeshChildren[1], CompositeParent, INDEX_NONE, false, Error));
    }
    bPassed &= TestEqual(TEXT("reparent accepts composite parent"), Draft->GetParentId(MeshChildren[1]), CompositeParent);

    {
        const FScopedTransaction Transaction(INVTEXT("authoring test: empty random parent"));
        EmptyRandom = Session->AddRandomNode(FGuid(), TEXT("always empty"), FTransform(FVector(0.0, 200.0, 0.0)),
            {AuthoringOption(EMHCompositeOptionKind::Empty, FString(), 1.0f)}, Error);
        RandomChild = Session->AddNode(EmptyRandom, EMHCompositeNodeKind::Mesh, F.MeshC,
            TEXT("unconditional child"), FTransform(FVector(0.0, 0.0, 7.0)), Error);
    }
    bPassed &= TestTrue(TEXT("random node accepts a child: ") + Error, EmptyRandom.IsValid() && RandomChild.IsValid());
    bPassed &= TestEqual(TEXT("random owns child"), Draft->GetParentId(RandomChild), EmptyRandom);
    bPassed &= TestNotNull(TEXT("empty selection does not suppress random children"), Projection->FindComponentForNodeId(RandomChild));

    TArray<uint8> Bytes;
    FMHCompositeDocument RoundTrip;
    bPassed &= TestTrue(TEXT("writer accepts any-kind children: ") + Error, Draft->CanonicalBytes(Bytes, Error));
    bPassed &= TestTrue(TEXT("parser accepts writer bytes: ") + Error, MHParseCompositeV5(Bytes, RoundTrip, Error));
    TArray<FMHCompositeAssetNode> FlatRoundTrip;
    MHFlattenCompositeDocument(RoundTrip, FlatRoundTrip);
    const int32 CompositeIndex = Draft->FindNodeIndex(CompositeParent);
    const int32 CompositeChildIndex = Draft->FindNodeIndex(CompositeChild);
    const int32 RandomIndex = Draft->FindNodeIndex(EmptyRandom);
    const int32 RandomChildIndex = Draft->FindNodeIndex(RandomChild);
    bPassed &= TestTrue(TEXT("composite child survives canonical roundtrip"),
        FlatRoundTrip.IsValidIndex(CompositeChildIndex) && FlatRoundTrip[CompositeChildIndex].ParentIndex == CompositeIndex);
    bPassed &= TestTrue(TEXT("random child survives canonical roundtrip"),
        FlatRoundTrip.IsValidIndex(RandomIndex) && FlatRoundTrip.IsValidIndex(RandomChildIndex) &&
        FlatRoundTrip[RandomChildIndex].ParentIndex == RandomIndex &&
        FlatRoundTrip[RandomIndex].Kind == EMHCompositeNodeKind::Random && FlatRoundTrip[RandomIndex].Options.Num() == 1 &&
        FlatRoundTrip[RandomIndex].Options[0].Kind == EMHCompositeOptionKind::Empty);

    Session->OnChanged.Remove(ChangedHandle);
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditAuthoringOptionConversionTest,
    "Mimir.V5.Composite.EditMode.Authoring.OptionsConvertContentWithoutLosingNodeIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditAuthoringOptionConversionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    FMHCompositeDocument Source;
    FMHCompositeNode& Mesh = Source.Nodes.AddDefaulted_GetRef();
    Mesh.Kind = EMHCompositeNodeKind::Mesh;
    Mesh.Resource = TEXT("mesh_a");
    Mesh.Name = TEXT("metadata mesh");
    Mesh.Transform.TranslationCm = FVector(11.0, 12.0, 13.0);
    Mesh.Profile = TEXT("profile_a");
    Mesh.PlaceType = 7;
    Mesh.bAppearanceSeedBoundary = true;
    FMHCompositeNode& MeshChild = Mesh.Children.AddDefaulted_GetRef();
    MeshChild.Kind = EMHCompositeNodeKind::Group;
    MeshChild.Name = TEXT("kept child");
    FMHCompositeNode& Group = Source.Nodes.AddDefaulted_GetRef();
    Group.Kind = EMHCompositeNodeKind::Group;
    Group.Name = TEXT("metadata group");
    Group.bHasInlinePlacement = true;
    Group.InlinePlacement.bHasUniformScale = true;
    Group.InlinePlacement.UniformScale.Base = 1.0f;
    Group.InlinePlacement.UniformScale.Deviation = 0.25f;
    Group.PlaceType = 3;
    FMHCompositeNode& Actor = Source.Nodes.AddDefaulted_GetRef();
    Actor.Kind = EMHCompositeNodeKind::Actor;
    Actor.Resource = TEXT("actor_source");
    FMHCompositeNode& Composite = Source.Nodes.AddDefaulted_GetRef();
    Composite.Kind = EMHCompositeNodeKind::Composite;
    Composite.Resource = TEXT("composite_source");
    FMHCompositeNode& GameObj = Source.Nodes.AddDefaulted_GetRef();
    GameObj.Kind = EMHCompositeNodeKind::GameObj;
    GameObj.Resource = TEXT("gameobj_source");

    UMHCompositeEditDocument* Draft = NewObject<UMHCompositeEditDocument>(GetTransientPackage(), NAME_None, RF_Transactional);
    Draft->Load(Source);
    const FGuid MeshId = Draft->GetNodeId(0);
    const FGuid ChildId = Draft->GetNodeId(1);
    const FGuid GroupId = Draft->GetNodeId(2);
    const TArray<FGuid> OtherResourceIds = {Draft->GetNodeId(3), Draft->GetNodeId(4), Draft->GetNodeId(5)};
    const TArray<EMHCompositeOptionKind> OtherResourceKinds = {
        EMHCompositeOptionKind::Actor, EMHCompositeOptionKind::Composite, EMHCompositeOptionKind::GameObj};
    const TArray<FString> OtherResources = {TEXT("actor_source"), TEXT("composite_source"), TEXT("gameobj_source")};
    const FTransform MeshTransform = AuthoringNodeOf(*Draft, MeshId)->Transform;
    FString Error;

    const TArray<FMHCompositeOption> MeshAdditions = {
        AuthoringOption(EMHCompositeOptionKind::Composite, TEXT("nested_a"), 2.0f)};
    bool bPassed = TestTrue(TEXT("resource node converts: ") + Error, Draft->AddNodeOptions(MeshId, MeshAdditions, Error));
    const FMHCompositeAssetNode* ConvertedMesh = AuthoringNodeOf(*Draft, MeshId);
    bPassed &= TestTrue(TEXT("previous resource is option zero at weight one"),
        ConvertedMesh != nullptr && ConvertedMesh->Kind == EMHCompositeNodeKind::Random && ConvertedMesh->Resource.IsEmpty() &&
        ConvertedMesh->Options.Num() == 2 && ConvertedMesh->Options[0].Kind == EMHCompositeOptionKind::Mesh &&
        ConvertedMesh->Options[0].Resource == TEXT("mesh_a") && ConvertedMesh->Options[0].Weight == 1.0f &&
        ConvertedMesh->Options[1].Kind == EMHCompositeOptionKind::Composite);
    bPassed &= TestTrue(TEXT("resource conversion preserves metadata"),
        ConvertedMesh != nullptr && ConvertedMesh->Name == TEXT("metadata mesh") &&
        ConvertedMesh->Transform.Equals(MeshTransform, 0.0) && ConvertedMesh->Profile == TEXT("profile_a") &&
        ConvertedMesh->PlaceType == 7 && ConvertedMesh->bAppearanceSeedBoundary);
    bPassed &= TestEqual(TEXT("resource conversion preserves node id"), Draft->GetNodeId(Draft->FindNodeIndex(MeshId)), MeshId);
    bPassed &= TestEqual(TEXT("resource conversion preserves child id"), Draft->GetNodeId(Draft->FindNodeIndex(ChildId)), ChildId);
    bPassed &= TestEqual(TEXT("resource conversion preserves child parent"), Draft->GetParentId(ChildId), MeshId);
    const TArray<FMHCompositeOption> Appended = {
        AuthoringOption(EMHCompositeOptionKind::Actor, TEXT("actor_a"), 4.0f)};
    bPassed &= TestTrue(TEXT("existing random appends: ") + Error, Draft->AddNodeOptions(MeshId, Appended, Error));
    ConvertedMesh = AuthoringNodeOf(*Draft, MeshId);
    bPassed &= TestTrue(TEXT("append keeps prior option order"),
        ConvertedMesh != nullptr && ConvertedMesh->Options.Num() == 3 &&
        ConvertedMesh->Options[0].Kind == EMHCompositeOptionKind::Mesh &&
        ConvertedMesh->Options[1].Kind == EMHCompositeOptionKind::Composite &&
        ConvertedMesh->Options[2].Kind == EMHCompositeOptionKind::Actor);

    const TArray<FMHCompositeOption> EmptyAddition = {
        AuthoringOption(EMHCompositeOptionKind::Empty, FString(), 1.0f)};
    for (int32 ResourceIndex = 0; ResourceIndex < OtherResourceIds.Num(); ++ResourceIndex)
    {
        bPassed &= TestTrue(TEXT("resource kind converts: ") + Error,
            Draft->AddNodeOptions(OtherResourceIds[ResourceIndex], EmptyAddition, Error));
        const FMHCompositeAssetNode* Converted = AuthoringNodeOf(*Draft, OtherResourceIds[ResourceIndex]);
        bPassed &= TestTrue(TEXT("resource kind is preserved as option zero"),
            Converted != nullptr && Converted->Kind == EMHCompositeNodeKind::Random && Converted->Options.Num() == 2 &&
            Converted->Options[0].Kind == OtherResourceKinds[ResourceIndex] &&
            Converted->Options[0].Resource == OtherResources[ResourceIndex] && Converted->Options[0].Weight == 1.0f);
    }

    const TArray<FMHCompositeOption> GroupAdditions = {
        AuthoringOption(EMHCompositeOptionKind::Empty, FString(), 0.0f),
        AuthoringOption(EMHCompositeOptionKind::GameObj, TEXT("spawn_a"), 3.0f)};
    bPassed &= TestTrue(TEXT("group converts: ") + Error, Draft->AddNodeOptions(GroupId, GroupAdditions, Error));
    const FMHCompositeAssetNode* ConvertedGroup = AuthoringNodeOf(*Draft, GroupId);
    bPassed &= TestTrue(TEXT("group conversion uses exactly the additions"),
        ConvertedGroup != nullptr && ConvertedGroup->Kind == EMHCompositeNodeKind::Random &&
        ConvertedGroup->Options.Num() == 2 && ConvertedGroup->Options[0].Kind == EMHCompositeOptionKind::Empty &&
        ConvertedGroup->Options[1].Kind == EMHCompositeOptionKind::GameObj);
    bPassed &= TestTrue(TEXT("group conversion preserves inline metadata"),
        ConvertedGroup != nullptr && ConvertedGroup->Name == TEXT("metadata group") &&
        ConvertedGroup->bHasInlinePlacement && ConvertedGroup->InlinePlacement.bHasUniformScale &&
        ConvertedGroup->InlinePlacement.UniformScale.Base == 1.0f && ConvertedGroup->InlinePlacement.UniformScale.Deviation == 0.25f &&
        ConvertedGroup->PlaceType == 3);

    const uint32 BeforeInvalid = Draft->GetRevision();
    bPassed &= TestFalse(TEXT("negative weight refused"), Draft->SetNodeOptionWeight(GroupId, 1, -1.0f, Error));
    bPassed &= TestFalse(TEXT("NaN weight refused"), Draft->SetNodeOptionWeight(GroupId, 1, std::numeric_limits<float>::quiet_NaN(), Error));
    bPassed &= TestFalse(TEXT("removing the only positive option is refused"), Draft->RemoveNodeOption(GroupId, 1, Error));
    bPassed &= TestEqual(TEXT("invalid weight combinations are atomic"), Draft->GetRevision(), BeforeInvalid);
    ConvertedGroup = AuthoringNodeOf(*Draft, GroupId);
    bPassed &= TestTrue(TEXT("invalid combinations preserve options"),
        ConvertedGroup != nullptr && ConvertedGroup->Options.Num() == 2 && ConvertedGroup->Options[1].Weight == 3.0f);

    bPassed &= TestTrue(TEXT("zero is valid while another option stays positive"), Draft->SetNodeOptionWeight(GroupId, 0, 1.0f, Error));
    bPassed &= TestTrue(TEXT("remove one option: ") + Error, Draft->RemoveNodeOption(GroupId, 1, Error));
    ConvertedGroup = AuthoringNodeOf(*Draft, GroupId);
    bPassed &= TestTrue(TEXT("one remaining option stays random"),
        ConvertedGroup != nullptr && ConvertedGroup->Kind == EMHCompositeNodeKind::Random && ConvertedGroup->Options.Num() == 1);
    bPassed &= TestTrue(TEXT("remove final option: ") + Error, Draft->RemoveNodeOption(GroupId, 0, Error));
    ConvertedGroup = AuthoringNodeOf(*Draft, GroupId);
    bPassed &= TestTrue(TEXT("final removal becomes empty group and preserves identity/metadata"),
        ConvertedGroup != nullptr && ConvertedGroup->Kind == EMHCompositeNodeKind::Group && ConvertedGroup->Resource.IsEmpty() &&
        ConvertedGroup->Options.IsEmpty() && Draft->GetNodeId(Draft->FindNodeIndex(GroupId)) == GroupId &&
        ConvertedGroup->bHasInlinePlacement && ConvertedGroup->PlaceType == 3);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditAuthoringCycleTest,
    "Mimir.V5.Composite.EditMode.Authoring.CyclicReferencesKeepRecoverableDraft",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditAuthoringCycleTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Invocation = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("invocation"), Invocation)) return false;
    FString Error;
    if (!TestTrue(TEXT("begin nested edit"), Subsystem->BeginEditNestedComposite(F.A, Invocation->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditDocument* Draft = Session->GetDraft();
    UMHCompositeEditProjection* Projection = Session->GetProjection();
    const FGuid OriginalNode = Draft->GetNodeId(0);
    TArray<uint8> Before;
    if (!TestTrue(TEXT("original bytes"), Draft->CanonicalBytes(Before, Error))) return false;
    const FMHResolvedCompositePlan* OriginalPlan = Projection->GetPlan();
    FGuid Added;
    {
        const FScopedTransaction Transaction(INVTEXT("authoring test: indirect cycle"));
        // Child -> Root -> Child is valid node syntax but an invalid source graph.
        Added = Session->AddNode(FGuid(), EMHCompositeNodeKind::Composite, F.Root->LogicalName,
            TEXT("cyclic reference"), FTransform::Identity, Error);
    }
    bool bPassed = TestTrue(TEXT("invalid graph stays as an undoable draft"), Added.IsValid() && Session->IsDirty());
    bPassed &= TestTrue(TEXT("indirect cycle reports a preview error"), Session->GetPreviewError().Contains(TEXT("cycle"), ESearchCase::IgnoreCase));
    bPassed &= TestTrue(TEXT("invalid graph preserves last valid preview"), Projection->GetPlan() == OriginalPlan && Projection->IsOpen());
    int32 PublishCalls = 0;
    Subsystem->SetCommitPublisherForTests([&PublishCalls](UMHCompositeAsset&, FString&) { ++PublishCalls; return true; });
    TArray<FString> Warnings;
    const bool bPublished = Subsystem->CommitEditComposite(Warnings, Error);
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestFalse(TEXT("cyclic draft is refused before Save"), bPublished);
    bPassed &= TestEqual(TEXT("cyclic draft never reaches publisher"), PublishCalls, 0);
    if (bPublished) return false;
    bPassed &= TestTrue(TEXT("refused Save keeps session and draft open"), Session->IsOpen() && Session->GetDraft() == Draft && Session->IsDirty());
    bPassed &= TestTrue(TEXT("undo cyclic reference"), GEditor->UndoTransaction());
    TArray<uint8> AfterUndo;
    bPassed &= TestTrue(TEXT("undo restores original bytes and preview"), Draft->CanonicalBytes(AfterUndo, Error) && AfterUndo == Before && Session->GetPreviewError().IsEmpty());

    {
        const FScopedTransaction Transaction(INVTEXT("authoring test: cyclic random option"));
        const TArray<FMHCompositeOption> Options = {
            AuthoringOption(EMHCompositeOptionKind::Composite, F.Child->LogicalName, 0.0f)};
        bPassed &= TestTrue(TEXT("add self-reference as an unselected content variant"), Session->AddNodeOptions(OriginalNode, Options, Error));
    }
    bPassed &= TestTrue(TEXT("unselected cyclic variant is rejected before traversal"), Session->GetPreviewError().Contains(TEXT("cycle"), ESearchCase::IgnoreCase));
    bPassed &= TestTrue(TEXT("undo cyclic variant"), GEditor->UndoTransaction());
    bPassed &= TestTrue(TEXT("variant undo restores original bytes and preview"), Draft->CanonicalBytes(AfterUndo, Error) && AfterUndo == Before && Session->GetPreviewError().IsEmpty());
    bPassed &= TestTrue(TEXT("cancel remains available"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
