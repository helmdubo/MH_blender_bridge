#include "MHRecipeTestFixture.h"

#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeRuntimeBridge.h"
#include "Composite/MHProofCache.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Source/MHSourceResolver.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Root {mesh A; random{mesh B, nested composite{mesh C}, empty}} on real mesh receipts. */
struct FProofFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    AMHCompositeActor* Actor = nullptr;
    UMHCompositeAsset* Root = nullptr;
    FString MeshA;
    FString MeshB;
    FString MeshC;
    UStaticMesh* MeshCObject = nullptr;

    explicit FProofFixture(FAutomationTestBase& Test) : Recipe(Test) {}

    ~FProofFixture()
    {
        if (UMHProofCacheSubsystem* Proofs = UMHProofCacheSubsystem::Get())
        {
            Proofs->SetSourceHashProviderForTests(nullptr);
            Proofs->InvalidateAll();
        }
        if (World != nullptr) World->DestroyWorld(true);
    }

    bool Build(FAutomationTestBase& Test)
    {
        MeshA = Recipe.Name(TEXT("proof_mesh_a"));
        MeshB = Recipe.Name(TEXT("proof_mesh_b"));
        MeshC = Recipe.Name(TEXT("proof_mesh_c"));
        Recipe.Mesh(MeshA);
        Recipe.Mesh(MeshB);
        MeshCObject = Recipe.Mesh(MeshC);
        FMHCompositeDocument ChildDocument;
        {
            FMHCompositeNode& Leaf = ChildDocument.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshC;
        }
        const FString ChildName = Recipe.Name(TEXT("proof_child_cmp"));
        if (Recipe.Composite(ChildName, ChildDocument, {}) == nullptr) return false;
        FMHCompositeDocument RootDocument;
        {
            FMHCompositeNode& Anchor = RootDocument.Nodes.AddDefaulted_GetRef();
            Anchor.Kind = EMHCompositeNodeKind::Mesh;
            Anchor.Resource = MeshA;
            FMHCompositeNode& Random = RootDocument.Nodes.AddDefaulted_GetRef();
            Random.Kind = EMHCompositeNodeKind::Random;
            // The nested composite is a zero-weight option: never selected by
            // any seed, always part of the full closure (the proof must see it).
            Random.Options.Add({EMHCompositeOptionKind::Mesh, MeshB, 1.0f});
            Random.Options.Add({EMHCompositeOptionKind::Composite, ChildName, 0.0f});
            Random.Options.Add({EMHCompositeOptionKind::Empty, FString(), 1.0f});
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("proof_root_cmp")), RootDocument, {});
        if (Root == nullptr) return false;
        World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
        if (!Test.TestNotNull(TEXT("proof world"), World)) return false;
        Actor = World->SpawnActor<AMHCompositeActor>();
        if (!Test.TestNotNull(TEXT("proof placement"), Actor)) return false;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(5);
        Actor->SetAppearanceSeed(9);
        Actor->SetCompositeAsset(Root);
        return Test.TestTrue(TEXT("preview builds: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty());
    }
};

FMHResourceKey ProofMeshKey(const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::StaticMesh;
    Key.LogicalName = Name;
    return Key;
}

} // namespace

