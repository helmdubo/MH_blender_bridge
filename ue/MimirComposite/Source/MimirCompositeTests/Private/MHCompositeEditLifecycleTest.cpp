#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor/Transactor.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace UE::MimirComposite::Tests
{
namespace
{

int32 ProjectionActorsIn(UWorld* World)
{
    int32 Count = 0;
    if (World == nullptr) return Count;
    for (TActorIterator<AMHCompositeEditProjectionActor> It(World); It; ++It)
    {
        if (IsValid(*It)) ++Count;
    }
    return Count;
}

int32 LiveSessions()
{
    int32 Count = 0;
    for (TObjectIterator<UMHCompositeEditSession> It; It; ++It)
    {
        if (IsValid(*It) && !It->HasAnyFlags(RF_ClassDefaultObject)) ++Count;
    }
    return Count;
}

} // namespace

// CE-6a (spec CE-6 "zero leftover delegates/proxies/leases после повторных
// циклов"): repeated open/close cycles leave no projection actor, no
// suppressed instance, no session object and no undo step behind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditLifecycleCyclesTest,
    "Mimir.V5.Composite.EditMode.Lifecycle.RepeatedCyclesLeaveNothingBehind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditLifecycleCyclesTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    const FString Prefix = F.Root->LogicalName + TEXT(":");
    const TArray<FVector> ABefore = FCompositeEditFixture::LeafWorldsUnder(*F.A, Prefix);
    const TArray<FVector> BBefore = FCompositeEditFixture::LeafWorldsUnder(*F.B, Prefix);
    bool bPassed = true;
    FString Error;
    for (int32 Cycle = 0; Cycle < 20; ++Cycle)
    {
        // Nested and root sessions alternate; every other cycle edits before leaving.
        const bool bRoot = (Cycle % 2) == 1;
        const bool bOpened = bRoot ? Subsystem->BeginEditComposite(F.A, Error) : Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error);
        if (!TestTrue(FString::Printf(TEXT("cycle %d opens: %s"), Cycle, *Error), bOpened)) return false;
        UMHCompositeEditSession* Session = Subsystem->GetEditSession();
        if (Session != nullptr && Session->GetDraft() != nullptr && (Cycle % 4) < 2)
        {
            Session->SetNodeTransform(Session->GetDraft()->GetNodeId(0), FTransform(FVector(0.0, 0.0, 10.0 * Cycle)), Error);
        }
        bPassed &= TestEqual(FString::Printf(TEXT("cycle %d: one projection actor"), Cycle), ProjectionActorsIn(F.World), 1);
        bPassed &= TestTrue(FString::Printf(TEXT("cycle %d closes"), Cycle), Subsystem->CancelEditComposite(Error));
    }
    bPassed &= TestFalse(TEXT("no session"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("no mode"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestEqual(TEXT("no projection actor left"), ProjectionActorsIn(F.World), 0);
    bPassed &= TestTrue(TEXT("every instance of A is back"), FCompositeEditFixture::SameLocations(ABefore, FCompositeEditFixture::LeafWorldsUnder(*F.A, Prefix)));
    bPassed &= TestTrue(TEXT("B never moved"), FCompositeEditFixture::SameLocations(BBefore, FCompositeEditFixture::LeafWorldsUnder(*F.B, Prefix)));
    bPassed &= TestFalse(TEXT("no undo step survives the last session"), GEditor->Trans->CanUndo());
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    bPassed &= TestEqual(TEXT("no session object survives garbage collection"), LiveSessions(), 0);
    return bPassed;
}

// CE-6a (spec CE-6 "world/host deletion, shutdown"): the session ends when
// its placement is deleted or its world is cleaned up — no dangling
// projection, no stale mode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditLifecycleTeardownTest,
    "Mimir.V5.Composite.EditMode.Lifecycle.RootDeletionAndWorldCleanupEndTheSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditLifecycleTeardownTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* SecondA = FCompositeEditFixture::Invocation(*F.A, 1);
    const FMHResolvedCompositeNode* SecondB = FCompositeEditFixture::Invocation(*F.B, 1);
    if (!TestNotNull(TEXT("second of A"), SecondA) || !TestNotNull(TEXT("second of B"), SecondB)) return false;
    const FString SecondAPath = SecondA->NodePath;
    const FString SecondBPath = SecondB->NodePath;
    FString Error;

    // The placement is deleted while it is being edited.
    if (!TestTrue(TEXT("session on A: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondAPath, Error))) return false;
    bool bPassed = TestEqual(TEXT("A: one projection actor"), ProjectionActorsIn(F.World), 1);
    bPassed &= TestTrue(TEXT("A is deleted"), F.World->DestroyActor(F.A));
    F.A = nullptr;
    bPassed &= TestFalse(TEXT("A's session ended with it"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("A's mode is gone"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestEqual(TEXT("A's projection is gone"), ProjectionActorsIn(F.World), 0);

    // The world is cleaned up while another placement is being edited.
    if (!TestTrue(TEXT("session on B: ") + Error, Subsystem->BeginEditNestedComposite(F.B, SecondBPath, Error))) return false;
    bPassed &= TestEqual(TEXT("B: one projection actor"), ProjectionActorsIn(F.World), 1);
    UWorld* World = F.World;
    F.World = nullptr;
    F.B = nullptr;
    World->DestroyWorld(true);
    bPassed &= TestFalse(TEXT("B's session ended with the world"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("B's mode is gone"), UMHCompositeEditorMode::IsActive());
    return bPassed;
}

// CE-6a (A28): the projection is invisible to save/cook/PIE — transient and
// duplicate-transient — while remaining renderable in editor Game View.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditLifecycleInvisibilityTest,
    "Mimir.V5.Composite.EditMode.Lifecycle.ProjectionNeverSavesOrCooks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditLifecycleInvisibilityTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    AActor* Actor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    if (!TestNotNull(TEXT("projection actor"), Actor)) return false;
    bool bPassed = TestTrue(TEXT("transient (never saved)"), Actor->HasAnyFlags(RF_Transient));
    bPassed &= TestTrue(TEXT("duplicate-transient (never copied into PIE)"), Actor->HasAnyFlags(RF_DuplicateTransient));
    bPassed &= TestFalse(TEXT("visible in editor Game View"), Actor->IsHidden());
    bPassed &= TestFalse(TEXT("off the World Outliner"), Actor->IsListedInSceneOutliner());
    bPassed &= TestTrue(TEXT("outside any actor package"), Actor->GetExternalPackage() == nullptr);
    int32 Components = 0;
    for (const TObjectPtr<USceneComponent>& Component : Projection->GetComponents())
    {
        if (!IsValid(Component)) continue;
        ++Components;
        bPassed &= TestTrue(TEXT("component transient: ") + Projection->GetOriginForComponent(Component), Component->HasAnyFlags(RF_Transient));
        bPassed &= TestTrue(TEXT("component duplicate-transient: ") + Projection->GetOriginForComponent(Component), Component->HasAnyFlags(RF_DuplicateTransient));
        bPassed &= TestFalse(TEXT("component visible in editor Game View: ") + Projection->GetOriginForComponent(Component), Component->bHiddenInGame);
    }
    bPassed &= TestTrue(TEXT("the projection has components"), Components > 0);

    // Exercise the same persistent duplicate archive and PPF_DuplicateForPIE
    // path used by UWorld::DuplicateWorldForPIE. The projection must be absent
    // as an object, rather than merely carrying an advisory flag.
    const FString DuplicatePackageName = TEXT("/Temp/MHCompositeProjectionPIE_") +
        FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* DuplicatePackage = CreatePackage(*DuplicatePackageName);
    DuplicatePackage->SetPackageFlags(PKG_PlayInEditor | PKG_NewlyCreated);
    UWorld* DuplicateWorld = UWorld::GetDuplicatedWorldForPIE(F.World, DuplicatePackage, 991);
    bPassed &= TestNotNull(TEXT("PIE duplicate world"), DuplicateWorld);
    if (DuplicateWorld != nullptr)
    {
        bPassed &= TestEqual(TEXT("PIE duplicate contains no projection actor"), ProjectionActorsIn(DuplicateWorld), 0);
        DuplicateWorld->DestroyWorld(false);
        DuplicateWorld->MarkAsGarbage();
    }
    DuplicatePackage->MarkAsGarbage();

    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
