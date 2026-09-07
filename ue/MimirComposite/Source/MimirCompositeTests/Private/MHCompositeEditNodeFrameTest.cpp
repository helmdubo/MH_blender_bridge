#include "MHCompositeEditFixture.h"

#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"

namespace UE::MimirComposite::Tests
{
namespace
{

const FMHResolvedCompositeNode* PlanNode(const UMHCompositeEditProjection& Projection, const FString& Path)
{
    const FMHResolvedCompositePlan* Plan = Projection.GetPlan();
    return Plan != nullptr ? Plan->Nodes.FindByPredicate(
        [&Path](const FMHResolvedCompositeNode& Node) { return Node.NodePath == Path; }) : nullptr;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditNodeFrameBindingTest,
    "Mimir.V5.Composite.EditMode.NodeFrame.BindsNestedAndRandomVisualsToStableAuthoredFrames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditNodeFrameBindingTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    // An empty inline placement body is a legal procedural transform source;
    // it samples identity but must still disable authored transform gestures.
    F.Root->Nodes[0].bHasInlinePlacement = true;
    FString Error;
    if (!TestTrue(TEXT("root session: ") + Error, Subsystem->BeginEditComposite(F.A, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("session"), Session) || !TestNotNull(TEXT("projection"), Projection) ||
        !TestNotNull(TEXT("draft"), Draft)) return false;

    const FGuid DeletedId = Draft->GetNodeId(0);
    const FGuid ReferenceId = Draft->GetNodeId(1);
    const FString ReferencePath = F.Root->LogicalName + TEXT(":nodes[1]");
    USceneComponent* ReferenceHandle = Projection->FindComponentForOrigin(ReferencePath);
    USceneComponent* PlainLeaf = Projection->FindComponentForOrigin(
        ReferencePath + TEXT(">") + F.Child->LogicalName + TEXT(":nodes[0]"));
    USceneComponent* GroupedLeaf = Projection->FindComponentForOrigin(
        ReferencePath + TEXT(">") + F.Child->LogicalName + TEXT(":nodes[1]/children[0]"));
    if (!TestNotNull(TEXT("reference handle"), ReferenceHandle) || !TestNotNull(TEXT("nested plain leaf"), PlainLeaf) ||
        !TestNotNull(TEXT("nested grouped leaf"), GroupedLeaf)) return false;

    bool bPassed = TestEqual(TEXT("both nested leaves bind to the reference"), Projection->GetNodeIdForComponent(PlainLeaf), ReferenceId);
    bPassed &= TestEqual(TEXT("the other nested leaf has the same binding"), Projection->GetNodeIdForComponent(GroupedLeaf), ReferenceId);
    bPassed &= TestTrue(TEXT("reference lookup returns its exact handle"), Projection->FindComponentForNodeId(ReferenceId) == ReferenceHandle);

    FMHCompositeEditNodeFrame ReferenceFrame;
    const FMHResolvedCompositeNode* ReferencePlanNode = PlanNode(*Projection, ReferencePath);
    if (!TestTrue(TEXT("reference frame exists"), Projection->GetNodeFrame(ReferenceId, ReferenceFrame)) ||
        !TestNotNull(TEXT("exact reference plan node"), ReferencePlanNode)) return false;
    const FMatrix PlacementBasis = F.A->GetActorTransform().ToMatrixWithScale();
    bPassed &= TestTrue(TEXT("reference world comes from its exact plan node"),
        ReferenceFrame.WorldMatrix.Equals(ReferencePlanNode->WorldMatrix * PlacementBasis, 1.e-3));
    bPassed &= TestTrue(TEXT("root reference parent is the placement basis"),
        ReferenceFrame.ParentWorldMatrix.Equals(PlacementBasis, 1.e-3));
    bPassed &= TestTrue(TEXT("the exact pivot is not a nested leaf pivot"),
        !ReferenceHandle->GetComponentLocation().Equals(PlainLeaf->GetComponentLocation(), 1.e-3));
    bPassed &= TestTrue(TEXT("the handle is on the node frame"),
        ReferenceHandle->GetComponentTransform().ToMatrixWithScale().Equals(ReferenceFrame.WorldMatrix, 1.e-3));
    FMHCompositeEditNodeFrame GeneratedFrame;
    bPassed &= TestTrue(TEXT("inline placement marks only its node frame generated"),
        Projection->GetNodeFrame(DeletedId, GeneratedFrame) && GeneratedFrame.bGeneratedTransform && !ReferenceFrame.bGeneratedTransform);

    FBox ReferenceBounds(ForceInit);
    bPassed &= TestTrue(TEXT("reference bounds include its visual primitives"), Projection->GetNodeBounds(ReferenceId, ReferenceBounds));
    bPassed &= TestTrue(TEXT("bounds contain plain nested geometry"), ReferenceBounds.IsInsideOrOn(PlainLeaf->GetComponentLocation()));
    bPassed &= TestTrue(TEXT("bounds contain grouped nested geometry"), ReferenceBounds.IsInsideOrOn(GroupedLeaf->GetComponentLocation()));

    Projection->UpdateSelection({ReferenceId});
    const UMHCompositeEditMeshComponent* SelectedLeaf = Cast<UMHCompositeEditMeshComponent>(PlainLeaf);
    const UMHCompositeEditMeshComponent* UnselectedLeaf = Cast<UMHCompositeEditMeshComponent>(
        Projection->FindComponentForOrigin(F.Root->LogicalName + TEXT(":nodes[0]")));
    bPassed &= TestTrue(TEXT("logical selection drives the selected leaf render flag"),
        SelectedLeaf != nullptr && SelectedLeaf->ShouldRenderSelected() && SelectedLeaf->IsComponentIndividuallySelected());
    bPassed &= TestTrue(TEXT("the selected projection actor does not outline an unselected sibling"),
        UnselectedLeaf != nullptr && !UnselectedLeaf->ShouldRenderSelected() && !UnselectedLeaf->IsComponentIndividuallySelected());

    const FGuid GroupId = Session->AddNode(
        FGuid(), EMHCompositeNodeKind::Group, FString(), TEXT("bounds_group"), FTransform(FVector(0.0, 100.0, 0.0)), Error);
    const FGuid GroupChildId = GroupId.IsValid() ? Session->AddNode(
        GroupId, EMHCompositeNodeKind::Mesh, F.MeshC, TEXT("bounds_child"), FTransform(FVector(0.0, 0.0, 25.0)), Error) : FGuid();
    bPassed &= TestTrue(TEXT("add group and child: ") + Error, GroupId.IsValid() && GroupChildId.IsValid());
    const TArray<USceneComponent*> GroupOnly = Projection->GetComponentsForNodeId(GroupId);
    const TArray<USceneComponent*> GroupWithDescendants = Projection->GetComponentsForNodeId(GroupId, true);
    USceneComponent* GroupChild = Projection->FindComponentForNodeId(GroupChildId);
    bPassed &= TestTrue(TEXT("group exact visuals exclude its child"),
        GroupOnly.Contains(Projection->FindComponentForNodeId(GroupId)) && !GroupOnly.Contains(GroupChild));
    bPassed &= TestTrue(TEXT("group descendant visuals include its child"), GroupWithDescendants.Contains(GroupChild));
    Projection->UpdateSelection({GroupId});
    const UMHCompositeEditMeshComponent* GroupChildMesh = Cast<UMHCompositeEditMeshComponent>(GroupChild);
    bPassed &= TestTrue(TEXT("group highlighting reaches descendant geometry"),
        GroupChildMesh != nullptr && GroupChildMesh->ShouldRenderSelected());

    FMHCompositeOption OnlyMesh;
    OnlyMesh.Kind = EMHCompositeOptionKind::Mesh;
    OnlyMesh.Resource = F.MeshA;
    OnlyMesh.Weight = 1.0f;
    const FGuid RandomId = Session->AddRandomNode(FGuid(), TEXT("one_pick"), FTransform::Identity, {OnlyMesh}, Error);
    bPassed &= TestTrue(TEXT("add deterministic random node: ") + Error, RandomId.IsValid());
    const FString RandomPath = F.Root->LogicalName + TEXT(":") + Draft->GetSelector(Draft->FindNodeIndex(RandomId));
    USceneComponent* RandomHandle = Projection->FindComponentForOrigin(RandomPath);
    USceneComponent* RandomLeaf = Projection->FindComponentForOrigin(RandomPath + TEXT("/options[0]"));
    bPassed &= TestTrue(TEXT("random handle and selected visual exist"), RandomHandle != nullptr && RandomLeaf != nullptr);
    bPassed &= TestEqual(TEXT("random handle binds to owner"), Projection->GetNodeIdForComponent(RandomHandle), RandomId);
    bPassed &= TestEqual(TEXT("selected random option binds to owner"), Projection->GetNodeIdForComponent(RandomLeaf), RandomId);
    FMHCompositeEditNodeFrame RandomFrame;
    bPassed &= TestTrue(TEXT("a random choice alone is not a generated transform"),
        Projection->GetNodeFrame(RandomId, RandomFrame) && !RandomFrame.bGeneratedTransform);

    // Delete the sibling before the selected reference. Its selector changes,
    // while its session id must resolve to the new exact path and frame.
    bPassed &= TestTrue(TEXT("delete preceding sibling: ") + Error, Session->DeleteNode(DeletedId, Error));
    const FString MovedReferencePath = F.Root->LogicalName + TEXT(":nodes[0]");
    FMHCompositeEditNodeFrame MovedFrame;
    const FMHResolvedCompositeNode* MovedPlanNode = PlanNode(*Projection, MovedReferencePath);
    bPassed &= TestTrue(TEXT("reference id still has a frame after selector shift"), Projection->GetNodeFrame(ReferenceId, MovedFrame));
    bPassed &= TestTrue(TEXT("shifted frame is the exact current plan node"),
        MovedPlanNode != nullptr && MovedFrame.WorldMatrix.Equals(MovedPlanNode->WorldMatrix * PlacementBasis, 1.e-3));
    bPassed &= TestTrue(TEXT("lookup returns the shifted reference handle"),
        Projection->FindComponentForNodeId(ReferenceId) == Projection->FindComponentForOrigin(MovedReferencePath));
    FMHCompositeEditNodeFrame MissingFrame;
    bPassed &= TestFalse(TEXT("deleted id has no arbitrary reused frame"), Projection->GetNodeFrame(DeletedId, MissingFrame));
    bPassed &= TestNull(TEXT("deleted id has no arbitrary reused component"), Projection->FindComponentForNodeId(DeletedId));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditNodeFrameScaledOccurrenceTest,
    "Mimir.V5.Composite.EditMode.NodeFrame.PreservesScaledNestedOccurrenceFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditNodeFrameScaledOccurrenceTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;

    FMHCompositeDocument ScaledDocument;
    FMHCompositeNode& InvocationNode = ScaledDocument.Nodes.AddDefaulted_GetRef();
    InvocationNode.Kind = EMHCompositeNodeKind::Composite;
    InvocationNode.Resource = F.Child->LogicalName;
    InvocationNode.Name = TEXT("scaled_child");
    InvocationNode.Transform.TranslationCm = FVector(250.0, 30.0, 10.0);
    InvocationNode.Transform.RotationQuat = FQuat(FRotator(0.0, 30.0, 0.0));
    InvocationNode.Transform.Scale = FVector(2.0);
    UMHCompositeAsset* ScaledRoot = F.Recipe.Composite(F.Recipe.Name(TEXT("ce_scaled_root")), ScaledDocument, {});
    AMHCompositeActor* ScaledActor = ScaledRoot != nullptr
        ? F.Spawn(FTransform(FRotator(0.0, 15.0, 0.0), FVector(700.0, -200.0, 50.0)), ScaledRoot)
        : nullptr;
    if (!TestNotNull(TEXT("scaled root"), ScaledRoot) || !TestNotNull(TEXT("scaled actor"), ScaledActor)) return false;
    const FMHResolvedCompositeNode* Invocation = FCompositeEditFixture::Invocation(*ScaledActor, 0);
    if (!TestNotNull(TEXT("scaled invocation"), Invocation)) return false;
    const FString InvocationPath = Invocation->NodePath;
    const FMatrix ExpectedOccurrence = Invocation->WorldMatrix * ScaledActor->GetActorTransform().ToMatrixWithScale();

    FString Error;
    if (!TestTrue(TEXT("nested session: ") + Error, Subsystem->BeginEditNestedComposite(ScaledActor, InvocationPath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("projection"), Projection) || !TestNotNull(TEXT("draft"), Draft) ||
        !TestNotNull(TEXT("projection actor"), Projection != nullptr ? Projection->GetProjectionActor() : nullptr)) return false;

    FMHCompositeEditNodeFrame Frame;
    const FGuid PlainId = Draft->GetNodeId(0);
    bool bPassed = TestTrue(TEXT("plain frame exists"), Projection->GetNodeFrame(PlainId, Frame));
    bPassed &= TestTrue(TEXT("projection actor keeps the complete occurrence transform including scale"),
        Projection->GetProjectionActor()->GetActorTransform().ToMatrixWithScale().Equals(ExpectedOccurrence, 1.e-3));
    bPassed &= TestTrue(TEXT("top-level node parent is that same scaled occurrence frame"),
        Frame.ParentWorldMatrix.Equals(ExpectedOccurrence, 1.e-3));
    bPassed &= TestTrue(TEXT("occurrence scale survives"),
        Projection->GetProjectionActor()->GetActorScale3D().Equals(FVector(2.0), 1.e-3));
    FTransform LegacyParent;
    USceneComponent* Plain = Projection->FindComponentForNodeId(PlainId);
    bPassed &= TestTrue(TEXT("legacy parent query routes through the node frame"),
        Plain != nullptr && Projection->GetParentWorldForComponent(Plain, LegacyParent) &&
        LegacyParent.ToMatrixWithScale().Equals(Frame.ParentWorldMatrix, 1.e-3));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