// ---------------------------------------------------------------------------
// R2c (Recipe Model v2 §2.6): the proof plane lives in the proof cache and at
// the exit points. The preview never builds it; PreSaveWorld only reads it;
// preflight and snapshot build it synchronously and refuse Stale/Missing.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProofBuildPreflightFullClosureTest,
    "Mimir.V5.Composite.Proof.BuildPreflightFullClosure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProofBuildPreflightFullClosureTest::RunTest(const FString& Parameters)
{
    UMHProofCacheSubsystem* Proofs = UMHProofCacheSubsystem::Get();
    if (!TestNotNull(TEXT("proof cache subsystem"), Proofs)) return false;
    Proofs->InvalidateAll();
    FProofFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    AMHCompositeActor& Actor = *Fixture.Actor;

    // 1. The preview built nothing of the proof plane.
    bool bPassed = TestTrue(TEXT("preview leaves the proof Unknown"), Proofs->GetProofState(Actor).State == EMHProofState::Unknown);

    // 2. Deferred proof: request, flush, read. Fresh, with closure and signatures,
    //    and layout parity with the resident preview plan.
    MHResetPlacementStageMetrics();
    bPassed &= TestTrue(TEXT("request schedules a deferred proof"), Proofs->RequestProof(Actor) == EMHProofState::ProofPending);
    bPassed &= TestEqual(TEXT("request itself builds nothing"), MHGetPlacementStageMetrics().Get(EMHPlacementStage::BuildAppliedGraph).Calls, 0ull);
    Proofs->FlushPendingProofs();
    const FMHProofResult Fresh = Proofs->GetProofState(Actor);
    bPassed &= TestTrue(TEXT("flushed proof is Fresh: ") + Fresh.Diagnostic, Fresh.State == EMHProofState::Fresh);
    if (TestTrue(TEXT("fresh proof carries a plan"), Fresh.Plan.IsValid()))
    {
        bPassed &= TestFalse(TEXT("proof plan carries the resolved signature"), Fresh.Plan->ResolvedSignature.IsEmpty());
        bPassed &= TestFalse(TEXT("proof plan carries the placement signature"), Fresh.Plan->PlacementSignature.IsEmpty());
        bPassed &= TestTrue(TEXT("proof closure covers the unselected nested mesh"),
            Fresh.Plan->Closure.Resources.Contains(TEXT("static_mesh:") + Fixture.MeshC));
        TArray<FString> Mismatches;
        const FMHResolvedCompositePlan* Preview = Actor.GetResolvedPlan();
        bPassed &= TestTrue(TEXT("proof and preview layouts agree"),
            Preview != nullptr && MHCompareRecipeShadowParity(*Fresh.Plan, *Preview, Mismatches));
    }
    else
    {
        bPassed = false;
    }

    // 3. Stale: the index knows a newer source payload for a closure resource.
    const FString StaleKey = TEXT("static_mesh:") + Fixture.MeshC;
    Proofs->SetSourceHashProviderForTests([StaleKey](const FMHResourceKey& Key, FString& OutHash)
    {
        if (Key.ToString() != StaleKey) return false;
        OutHash = TEXT("blake3-160:ffffffffffffffffffffffffffffffffffffffff");
        return true;
    });
    Proofs->InvalidateAll();
    FMHProofResult StaleResult;
    FString Error;
    bPassed &= TestFalse(TEXT("stale receipt fails the synchronous proof"), Proofs->BuildProofNow(Actor, StaleResult, Error));
    bPassed &= TestTrue(TEXT("stale proof is Stale: ") + Error, StaleResult.State == EMHProofState::Stale);
    bPassed &= TestTrue(TEXT("stale diagnostic names the code and the key: ") + Error,
        Error.Contains(TEXT("MH_E_STALE_SOURCE")) && Error.Contains(Fixture.MeshC));
    bPassed &= TestTrue(TEXT("stale proof still admitted the closure"), StaleResult.Plan.IsValid());
    bPassed &= TestTrue(TEXT("cache reports Stale after the build"), Proofs->GetProofState(Actor).State == EMHProofState::Stale);
    bPassed &= TestTrue(TEXT("preview is untouched by a stale proof"), Actor.GetResolvedPlan() != nullptr && Actor.GetLastPlacementError().IsEmpty());
    Proofs->SetSourceHashProviderForTests(nullptr);

    // 4. Missing: an unselected endpoint loses its receipt. Preview keeps its
    //    plan; the proof, preflight and snapshot refuse.
    Fixture.MeshCObject->SetAssetImportData(nullptr);
    MHNotifyGeneratedResourceChanged(ProofMeshKey(Fixture.MeshC));
    bPassed &= TestTrue(TEXT("preview survives a missing unselected receipt"), Actor.GetResolvedPlan() != nullptr && Actor.GetLastPlacementError().IsEmpty());
    FMHProofResult MissingResult;
    Error.Reset();
    bPassed &= TestFalse(TEXT("missing receipt fails the synchronous proof"), Proofs->BuildProofNow(Actor, MissingResult, Error));
    bPassed &= TestTrue(TEXT("missing proof is Missing: ") + Error, MissingResult.State == EMHProofState::Missing);
    bPassed &= TestTrue(TEXT("missing diagnostic names the key: ") + Error,
        Error.Contains(TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE")) && Error.Contains(Fixture.MeshC));
    Error.Reset();
    bPassed &= TestFalse(TEXT("preflight refuses the world"), MHValidateRuntimeCompositeWorld(*Fixture.World, Error));
    bPassed &= TestTrue(TEXT("preflight names the missing key: ") + Error, Error.Contains(Fixture.MeshC));
    FMHRuntimeCompositeInput Snapshot;
    Error.Reset();
    bPassed &= TestFalse(TEXT("snapshot admission refuses"), MHBuildRuntimeCompositeInput(Actor, Snapshot, Error));

    // 5. Save reads the cache only: an audit builds nothing and reports the state.
    MHResetPlacementStageMetrics();
    const TArray<FMHProofAuditRow> Audit = Proofs->AuditWorld(*Fixture.World);
    bPassed &= TestEqual(TEXT("audit builds no proof"), MHGetPlacementStageMetrics().Get(EMHPlacementStage::BuildAppliedGraph).Calls, 0ull);
    const FMHProofAuditRow* Row = Audit.FindByPredicate([&](const FMHProofAuditRow& Value) { return Value.Placement.Get() == &Actor; });
    bPassed &= TestNotNull(TEXT("audit lists the placement"), Row);
    if (Row != nullptr)
    {
        bPassed &= TestTrue(TEXT("audit reports Missing"), Row->State == EMHProofState::Missing);
        bPassed &= TestTrue(TEXT("audit carries the diagnostic"), Row->Diagnostic.Contains(Fixture.MeshC));
    }
    return bPassed;
}

// Saving a map is not an exit point: it reads the proof cache, warns about
// every placement that is not Fresh, schedules a deferred proof for Unknown
// ones, and never builds a proof or refuses the save (§2.6 п. 1).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProofSaveWarnsWithoutProofTest,
    "Mimir.V5.Composite.Proof.SaveWarnsWithoutProof",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProofSaveWarnsWithoutProofTest::RunTest(const FString& Parameters)
{
    UMHProofCacheSubsystem* Proofs = UMHProofCacheSubsystem::Get();
    if (!TestNotNull(TEXT("proof cache subsystem"), Proofs)) return false;
    Proofs->InvalidateAll();
    FProofFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    AMHCompositeActor& Actor = *Fixture.Actor;
    bool bPassed = TestTrue(TEXT("placement starts Unknown"), Proofs->GetProofState(Actor).State == EMHProofState::Unknown);

    MHResetPlacementStageMetrics();
    {
        // A non-cook save of the preview world: no target platform.
        FObjectSaveContextData Data(Fixture.World->GetOutermost(), nullptr, TEXT(""), SAVE_None);
        FObjectPreSaveContext Context(Data);
        bPassed &= TestFalse(TEXT("probe context is not a cook"), Context.IsCooking());
        FEditorDelegates::PreSaveWorldWithContext.Broadcast(Fixture.World, Context);
    }
    bPassed &= TestEqual(TEXT("save builds no proof"), MHGetPlacementStageMetrics().Get(EMHPlacementStage::BuildAppliedGraph).Calls, 0ull);
    bPassed &= TestTrue(TEXT("save schedules the missing proof"), Proofs->GetProofState(Actor).State == EMHProofState::ProofPending);
    bPassed &= TestTrue(TEXT("save warned about the unproven placement"), Proofs->GetLastSaveAuditWarningCount() >= 1);
    bPassed &= TestTrue(TEXT("save leaves the preview alone"), Actor.GetResolvedPlan() != nullptr && Actor.GetLastPlacementError().IsEmpty());

    // The deferred proof runs later; once Fresh, the next save has nothing to warn about.
    Proofs->FlushPendingProofs();
    bPassed &= TestTrue(TEXT("deferred proof became Fresh"), Proofs->GetProofState(Actor).State == EMHProofState::Fresh);
    MHResetPlacementStageMetrics();
    {
        FObjectSaveContextData Data(Fixture.World->GetOutermost(), nullptr, TEXT(""), SAVE_None);
        FObjectPreSaveContext Context(Data);
        FEditorDelegates::PreSaveWorldWithContext.Broadcast(Fixture.World, Context);
    }
    bPassed &= TestEqual(TEXT("second save builds no proof either"), MHGetPlacementStageMetrics().Get(EMHPlacementStage::BuildAppliedGraph).Calls, 0ull);
    bPassed &= TestEqual(TEXT("fresh placement raises no save warning"), Proofs->GetLastSaveAuditWarningCount(), 0);
    return bPassed;
}

// A receipt that no longer matches the source payload known to the index is
// Stale: cook preflight and runtime snapshot admission refuse with
// MH_E_STALE_SOURCE; the preview keeps working.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProofStaleSourceBlocksCookAndSnapshotTest,
    "Mimir.V5.Composite.Proof.StaleSourceBlocksCookAndSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProofStaleSourceBlocksCookAndSnapshotTest::RunTest(const FString& Parameters)
{
    UMHProofCacheSubsystem* Proofs = UMHProofCacheSubsystem::Get();
    if (!TestNotNull(TEXT("proof cache subsystem"), Proofs)) return false;
    Proofs->InvalidateAll();
    FProofFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    AMHCompositeActor& Actor = *Fixture.Actor;
    FString Error;
    bool bPassed = TestTrue(TEXT("fresh world passes preflight: ") + Error, MHValidateRuntimeCompositeWorld(*Fixture.World, Error));

    // The selected anchor mesh is what the index now reports with a newer payload.
    const FString StaleKey = TEXT("static_mesh:") + Fixture.MeshA;
    Proofs->SetSourceHashProviderForTests([StaleKey](const FMHResourceKey& Key, FString& OutHash)
    {
        if (Key.ToString() != StaleKey) return false;
        OutHash = TEXT("blake3-160:0123456789abcdef0123456789abcdef01234567");
        return true;
    });
    Proofs->InvalidateAll();
    Error.Reset();
    bPassed &= TestFalse(TEXT("stale source refuses cook preflight"), MHValidateRuntimeCompositeWorld(*Fixture.World, Error));
    bPassed &= TestTrue(TEXT("preflight names the stale code and key: ") + Error,
        Error.Contains(TEXT("MH_E_STALE_SOURCE")) && Error.Contains(Fixture.MeshA));
    FMHRuntimeCompositeInput Snapshot;
    Error.Reset();
    bPassed &= TestFalse(TEXT("stale source refuses snapshot admission"), MHBuildRuntimeCompositeInput(Actor, Snapshot, Error));
    bPassed &= TestTrue(TEXT("snapshot names the stale code: ") + Error, Error.Contains(TEXT("MH_E_STALE_SOURCE")));
    bPassed &= TestTrue(TEXT("cache reports Stale"), Proofs->GetProofState(Actor).State == EMHProofState::Stale);
    bPassed &= TestTrue(TEXT("preview is untouched by a stale source"), Actor.GetResolvedPlan() != nullptr && Actor.GetLastPlacementError().IsEmpty());

    // The index catches up (reimport): the proof is Fresh again.
    Proofs->SetSourceHashProviderForTests(nullptr);
    Proofs->InvalidateAll();
    Error.Reset();
    bPassed &= TestTrue(TEXT("refreshed source passes preflight again: ") + Error, MHValidateRuntimeCompositeWorld(*Fixture.World, Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
