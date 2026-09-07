#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FPerfV2Scope
{
    bool bPrevious = false;
    FPerfV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FPerfV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
    }
};

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
    const FPerfV2Scope V2;
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

} // namespace UE::MimirComposite::Tests
