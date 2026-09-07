#include "MHCompositeEditFixture.h"

#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor/Transactor.h"
#include "Selection.h"
#include "UI/MHEditSessionKeys.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "PrimitiveSceneProxy.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHNestedVisualGestureTest,
    "Mimir.V5.Composite.EditMode.Interaction.NestedVisualMovesItsAuthoringReference",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHNestedVisualGestureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FMHCompositeEditBackendScope Backend(true);
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
    FString Error;
    if (!TestTrue(TEXT("open root: ") + Error, Subsystem->BeginEditComposite(F.A, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session->GetProjection();
    UMHCompositeEditDocument* Draft = Session->GetDraft();
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    if (!TestNotNull(TEXT("mode"), Mode)) return false;
    const FGuid ReferenceId = Draft->GetNodeId(2);
    const FTransform Before = Draft->GetNodes()[2].Transform;
    const FString Prefix = F.Root->LogicalName + TEXT(":nodes[2]>");
    TMap<FString, FVector> BeforeVisuals;
    USceneComponent* Hit = nullptr;
    for (USceneComponent* Component : Projection->GetComponents())
    {
        const FString Origin = Projection->GetOriginForComponent(Component);
        if (Cast<UStaticMeshComponent>(Component) && Origin.StartsWith(Prefix))
        {
            BeforeVisuals.Add(Origin, Component->GetComponentLocation());
            if (Origin.EndsWith(TEXT(":nodes[0]"))) Hit = Component;
        }
    }
    if (!TestNotNull(TEXT("nested visual hit"), Hit) || !TestTrue(TEXT("multiple reference visuals"), BeforeVisuals.Num() >= 2)) return false;
    TestEqual(TEXT("visual belongs to reference"), Projection->GetNodeIdForComponent(Hit), ReferenceId);
    if (!TestTrue(TEXT("select visual"), Mode->SelectComponent(Hit))) return false;
    FMHCompositeEditNodeFrame Frame;
    TestTrue(TEXT("reference frame exists"), Projection->GetNodeFrame(ReferenceId, Frame));
    TestTrue(TEXT("widget uses authoring reference pivot"), Mode->GetWidgetLocation().Equals(Frame.WorldMatrix.GetOrigin()));
    for (USceneComponent* Component : Projection->GetComponents())
    {
        if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
        {
            const bool bInNode = Projection->GetNodeIdForComponent(Component) == ReferenceId;
            TestEqual(TEXT("only chosen reference visuals highlighted: ") + Projection->GetOriginForComponent(Component), Mesh->ShouldRenderSelected(), bInNode);
            TestEqual(TEXT("native component selection mirrors logical node"), GEditor->GetSelectedComponents()->IsSelected(Component), bInNode);
        }
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("MHPreviewRenderSmoke")))
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!TestNotNull(TEXT("renderable cube"), Cube)) return false;
        for (USceneComponent* Component : Projection->GetComponents())
        {
            if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component)) Mesh->SetStaticMesh(Cube);
        }
        F.World->SendAllEndOfFrameUpdates();
        Projection->UpdateSelection(Session->GetSelectedNodeIds());
        Projection->PushEditingTint();
        FlushRenderingCommands();
        for (USceneComponent* Component : Projection->GetComponents())
        {
            if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
            {
                const FPrimitiveSceneProxy* Proxy = Mesh->GetSceneProxy();
                if (!TestNotNull(TEXT("mesh render proxy"), Proxy)) continue;
                const bool bSelected = Projection->GetNodeIdForComponent(Component) == ReferenceId;
                TestEqual(TEXT("render selection excludes sibling nodes"), Proxy->IsSelected(), bSelected);
                TestEqual(TEXT("render active selection color covers whole node"), Proxy->IsIndividuallySelected(), bSelected);
            }
        }
    }
    FVector Drag(10.0, 0.0, 0.0), Scale = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    TestTrue(TEXT("begin gesture"), Mode->StartTracking(nullptr, nullptr));
    TestTrue(TEXT("drag"), Mode->InputDelta(nullptr, nullptr, Drag, Rotation, Scale));
    TestTrue(TEXT("end gesture"), Mode->EndTracking(nullptr, nullptr));
    TestTrue(TEXT("reference local moved by the world delta, not replaced by leaf local"),
        Draft->GetNodes()[Draft->FindNodeIndex(ReferenceId)].Transform.GetLocation().Equals(Before.GetLocation() + Drag, 1e-3));
    for (const TPair<FString, FVector>& Visual : BeforeVisuals)
    {
        const USceneComponent* Component = Projection->FindComponentForOrigin(Visual.Key);
        TestTrue(TEXT("every reference visual follows: ") + Visual.Key,
            Component && Component->GetComponentLocation().Equals(Visual.Value + Drag, 1e-3));
    }
    TestTrue(TEXT("undo"), GEditor->UndoTransaction());
    TestTrue(TEXT("undo restores authoring reference"), Draft->GetNodes()[Draft->FindNodeIndex(ReferenceId)].Transform.Equals(Before));
    TestTrue(TEXT("undo keeps the session"), Subsystem->GetEditSession() == Session && Session->IsOpen());
    FVector TinyDrag(10.0, 0.0, 0.0);
    TestTrue(TEXT("begin sub-tolerance gesture"), Mode->StartTracking(nullptr, nullptr));
    TestTrue(TEXT("small accepted drag"), Mode->InputDelta(nullptr, nullptr, TinyDrag, Rotation, Scale));
    TinyDrag.X = -9.99995;
    TestTrue(TEXT("return near gesture origin"), Mode->InputDelta(nullptr, nullptr, TinyDrag, Rotation, Scale));
    TestTrue(TEXT("finish small gesture"), Mode->EndTracking(nullptr, nullptr));
    TestFalse(TEXT("small drag changes authored transform"), Draft->GetNodes()[Draft->FindNodeIndex(ReferenceId)].Transform.Equals(Before, 0.0));
    TestTrue(TEXT("every accepted edit retains an Undo step"), GEditor->Trans->CanUndo());
    TestTrue(TEXT("undo small edit"), GEditor->UndoTransaction());
    TestTrue(TEXT("small edit Undo restores exact local"), Draft->GetNodes()[Draft->FindNodeIndex(ReferenceId)].Transform.Equals(Before, 0.0));
    FMHCompositeDocument ReplacementDocument;
    FMHCompositeNode& ReplacementLeaf = ReplacementDocument.Nodes.AddDefaulted_GetRef();
    ReplacementLeaf.Kind = EMHCompositeNodeKind::Mesh;
    ReplacementLeaf.Resource = F.MeshA;
    UMHCompositeAsset* Replacement = F.Recipe.Composite(F.Recipe.Name(TEXT("ce_selection_replacement")), ReplacementDocument, {});
    if (!TestNotNull(TEXT("replacement definition"), Replacement)) return false;
    const TArray<USceneComponent*> RetiredVisuals = Projection->GetComponentsForNodeId(ReferenceId);
    {
        const FScopedTransaction Transaction(INVTEXT("Replace selected reference"));
        TestTrue(TEXT("replace selected reference resource"), Session->SetNodeResource(ReferenceId, Replacement->LogicalName, Error));
    }
    TestEqual(TEXT("replacement keeps logical selection"), Session->GetActiveNodeId(), ReferenceId);
    for (USceneComponent* Component : RetiredVisuals)
    {
        if (!IsValid(Component)) TestFalse(TEXT("retired visual released from native selection"), GEditor->GetSelectedComponents()->IsSelected(Component));
    }
    for (USceneComponent* Component : Projection->GetComponentsForNodeId(ReferenceId))
    {
        TestTrue(TEXT("replacement visuals selected"), GEditor->GetSelectedComponents()->IsSelected(Component));
    }
    TestTrue(TEXT("undo replacement"), GEditor->UndoTransaction());
    TestEqual(TEXT("Undo restores reference visuals"), Projection->GetComponentsForNodeId(ReferenceId).Num(), RetiredVisuals.Num());
    TArray<USceneComponent*> SelectedAfterUndo;
    GEditor->GetSelectedComponents()->GetSelectedObjects(SelectedAfterUndo);
    TestTrue(TEXT("Undo keeps projection actor selected"), Projection->GetProjectionActor()->IsSelected());
    TestEqual(TEXT("Undo keeps projection actor exclusive"), GEditor->GetSelectedActorCount(), 1);
    TestEqual(TEXT("Undo selects current visuals only"), SelectedAfterUndo.Num(), Projection->GetComponentsForNodeId(ReferenceId).Num());
    for (USceneComponent* Component : SelectedAfterUndo)
    {
        TestTrue(TEXT("Undo selection has a live authoring binding"), IsValid(Component) && Projection->GetNodeIdForComponent(Component) == ReferenceId);
        TestTrue(TEXT("Undo selection belongs to current projection"), IsValid(Component) && Component->GetOwner() == Projection->GetProjectionActor() && Projection->GetComponents().Contains(Component));
    }
    TestTrue(TEXT("close while reference is selected"), Subsystem->CancelEditComposite(Error));
    TestEqual(TEXT("teardown cannot reselect dying projection components"), GEditor->GetSelectedComponentCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHMultiNodeGestureTest,
    "Mimir.V5.Composite.EditMode.Interaction.MultiSelectionMovesDescendantsOnceAndEscCancelsSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMultiNodeGestureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FMHCompositeEditBackendScope Backend(true);
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("invocation"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("open nested"), Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session->GetProjection();
    UMHCompositeEditDocument* Draft = Session->GetDraft();
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    if (!TestNotNull(TEXT("mode"), Mode)) return false;
    const FGuid Plain = Draft->GetNodeId(0), Group = Draft->GetNodeId(1), Child = Draft->GetNodeId(2);
    Mode->SelectNodeIds({Plain, Group, Child}, Group);
    TestEqual(TEXT("all logical targets retained"), Session->GetSelectedNodeIds().Num(), 3);
    const FTransform ChildBefore = Draft->GetNodes()[2].Transform;
    FMHCompositeEditNodeFrame BeforeFrame;
    if (!TestTrue(TEXT("child frame"), Projection->GetNodeFrame(Child, BeforeFrame))) return false;
    TArray<uint8> Before;
    if (!TestTrue(TEXT("canonical before"), Draft->CanonicalBytes(Before, Error))) return false;
    TArray<uint8> PublishedBefore;
    if (!TestTrue(TEXT("published bytes before"), FCompositeEditFixture::AssetBytes(*F.Child, PublishedBefore))) return false;
    FVector Drag(10.0, 0.0, 0.0), Scale = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    TestTrue(TEXT("start group gesture"), Mode->StartTracking(nullptr, nullptr));
    TestTrue(TEXT("delta"), Mode->InputDelta(nullptr, nullptr, Drag, Rotation, Scale));
    TestTrue(TEXT("finish"), Mode->EndTracking(nullptr, nullptr));
    FMHCompositeEditNodeFrame AfterFrame;
    TestTrue(TEXT("child frame after"), Projection->GetNodeFrame(Child, AfterFrame));
    TestTrue(TEXT("selected descendant moves once with selected parent"), AfterFrame.WorldMatrix.GetOrigin().Equals(BeforeFrame.WorldMatrix.GetOrigin() + Drag));
    TestTrue(TEXT("selected descendant local unchanged"), Draft->GetNodes()[2].Transform.Equals(ChildBefore));
    TestTrue(TEXT("one gesture undoes"), GEditor->UndoTransaction());
    TArray<uint8> Undone;
    TestTrue(TEXT("one Undo restores entire batch"), Draft->CanonicalBytes(Undone, Error) && Undone == Before);
    TestFalse(TEXT("no second authored undo step"), GEditor->Trans->CanUndo());
    TestEqual(TEXT("Undo retains logical selection"), Session->GetSelectedNodeIds().Num(), 3);

    TestTrue(TEXT("start cancellable gesture"), Mode->StartTracking(nullptr, nullptr));
    TestTrue(TEXT("cancellable delta"), Mode->InputDelta(nullptr, nullptr, Drag, Rotation, Scale));
    TestTrue(TEXT("Esc cancels session during a selected-node gesture"), MHHandleEditSessionKey(EKeys::Escape, false));
    TArray<uint8> PublishedAfter;
    TestTrue(TEXT("Esc leaves published definition unchanged"), FCompositeEditFixture::AssetBytes(*F.Child, PublishedAfter) && PublishedAfter == PublishedBefore);
    TestFalse(TEXT("cancelled gesture leaves no undo"), GEditor->Trans->CanUndo());
    TestFalse(TEXT("session closed"), Subsystem->IsEditingComposite());
    TestFalse(TEXT("mode left immediately"), UMHCompositeEditorMode::IsActive());
    TestEqual(TEXT("no transient component remains selected"), GEditor->GetSelectedComponentCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHEscapeInputDispatchTest,
    "Mimir.V5.Composite.EditMode.Interaction.ViewportEscapeExitsAfterInputDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEscapeInputDispatchTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const TSharedRef<FMHCompositeEditBackendScope> Backend = MakeShared<FMHCompositeEditBackendScope>(true);
    const TSharedRef<FCompositeEditFixture> Fixture = MakeShared<FCompositeEditFixture>(*this);
    if (!Fixture->Build(*this)) return false;
    UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
    const FMHResolvedCompositeNode* Invocation = FCompositeEditFixture::Invocation(*Fixture->A, 1);
    if (!TestNotNull(TEXT("invocation"), Invocation)) return false;
    FString Error;
    if (!TestTrue(TEXT("open nested"), Subsystem->BeginEditNestedComposite(Fixture->A, Invocation->NodePath, Error))) return false;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("mode"), Mode) || !TestNotNull(TEXT("session"), Session)) return false;
    Mode->SelectNodeIds({Session->GetDraft()->GetNodeId(0)});
    TArray<uint8> Before;
    if (!TestTrue(TEXT("published bytes"), FCompositeEditFixture::AssetBytes(*Fixture->Child, Before))) return false;
    FVector Drag(25.0, 0.0, 0.0), Scale = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    TestTrue(TEXT("start gesture"), Mode->StartTracking(nullptr, nullptr));
    TestTrue(TEXT("move node"), Mode->InputDelta(nullptr, nullptr, Drag, Rotation, Scale));
    TestTrue(TEXT("viewport Esc consumed"), Mode->InputKey(nullptr, nullptr, EKeys::Escape, IE_Pressed));
    TestTrue(TEXT("mode teardown waits for input dispatch to finish"), Subsystem->IsEditingComposite());
    const uint32 CancelledRevision = Session->GetDraft()->GetRevision();
    TestTrue(TEXT("remaining drag before teardown is consumed"), Mode->InputDelta(nullptr, nullptr, Drag, Rotation, Scale));
    TestEqual(TEXT("remaining drag writes nothing"), Session->GetDraft()->GetRevision(), CancelledRevision);
    const double Started = FPlatformTime::Seconds();
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Backend, Fixture, Before, Started]()
    {
        UMHCompositeLevelSubsystem* Current = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
        if (Current->IsEditingComposite() && FPlatformTime::Seconds() - Started < 1.0) return false;
        TestFalse(TEXT("one viewport Esc closes the session on the next tick"), Current->IsEditingComposite());
        TestFalse(TEXT("Composite Edit mode is inactive"), UMHCompositeEditorMode::IsActive());
        TestFalse(TEXT("cancelled gesture leaves no undo"), GEditor->Trans->CanUndo());
        TArray<uint8> After;
        TestTrue(TEXT("source survives viewport Esc unchanged"), FCompositeEditFixture::AssetBytes(*Fixture->Child, After) && After == Before);
        return true;
    }));
    return true;
}

} // namespace UE::MimirComposite::Tests
