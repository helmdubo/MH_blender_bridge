#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Selection.h"

namespace UE::MimirComposite::Tests
{
namespace
{

double MillisecondsSince(const double Start)
{
    return (FPlatformTime::Seconds() - Start) * 1000.0;
}

int32 ComponentsOf(const AActor* Actor)
{
    return Actor != nullptr ? Actor->GetComponents().Num() : 0;
}

} // namespace

// CE-6c (spec CE-6 "Метрики"): the baseline numbers of the CE backend on
// this host — cold and warm Enter/Exit, one hundred gesture updates, the
// peak component count of the projection, and the steady state after fifty
// cycles. Numbers are reported, not judged: thresholds are agreed on a
// baseline, not invented here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditPerfBaselineTest,
    "Mimir.V5.Composite.EditMode.Perf.BaselineNumbersAreReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditPerfBaselineTest::RunTest(const FString& Parameters)
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
    bool bPassed = true;

    // Cold enter / exit.
    double Start = FPlatformTime::Seconds();
    if (!TestTrue(TEXT("cold enter: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    const double ColdEnterMs = MillisecondsSince(Start);
    const UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    const int32 PeakComponents = Session != nullptr && Session->GetProjection() != nullptr ? ComponentsOf(Session->GetProjection()->GetProjectionActor()) : 0;
    Start = FPlatformTime::Seconds();
    bPassed &= TestTrue(TEXT("cold exit"), Subsystem->CancelEditComposite(Error));
    const double ColdExitMs = MillisecondsSince(Start);

    // Warm enter / exit.
    Start = FPlatformTime::Seconds();
    if (!TestTrue(TEXT("warm enter: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    const double WarmEnterMs = MillisecondsSince(Start);

    // One hundred gesture updates on a leaf.
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Live = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Live != nullptr ? Live->GetProjection() : nullptr;
    USceneComponent* Plain = Projection != nullptr ? Projection->FindComponentForOrigin(Prefix + TEXT("nodes[0]")) : nullptr;
    double UpdatesMs = -1.0;
    if (Mode != nullptr && Plain != nullptr && Mode->SelectComponent(Plain) && Mode->StartTracking(nullptr, nullptr))
    {
        Start = FPlatformTime::Seconds();
        for (int32 Step = 0; Step < 100; ++Step)
        {
            FVector Drag(0.0, 0.0, 1.0);
            FRotator Rot(FRotator::ZeroRotator);
            FVector Scale(FVector::ZeroVector);
            Mode->InputDelta(nullptr, nullptr, Drag, Rot, Scale);
        }
        UpdatesMs = MillisecondsSince(Start);
        Mode->EndTracking(nullptr, nullptr);
    }
    bPassed &= TestTrue(TEXT("a hundred updates ran"), UpdatesMs >= 0.0);
    Start = FPlatformTime::Seconds();
    bPassed &= TestTrue(TEXT("warm exit"), Subsystem->CancelEditComposite(Error));
    const double WarmExitMs = MillisecondsSince(Start);

    // Fifty cycles: steady state, nothing accumulates.
    Start = FPlatformTime::Seconds();
    for (int32 Cycle = 0; Cycle < 50; ++Cycle)
    {
        if (!Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error) || !Subsystem->CancelEditComposite(Error))
        {
            bPassed &= TestTrue(FString::Printf(TEXT("cycle %d: %s"), Cycle, *Error), false);
            break;
        }
    }
    const double FiftyCyclesMs = MillisecondsSince(Start);
    int32 LeftoverProjections = 0;
    for (TActorIterator<AMHCompositeEditProjectionActor> It(F.World); It; ++It)
    {
        if (IsValid(*It)) ++LeftoverProjections;
    }
    bPassed &= TestEqual(TEXT("no projection actor after fifty cycles"), LeftoverProjections, 0);

    AddInfo(FString::Printf(TEXT("PERF cold enter %.2f ms, cold exit %.2f ms, warm enter %.2f ms, 100 updates %.2f ms (%.3f ms/update), warm exit %.2f ms, 50 cycles %.2f ms (%.2f ms/cycle), projection components %d"),
        ColdEnterMs, ColdExitMs, WarmEnterMs, UpdatesMs, UpdatesMs / 100.0, WarmExitMs, FiftyCyclesMs, FiftyCyclesMs / 50.0, PeakComponents));
    return bPassed;
}

// A gesture is one authoring command from the editor's point of view.  The
// projection may refresh its derived visuals for each delta, but session and
// selection observers must not see one authored change per mouse sample.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditPerfGestureNotificationTest,
    "Mimir.V5.Composite.EditMode.Perf.GestureNotificationsAreBatched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditPerfGestureNotificationTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;

    FString Error;
    if (!TestTrue(TEXT("open nested"), Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    if (!TestNotNull(TEXT("session"), Session) || !TestNotNull(TEXT("projection"), Projection) || !TestNotNull(TEXT("mode"), Mode)) return false;

    const FString Origin = Second->NodePath + TEXT(">") + F.Child->LogicalName + TEXT(":nodes[0]");
    USceneComponent* BeforeComponent = Projection->FindComponentForOrigin(Origin);
    if (!TestNotNull(TEXT("edited component"), BeforeComponent)) return false;

    int32 ChangedEvents = 0;
    const FDelegateHandle ChangedHandle = Session->OnChanged.AddLambda([&ChangedEvents]() { ++ChangedEvents; });
    ON_SCOPE_EXIT
    {
        if (IsValid(Session)) Session->OnChanged.Remove(ChangedHandle);
    };
    if (!TestTrue(TEXT("select component"), Mode->SelectComponent(BeforeComponent))) return false;
    const TArray<FGuid>& SelectedIds = Session->GetSelectedNodeIds();
    if (!TestEqual(TEXT("one selected logical node"), SelectedIds.Num(), 1)) return false;
    const FGuid SelectedId = SelectedIds[0];
    FMHCompositeEditNodeFrame BeforeSelectedFrame;
    if (!TestTrue(TEXT("selected frame"), Projection->GetNodeFrame(SelectedId, BeforeSelectedFrame))) return false;
    int32 NativeSelectionEvents = 0;
    const FDelegateHandle NativeSelectionHandle = USelection::SelectionChangedEvent.AddLambda([&NativeSelectionEvents](UObject*)
    {
        ++NativeSelectionEvents;
    });
    ON_SCOPE_EXIT
    {
        USelection::SelectionChangedEvent.Remove(NativeSelectionHandle);
    };
    if (!TestTrue(TEXT("start gesture"), Mode->StartTracking(nullptr, nullptr))) return false;
    const uint64 GraphBuildsBeforeGesture = Projection->GetFullGraphBuildCount();

    for (int32 Step = 0; Step < 100; ++Step)
    {
        FVector Drag(0.0, 0.0, 1.0);
        FRotator Rotation = FRotator::ZeroRotator;
        FVector Scale = FVector::ZeroVector;
        if (!TestTrue(FString::Printf(TEXT("delta %d"), Step), Mode->InputDelta(nullptr, nullptr, Drag, Rotation, Scale))) return false;
    }
    TestEqual(TEXT("session observer is quiet during gesture samples"), ChangedEvents, 0);
    TestEqual(TEXT("native selection observer is quiet during gesture samples"), NativeSelectionEvents, 0);
    TestEqual(TEXT("draft graph is cached during gesture samples"), Projection->GetFullGraphBuildCount(), GraphBuildsBeforeGesture);
    if (!TestTrue(TEXT("end gesture"), Mode->EndTracking(nullptr, nullptr))) return false;
    TestTrue(TEXT("gesture emits at most one final session change"), ChangedEvents <= 1);
    FMHCompositeEditNodeFrame AfterTranslationFrame;
    TestTrue(TEXT("frame after translation gesture"), Projection->GetNodeFrame(SelectedId, AfterTranslationFrame));
    TestTrue(TEXT("translation applied once"), AfterTranslationFrame.WorldMatrix.GetOrigin().Equals(BeforeSelectedFrame.WorldMatrix.GetOrigin() + FVector(0.0, 0.0, 100.0)));

    // Exercise the same cached gesture path with rotational and scale deltas;
    // the forced refresh below is the full-resolve oracle for all three axes.
    if (!TestTrue(TEXT("start rotation gesture"), Mode->StartTracking(nullptr, nullptr))) return false;
    FVector NoDrag = FVector::ZeroVector;
    FRotator Rotation(0.0, 15.0, 0.0);
    FVector NoScale = FVector::ZeroVector;
    if (!TestTrue(TEXT("rotation delta"), Mode->InputDelta(nullptr, nullptr, NoDrag, Rotation, NoScale))) return false;
    if (!TestTrue(TEXT("end rotation gesture"), Mode->EndTracking(nullptr, nullptr))) return false;
    if (!TestTrue(TEXT("start scale gesture"), Mode->StartTracking(nullptr, nullptr))) return false;
    Rotation = FRotator::ZeroRotator;
    FVector Scale(0.1, 0.0, 0.0);
    if (!TestTrue(TEXT("scale delta"), Mode->InputDelta(nullptr, nullptr, NoDrag, Rotation, Scale))) return false;
    if (!TestTrue(TEXT("end scale gesture"), Mode->EndTracking(nullptr, nullptr))) return false;

    USceneComponent* AfterComponent = Projection->FindComponentForOrigin(Origin);
    TestTrue(TEXT("component identity survives gesture"), AfterComponent == BeforeComponent);
    FMHCompositeEditNodeFrame AfterFrame;
    TestTrue(TEXT("edited frame after TRS gestures"), Projection->GetNodeFrame(SelectedId, AfterFrame));

    if (!TestTrue(TEXT("forced projection refresh"), Projection->Refresh(Error))) return false;
    USceneComponent* RefreshedComponent = Projection->FindComponentForOrigin(Origin);
    TestTrue(TEXT("component identity survives forced refresh"), RefreshedComponent == BeforeComponent);
    FMHCompositeEditNodeFrame RefreshedFrame;
    TestTrue(TEXT("frame after forced refresh"), Projection->GetNodeFrame(Session->GetSelectedNodeIds()[0], RefreshedFrame));
    TestTrue(TEXT("forced refresh preserves final transform"), RefreshedFrame.WorldMatrix.Equals(AfterFrame.WorldMatrix, 0.01));
    TestEqual(TEXT("one full graph build for forced refresh"), Projection->GetFullGraphBuildCount(), GraphBuildsBeforeGesture + 1);

    Subsystem->CancelEditComposite(Error);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditPerfSubtreeTransformOracleTest,
    "Mimir.V5.Composite.EditMode.Perf.SubtreeTransformMatchesFullRefresh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditPerfSubtreeTransformOracleTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;

    // Make the grouped descendant procedural while retaining the fixture's
    // repeated child invocation and nonzero occurrence frame.
    if (!TestTrue(TEXT("child grouped node exists"), F.Child->Nodes.Num() > 2)) return false;
    F.Child->Nodes[2].bHasInlinePlacement = true;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("open nested"), Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    if (!TestNotNull(TEXT("session"), Session) || !TestNotNull(TEXT("projection"), Projection) || !TestNotNull(TEXT("mode"), Mode)) return false;
    UMHCompositeEditDocument* Draft = Session->GetDraft();
    const FGuid GroupId = Draft->GetNodeId(1);
    const FGuid ProceduralChildId = Draft->GetNodeId(2);
    FMHCompositeEditNodeFrame ProceduralFrame;
    if (!TestTrue(TEXT("procedural descendant frame"), Projection->GetNodeFrame(ProceduralChildId, ProceduralFrame))) return false;
    if (!TestTrue(TEXT("descendant is generated"), ProceduralFrame.bGeneratedTransform)) return false;
    const TArray<USceneComponent*> Components = Projection->GetComponentsForNodeId(GroupId, true);
    if (!TestTrue(TEXT("group has projected subtree"), Components.Num() >= 2)) return false;

    Mode->SelectNodeIds({GroupId}, GroupId);
    if (!TestEqual(TEXT("one group selected"), Session->GetSelectedNodeIds().Num(), 1)) return false;
    if (!TestTrue(TEXT("start group gesture"), Mode->StartTracking(nullptr, nullptr))) return false;
    const uint64 InitialBuilds = Projection->GetFullGraphBuildCount();
    FVector Drag(7.0, -3.0, 2.0);
    FRotator Rotation(0.0, 20.0, 10.0);
    FVector Scale = FVector::ZeroVector;
    if (!TestTrue(TEXT("group TR gesture"), Mode->InputDelta(nullptr, nullptr, Drag, Rotation, Scale))) return false;
    if (!TestTrue(TEXT("end group TR gesture"), Mode->EndTracking(nullptr, nullptr))) return false;
    TestEqual(TEXT("TR gesture does not compile full graph"), Projection->GetFullGraphBuildCount(), InitialBuilds);

    TArray<FMatrix> FastMatrices;
    FastMatrices.Reserve(Components.Num());
    for (USceneComponent* Component : Components) FastMatrices.Add(Component->GetComponentTransform().ToMatrixWithScale());
    if (!TestTrue(TEXT("full refresh oracle"), Projection->Refresh(Error))) return false;
    TestEqual(TEXT("forced refresh compiles once"), Projection->GetFullGraphBuildCount(), InitialBuilds + 1);
    for (int32 Index = 0; Index < Components.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("TR subtree component %d matches full refresh"), Index),
            Components[Index] != nullptr && Components[Index]->GetComponentTransform().ToMatrixWithScale().Equals(FastMatrices[Index], 0.01));
    }

    if (!TestTrue(TEXT("start uniform scale gesture"), Mode->StartTracking(nullptr, nullptr))) return false;
    Drag = FVector::ZeroVector;
    Rotation = FRotator::ZeroRotator;
    Scale = FVector(0.15, 0.15, 0.15);
    const uint64 ScaleBuilds = Projection->GetFullGraphBuildCount();
    if (!TestTrue(TEXT("uniform scale delta"), Mode->InputDelta(nullptr, nullptr, Drag, Rotation, Scale))) return false;
    if (!TestTrue(TEXT("end uniform scale gesture"), Mode->EndTracking(nullptr, nullptr))) return false;
    TestEqual(TEXT("scale gesture does not compile full graph"), Projection->GetFullGraphBuildCount(), ScaleBuilds);
    FastMatrices.Reset();
    for (USceneComponent* Component : Components) FastMatrices.Add(Component->GetComponentTransform().ToMatrixWithScale());
    if (!TestTrue(TEXT("full scale refresh oracle"), Projection->Refresh(Error))) return false;
    TestEqual(TEXT("forced scale refresh compiles once"), Projection->GetFullGraphBuildCount(), ScaleBuilds + 1);
    for (int32 Index = 0; Index < Components.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("scale subtree component %d matches full refresh"), Index),
            Components[Index] != nullptr && Components[Index]->GetComponentTransform().ToMatrixWithScale().Equals(FastMatrices[Index], 0.01));
    }

    Subsystem->CancelEditComposite(Error);
    return true;
}

} // namespace UE::MimirComposite::Tests
