#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor/Transactor.h"
#include "Selection.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FTransformV2Scope
{
    bool bPrevious = false;
    FTransformV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FTransformV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
    }
};

FVector DraftTranslation(const UMHCompositeEditDocument& Draft, const int32 Index)
{
    return Draft.GetNodes().IsValidIndex(Index) ? Draft.GetNodes()[Index].Transform.GetTranslation() : FVector(TNumericLimits<double>::Max());
}

bool Gesture(UMHCompositeEditorMode& Mode, const FVector& Drag, const FRotator& Rot = FRotator::ZeroRotator, const FVector& Scale = FVector::ZeroVector, const int32 Steps = 1)
{
    if (!Mode.StartTracking(nullptr, nullptr)) return false;
    bool bOk = true;
    for (int32 Step = 0; Step < Steps; ++Step)
    {
        FVector StepDrag = Drag;
        FRotator StepRot = Rot;
        FVector StepScale = Scale;
        bOk &= Mode.InputDelta(nullptr, nullptr, StepDrag, StepRot, StepScale);
    }
    return Mode.EndTracking(nullptr, nullptr) && bOk;
}

} // namespace

// CE-4a (spec CE-ADR-3, A07, A11): a gizmo gesture on a projection node is
// event-driven — StartTracking opens one transaction, every InputDelta
// writes the draft (local transform under the node's parent), EndTracking
// closes it; Undo/Redo inside the session restore draft and projection and
// keep the session; the history restarts on enter (BPP policy).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeGestureTest,
    "Mimir.V5.Composite.EditMode.Transform.GizmoGestureWritesTheDraftInOneUndoStep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeGestureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FTransformV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* First = FCompositeEditFixture::Invocation(*F.A, 0);
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("first"), First) || !TestNotNull(TEXT("second"), Second)) return false;
    const FString FirstPath = First->NodePath;
    const FString SecondPath = Second->NodePath;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    bool bPassed = TestFalse(TEXT("the undo history restarts on enter"), GEditor->Trans->CanUndo());
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("mode"), Mode) || !TestNotNull(TEXT("projection"), Projection) || !TestNotNull(TEXT("draft"), Draft)) return false;
    const FString Prefix = SecondPath + TEXT(">") + F.Child->LogicalName + TEXT(":");
    const FString PlainOrigin = Prefix + TEXT("nodes[0]");
    const FString GroupedOrigin = Prefix + TEXT("nodes[1]/children[0]");
    USceneComponent* Plain = Projection->FindComponentForOrigin(PlainOrigin);
    if (!TestNotNull(TEXT("plain"), Plain)) return false;
    const FVector PlainBefore = Plain->GetComponentLocation();
    const FVector Node0Before = DraftTranslation(*Draft, 0);

    // One gesture, three deltas, one undo step.
    bPassed &= TestTrue(TEXT("grab the node"), Mode->SelectComponent(Plain));
    bPassed &= TestTrue(TEXT("gesture"), Gesture(*Mode, FVector(0.0, 0.0, 10.0), FRotator::ZeroRotator, FVector::ZeroVector, 3));
    Plain = Projection->FindComponentForOrigin(PlainOrigin);
    bPassed &= TestTrue(TEXT("the projection followed the gesture"), Plain != nullptr && Plain->GetComponentLocation().Equals(PlainBefore + FVector(0.0, 0.0, 30.0), 1e-2));
    bPassed &= TestTrue(TEXT("the draft carries the gesture"), DraftTranslation(*Draft, 0).Equals(Node0Before + FVector(0.0, 0.0, 30.0), 1e-2));
    bPassed &= TestTrue(TEXT("dirty"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("undo runs"), GEditor->UndoTransaction());
    bPassed &= TestTrue(TEXT("the session survived Undo"), Subsystem->GetEditSession() == Session && Session->IsOpen() && UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("undo restored the draft"), DraftTranslation(*Draft, 0).Equals(Node0Before, 1e-2));
    Plain = Projection->FindComponentForOrigin(PlainOrigin);
    bPassed &= TestTrue(TEXT("undo restored the projection"), Plain != nullptr && Plain->GetComponentLocation().Equals(PlainBefore, 1e-2));
    bPassed &= TestFalse(TEXT("clean again"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("redo runs"), GEditor->RedoTransaction());
    bPassed &= TestTrue(TEXT("redo re-applied the gesture"), DraftTranslation(*Draft, 0).Equals(Node0Before + FVector(0.0, 0.0, 30.0), 1e-2));
    Plain = Projection->FindComponentForOrigin(PlainOrigin);
    bPassed &= TestTrue(TEXT("redo moved the projection again"), Plain != nullptr && Plain->GetComponentLocation().Equals(PlainBefore + FVector(0.0, 0.0, 30.0), 1e-2));
    bPassed &= TestFalse(TEXT("one undo step per gesture"), GEditor->Trans->CanUndo() && GEditor->Trans->GetUndoCount() > 1);

    // A node under a group: the local transform is relative to the group.
    USceneComponent* Grouped = Projection->FindComponentForOrigin(GroupedOrigin);
    if (!TestNotNull(TEXT("grouped"), Grouped)) return false;
    const FVector GroupedBefore = Grouped->GetComponentLocation();
    const FVector Node2Before = DraftTranslation(*Draft, 2);
    bPassed &= TestTrue(TEXT("grab the grouped node"), Mode->SelectComponent(Grouped));
    bPassed &= TestTrue(TEXT("grouped gesture"), Gesture(*Mode, FVector(5.0, 0.0, 0.0)));
    Grouped = Projection->FindComponentForOrigin(GroupedOrigin);
    bPassed &= TestTrue(TEXT("the grouped node moved in the world"), Grouped != nullptr && Grouped->GetComponentLocation().Equals(GroupedBefore + FVector(5.0, 0.0, 0.0), 1e-2));
    bPassed &= TestTrue(TEXT("the grouped node's local moved under its group"), DraftTranslation(*Draft, 2).Equals(Node2Before + FVector(5.0, 0.0, 0.0), 1e-2));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));

    // A rotated occurrence (first: yaw 90): a world drag lands as the right local delta.
    if (!TestTrue(TEXT("first context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, FirstPath, Error))) return false;
    Mode = UMHCompositeEditorMode::GetActive();
    Session = Subsystem->GetEditSession();
    Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("mode 2"), Mode) || !TestNotNull(TEXT("projection 2"), Projection) || !TestNotNull(TEXT("draft 2"), Draft)) return false;
    const FString FirstOrigin = FirstPath + TEXT(">") + F.Child->LogicalName + TEXT(":nodes[0]");
    USceneComponent* Rotated = Projection->FindComponentForOrigin(FirstOrigin);
    if (!TestNotNull(TEXT("rotated plain"), Rotated)) return false;
    const FVector RotatedBefore = Rotated->GetComponentLocation();
    const FVector RotatedNodeBefore = DraftTranslation(*Draft, 0);
    bPassed &= TestTrue(TEXT("grab under the rotated occurrence"), Mode->SelectComponent(Rotated));
    bPassed &= TestTrue(TEXT("rotated gesture"), Gesture(*Mode, FVector(10.0, 0.0, 0.0)));
    Rotated = Projection->FindComponentForOrigin(FirstOrigin);
    bPassed &= TestTrue(TEXT("world drag lands in the world"), Rotated != nullptr && Rotated->GetComponentLocation().Equals(RotatedBefore + FVector(10.0, 0.0, 0.0), 1e-2));
    bPassed &= TestTrue(TEXT("the local delta is the drag brought under yaw 90"), DraftTranslation(*Draft, 0).Equals(RotatedNodeBefore + FVector(0.0, -10.0, 0.0), 1e-2));
    bPassed &= TestTrue(TEXT("cancel 2"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// CE-4a: the framed occurrence is not draggable as a whole (v1), and a
// click without a drag leaves no undo step.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeGestureEdgesTest,
    "Mimir.V5.Composite.EditMode.Transform.ActorDragIsSwallowedAndEmptyGestureLeavesNoStep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeGestureEdgesTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FTransformV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    AActor* ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    if (!TestNotNull(TEXT("mode"), Mode) || !TestNotNull(TEXT("projection actor"), ProjectionActor)) return false;

    // The frame (actor-only selection, as on enter) swallows the gesture.
    bool bPassed = TestTrue(TEXT("the frame is selected on enter"), ProjectionActor->IsSelected() && GEditor->GetSelectedComponentCount() == 0);
    bPassed &= TestTrue(TEXT("a frame gesture is swallowed"), Gesture(*Mode, FVector(10.0, 0.0, 0.0)));
    bPassed &= TestFalse(TEXT("it changed nothing"), Session->IsDirty());
    bPassed &= TestFalse(TEXT("it left no undo step"), GEditor->Trans->CanUndo());

    // A click without a drag on a node: no step either.
    const FString PlainOrigin = SecondPath + TEXT(">") + F.Child->LogicalName + TEXT(":nodes[0]");
    USceneComponent* Plain = Projection->FindComponentForOrigin(PlainOrigin);
    if (!TestNotNull(TEXT("plain"), Plain)) return false;
    bPassed &= TestTrue(TEXT("grab"), Mode->SelectComponent(Plain));
    bPassed &= TestTrue(TEXT("empty gesture"), Mode->StartTracking(nullptr, nullptr) && Mode->EndTracking(nullptr, nullptr));
    bPassed &= TestFalse(TEXT("still clean"), Session->IsDirty());
    bPassed &= TestFalse(TEXT("an empty gesture leaves no undo step"), GEditor->Trans->CanUndo());

    // Nothing of ours selected: the gesture is not ours.
    GEditor->SelectNone(false, true, false);
    bPassed &= TestFalse(TEXT("no selection: not our gesture"), Mode->StartTracking(nullptr, nullptr));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
