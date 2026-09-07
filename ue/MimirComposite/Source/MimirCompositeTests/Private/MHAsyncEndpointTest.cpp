#include "MHRecipeTestFixture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHInstancePool.h"
#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMesh/MHStaticMeshImporter.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** A managed mesh that exists only on disk: saved to its canonical package, then released and collected. */
struct FColdMesh
{
    FString LogicalName;
    FString PackageName;
    FString Filename;

    bool Create(FAutomationTestBase& Test, const FString& InLogicalName)
    {
        LogicalName = InLogicalName;
        PackageName = TEXT("/Game/MH/Generated/Meshes/") + LogicalName;
        Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
        UPackage* Package = CreatePackage(*PackageName);
        // Real geometry (a stock cube copy), so the saved asset passes the
        // engine's asset check on reload; only the fixture may wait for it.
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!Test.TestNotNull(TEXT("stock cube"), Cube)) return false;
        FStaticMeshCompilingManager::Get().FinishCompilation({Cube});
        UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, Package, FName(*LogicalName));
        Mesh->SetFlags(RF_Public | RF_Standalone);
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = LogicalName;
        Receipt->SourceRelativePath = LogicalName + TEXT(".mesh.fbx");
        Receipt->SourceHash = MHRawPayloadHash({0x63, 0x6f, 0x6c, 0x64});
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);
        Package->MarkPackageDirty();
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        if (!Test.TestTrue(TEXT("cold mesh package saves"), UPackage::SavePackage(Package, Mesh, *Filename, Args))) return false;
        // Release: no strong references remain, so a forced GC unloads both the
        // mesh and its package; the next resolve finds nothing in memory.
        Mesh->ClearFlags(RF_Public | RF_Standalone);
        Mesh->MarkAsGarbage();
        Package->ClearFlags(RF_Public | RF_Standalone);
        Package->MarkAsGarbage();
        CollectGarbage(RF_NoFlags);
        return Test.TestNull(TEXT("cold mesh is not resident after GC"), FindObject<UStaticMesh>(nullptr, *(PackageName + TEXT(".") + LogicalName)));
    }

    ~FColdMesh()
    {
        if (!Filename.IsEmpty()) IFileManager::Get().Delete(*Filename, false, true, true);
    }

    static FMHResourceKey Key(const FString& Name)
    {
        FMHResourceKey Result;
        Result.Kind = EMHResourceKind::StaticMesh;
        Result.LogicalName = Name;
        return Result;
    }
};

/** Number of plan rows currently rendered by Mesh (pooled or actor-owned). */
int32 CountRowsForMesh(const AMHCompositeActor& Actor, const UStaticMesh* Mesh)
{
    int32 Count = 0;
    for (const FMHCompositeLeafMaterialization& Row : Actor.GetLeafMaterializations())
    {
        const UStaticMeshComponent* Component = Cast<UStaticMeshComponent>(Row.Component.Get());
        if (Component != nullptr && Component->GetStaticMesh() == Mesh) ++Count;
    }
    return Count;
}

} // namespace

// Loading slice A: selected endpoint readiness is an admission barrier, not a
// generated-resource change. The candidate plan is resolved once, repeated
// selected leaf resources are requested once, and no partial/placeholder view
// is published. The complete ready set is materialized once. Unselected options
// are never loaded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAsyncColdEndpointBatchTest,
    "Mimir.V5.Composite.Async.ColdSelectedEndpointsMaterializeOnceWhenAllReady",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAsyncColdEndpointBatchTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (!TestNotNull(TEXT("endpoint registry"), Registry)) return false;

    FRecipeFixture Recipe(*this);
    FColdMesh SelectedA, SelectedB, Unselected;
    if (!SelectedA.Create(*this, Recipe.Name(TEXT("async_cold_selected_a"))) ||
        !SelectedB.Create(*this, Recipe.Name(TEXT("async_cold_selected_b"))) ||
        !Unselected.Create(*this, Recipe.Name(TEXT("async_cold_unselected")))) return false;

    // A, B, A plus random{A w1, unselected w0}: four leaves, two selected
    // endpoint keys and one unselected option.
    FMHCompositeDocument Document;
    for (const FString& Resource : {SelectedA.LogicalName, SelectedB.LogicalName, SelectedA.LogicalName})
    {
        FMHCompositeNode& Mesh = Document.Nodes.AddDefaulted_GetRef();
        Mesh.Kind = EMHCompositeNodeKind::Mesh;
        Mesh.Resource = Resource;
    }
    {
        FMHCompositeNode& Random = Document.Nodes.AddDefaulted_GetRef();
        Random.Kind = EMHCompositeNodeKind::Random;
        FMHCompositeOption& A = Random.Options.AddDefaulted_GetRef();
        A.Kind = EMHCompositeOptionKind::Mesh;
        A.Resource = SelectedA.LogicalName;
        A.Weight = 1.0f;
        FMHCompositeOption& B = Random.Options.AddDefaulted_GetRef();
        B.Kind = EMHCompositeOptionKind::Mesh;
        B.Resource = Unselected.LogicalName;
        B.Weight = 0.0f;
    }
    UMHCompositeAsset* Root = Recipe.Composite(Recipe.Name(TEXT("async_cold_root")), Document, {});
    if (Root == nullptr) return false;
    Registry->InvalidateAll();

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("async world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(true); };
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("async actor"), Actor)) return false;
    Actor->SetAutoSeed(false);
    Actor->SetAutoAppearanceSeed(false);
    Actor->SetSeed(5);
    Actor->SetAppearanceSeed(9);
    UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(World);
    if (!TestNotNull(TEXT("instance pool"), Pool)) return false;

    int32 GeneratedResourceNotifications = 0;
    MHSetGeneratedResourceChangedObserverForTests(
        [&GeneratedResourceNotifications](const FMHResourceKey&) { ++GeneratedResourceNotifications; });
    ON_SCOPE_EXIT { MHSetGeneratedResourceChangedObserverForTests({}); };
    MHResetEndpointResolveMetrics();
    MHResetPlacementStageMetrics();
    Actor->SetCompositeAsset(Root);

    // Pending: the candidate exists only in the actor's private admission state.
    // There is no committed plan, no leaf row and no placeholder pool instance.
    bool bPassed = TestTrue(TEXT("pending batch has no error: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty());
    bPassed &= TestNull(TEXT("pending candidate is not exposed as the resident rendered plan"), Actor->GetResolvedPlan());
    bPassed &= TestTrue(TEXT("no leaf rows are published before the batch is ready"), Actor->GetLeafMaterializations().IsEmpty());
    bPassed &= TestEqual(TEXT("no pool instance is published before the batch is ready"), Pool->NumLiveInstances(*Actor), 0);
    bPassed &= TestEqual(TEXT("cold endpoint performs no synchronous package load"), MHGetEndpointResolveMetrics().PackageLoadsSync, 0ull);
    bPassed &= TestEqual(TEXT("interactive path never waits for mesh compilation"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::WaitStaticMeshCompilation).Calls, 0ull);
    bPassed &= TestEqual(TEXT("candidate layout resolves once while endpoints load"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::ResolveCompositePlan).Calls, 1ull);
    bPassed &= TestEqual(TEXT("placement compilation waits behind endpoint readiness"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::CompilePlacement).Calls, 0ull);
    bPassed &= TestTrue(TEXT("selected cold endpoint A is Loading"),
        Registry->Resolve(FColdMesh::Key(SelectedA.LogicalName)).State == EMHEndpointState::Loading);
    bPassed &= TestTrue(TEXT("selected cold endpoint B is Loading"),
        Registry->Resolve(FColdMesh::Key(SelectedB.LogicalName)).State == EMHEndpointState::Loading);
    bPassed &= TestFalse(TEXT("unselected variant is never resolved"), Registry->HasPrototype(FColdMesh::Key(Unselected.LogicalName)));
    const uint32 RebuildsBeforeReady = Actor->GetPlacementRebuildCount();

    // Completion is a distinct readiness signal: it resumes the cached
    // candidate exactly once and never enters the reimport/proof funnel.
    bPassed &= TestTrue(TEXT("async loads flush"), Registry->FlushAsyncLoadsForTests());
    const FMHEndpointPrototype& ReadyA = Registry->Resolve(FColdMesh::Key(SelectedA.LogicalName));
    const FMHEndpointPrototype& ReadyB = Registry->Resolve(FColdMesh::Key(SelectedB.LogicalName));
    bPassed &= TestTrue(TEXT("selected endpoint A is Ready after the flush"), ReadyA.State == EMHEndpointState::Ready);
    bPassed &= TestTrue(TEXT("selected endpoint B is Ready after the flush"), ReadyB.State == EMHEndpointState::Ready);
    bPassed &= TestEqual(TEXT("still no synchronous package load"), MHGetEndpointResolveMetrics().PackageLoadsSync, 0ull);
    bPassed &= TestNotNull(TEXT("ready materialization publishes the resident plan"), Actor->GetResolvedPlan());
    bPassed &= TestEqual(TEXT("all repeated selected leaves materialize together"), Actor->GetLeafMaterializations().Num(), 4);
    bPassed &= TestEqual(TEXT("mesh A materializes for its three selected occurrences"),
        CountRowsForMesh(*Actor, Cast<UStaticMesh>(ReadyA.Object.Get())), 3);
    bPassed &= TestEqual(TEXT("mesh B materializes for its selected occurrence"),
        CountRowsForMesh(*Actor, Cast<UStaticMesh>(ReadyB.Object.Get())), 1);
    bPassed &= TestEqual(TEXT("completion does not rerun candidate layout"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::ResolveCompositePlan).Calls, 1ull);
    bPassed &= TestEqual(TEXT("the complete batch is compiled once"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::CompilePlacement).Calls, 1ull);
    bPassed &= TestEqual(TEXT("readiness does not enter actor rebuild"), Actor->GetPlacementRebuildCount(), RebuildsBeforeReady);
    bPassed &= TestEqual(TEXT("ordinary readiness emits no generated-resource change"), GeneratedResourceNotifications, 0);
    bPassed &= TestTrue(TEXT("no error after ready materialization: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty());
    bPassed &= TestFalse(TEXT("unselected variant stays unloaded"), FindObject<UStaticMesh>(nullptr, *(Unselected.PackageName + TEXT(".") + Unselected.LogicalName)) != nullptr);
    return bPassed;
}

// Loading slice A update rule: changing a live placement to a cold selected
// endpoint keeps the committed row and its stable pool handle until the whole
// candidate is ready. The ready callback publishes one replacement view.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAsyncUpdateRetainsCommittedViewTest,
    "Mimir.V5.Composite.Async.UpdateRetainsCommittedViewUntilReady",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAsyncUpdateRetainsCommittedViewTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (!TestNotNull(TEXT("endpoint registry"), Registry)) return false;

    FRecipeFixture Recipe(*this);
    FColdMesh Cold;
    if (!Cold.Create(*this, Recipe.Name(TEXT("async_update_cold")))) return false;
    UStaticMesh* OldMesh = Recipe.Mesh(Recipe.Name(TEXT("async_update_old")));
    if (!TestNotNull(TEXT("resident old mesh"), OldMesh)) return false;

    FMHCompositeDocument OldDocument;
    FMHCompositeNode& OldNode = OldDocument.Nodes.AddDefaulted_GetRef();
    OldNode.Kind = EMHCompositeNodeKind::Mesh;
    OldNode.Resource = OldMesh->GetName();
    UMHCompositeAsset* OldRoot = Recipe.Composite(Recipe.Name(TEXT("async_update_old_root")), OldDocument, {});

    FMHCompositeDocument ColdDocument;
    FMHCompositeNode& ColdNode = ColdDocument.Nodes.AddDefaulted_GetRef();
    ColdNode.Kind = EMHCompositeNodeKind::Mesh;
    ColdNode.Resource = Cold.LogicalName;
    UMHCompositeAsset* ColdRoot = Recipe.Composite(Recipe.Name(TEXT("async_update_cold_root")), ColdDocument, {});
    if (OldRoot == nullptr || ColdRoot == nullptr) return false;
    Registry->InvalidateAll();

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("async update world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(true); };
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(World);
    if (!TestNotNull(TEXT("async update actor"), Actor) || !TestNotNull(TEXT("instance pool"), Pool)) return false;
    Actor->SetAutoSeed(false);
    Actor->SetAutoAppearanceSeed(false);
    Actor->SetSeed(11);
    Actor->SetAppearanceSeed(17);
    Actor->SetCompositeAsset(OldRoot);
    if (!TestEqual(TEXT("old view has one row"), Actor->GetLeafMaterializations().Num(), 1)) return false;

    const FMHCompositeLeafMaterialization OldRow = Actor->GetLeafMaterializations()[0];
    UInstancedStaticMeshComponent* OldBucket = Cast<UInstancedStaticMeshComponent>(OldRow.Component.Get());
    if (!TestTrue(TEXT("old row is a live pooled mesh"),
            OldBucket != nullptr && OldBucket->GetStaticMesh() == OldMesh && OldRow.Handle.IsSet() && Pool->IsValidHandle(OldRow.Handle))) return false;
    const uint32 RevisionBeforeUpdate = Actor->GetPreviewRevision();

    int32 GeneratedResourceNotifications = 0;
    MHSetGeneratedResourceChangedObserverForTests(
        [&GeneratedResourceNotifications](const FMHResourceKey&) { ++GeneratedResourceNotifications; });
    ON_SCOPE_EXIT { MHSetGeneratedResourceChangedObserverForTests({}); };
    MHResetEndpointResolveMetrics();
    MHResetPlacementStageMetrics();
    Actor->SetCompositeAsset(ColdRoot);

    bool bPassed = TestTrue(TEXT("cold update has no error: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty());
    bPassed &= TestTrue(TEXT("new endpoint is Loading"),
        Registry->Resolve(FColdMesh::Key(Cold.LogicalName)).State == EMHEndpointState::Loading);
    bPassed &= TestEqual(TEXT("committed row remains while update loads"), Actor->GetLeafMaterializations().Num(), 1);
    if (Actor->GetLeafMaterializations().Num() == 1)
    {
        const FMHCompositeLeafMaterialization& PendingRow = Actor->GetLeafMaterializations()[0];
        bPassed &= TestTrue(TEXT("committed handle remains identical while update loads"), PendingRow.Handle == OldRow.Handle);
        bPassed &= TestEqual(TEXT("committed bucket remains identical while update loads"), PendingRow.Component.Get(), OldRow.Component.Get());
    }
    bPassed &= TestTrue(TEXT("committed handle remains valid while update loads"), Pool->IsValidHandle(OldRow.Handle));
    bPassed &= TestEqual(TEXT("preview revision does not advance for a pending update"), Actor->GetPreviewRevision(), RevisionBeforeUpdate);
    bPassed &= TestEqual(TEXT("updated candidate resolves once"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::ResolveCompositePlan).Calls, 1ull);
    bPassed &= TestEqual(TEXT("updated placement is not compiled before readiness"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::CompilePlacement).Calls, 0ull);

    // The retained view remains the actor's visible committed representation,
    // so it follows ordinary basis changes while the new candidate is pending.
    const FVector MovedLocation(250.0, 75.0, 30.0);
    Actor->SetActorLocation(MovedLocation);
    FTransform RetainedWorld;
    bPassed &= TestTrue(TEXT("retained row follows an actor move while update loads"),
        OldBucket->GetInstanceTransform(OldRow.InstanceIndex, RetainedWorld, true) &&
        RetainedWorld.GetLocation().Equals(MovedLocation, 1e-3));

    // A map can be saved before the new dependency finishes streaming.
    Actor->GetOutermost()->SetDirtyFlag(false);
    bPassed &= TestTrue(TEXT("async update flushes"), Registry->FlushAsyncLoadsForTests());
    bPassed &= TestTrue(TEXT("late dependency commit remains saveable"), Actor->GetOutermost()->IsDirty());
    const FMHEndpointPrototype& Ready = Registry->Resolve(FColdMesh::Key(Cold.LogicalName));
    UStaticMesh* ReadyMesh = Cast<UStaticMesh>(Ready.Object.Get());
    bPassed &= TestTrue(TEXT("cold update endpoint is Ready"), Ready.State == EMHEndpointState::Ready && ReadyMesh != nullptr);
    bPassed &= TestEqual(TEXT("updated view has one row"), Actor->GetLeafMaterializations().Num(), 1);
    bPassed &= TestEqual(TEXT("ready update renders the new mesh"), CountRowsForMesh(*Actor, ReadyMesh), 1);
    if (Actor->GetLeafMaterializations().Num() == 1)
    {
        const FMHCompositeLeafMaterialization& ReadyRow = Actor->GetLeafMaterializations()[0];
        UInstancedStaticMeshComponent* ReadyBucket = Cast<UInstancedStaticMeshComponent>(ReadyRow.Component.Get());
        FTransform ReadyWorld;
        bPassed &= TestTrue(TEXT("ready commit uses the actor's current basis without resolving layout again"),
            ReadyBucket != nullptr && ReadyBucket->GetInstanceTransform(ReadyRow.InstanceIndex, ReadyWorld, true) &&
            ReadyWorld.GetLocation().Equals(MovedLocation, 1e-3));
    }
    bPassed &= TestFalse(TEXT("old handle retires only after ready commit"), Pool->IsValidHandle(OldRow.Handle));
    bPassed &= TestEqual(TEXT("ready update advances preview exactly once"), Actor->GetPreviewRevision(), RevisionBeforeUpdate + 1u);
    bPassed &= TestEqual(TEXT("ready update does not rerun layout"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::ResolveCompositePlan).Calls, 1ull);
    bPassed &= TestEqual(TEXT("ready update compiles one complete placement"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::CompilePlacement).Calls, 1ull);
    bPassed &= TestEqual(TEXT("ordinary update readiness emits no generated-resource change"), GeneratedResourceNotifications, 0);
    bPassed &= TestEqual(TEXT("update performs no synchronous package load"), MHGetEndpointResolveMetrics().PackageLoadsSync, 0ull);
    return bPassed;
}

// Replacing a pending request invalidates its candidate. Completion of the old
// key may make that shared registry prototype Ready, but it cannot publish the
// old plan. Destroying another pending actor similarly leaves no late pool row.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAsyncObsoletePendingCandidateTest,
    "Mimir.V5.Composite.Async.ObsoletePendingCandidateCannotCommit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAsyncObsoletePendingCandidateTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (!TestNotNull(TEXT("endpoint registry"), Registry)) return false;

    FRecipeFixture Recipe(*this);
    FColdMesh ColdA, ColdB, ColdDestroyed;
    if (!ColdA.Create(*this, Recipe.Name(TEXT("async_stale_a"))) ||
        !ColdB.Create(*this, Recipe.Name(TEXT("async_stale_b"))) ||
        !ColdDestroyed.Create(*this, Recipe.Name(TEXT("async_stale_destroyed")))) return false;
    const auto MakeRoot = [&](const FString& RootStem, const FString& MeshName) -> UMHCompositeAsset*
    {
        FMHCompositeDocument Document;
        FMHCompositeNode& Node = Document.Nodes.AddDefaulted_GetRef();
        Node.Kind = EMHCompositeNodeKind::Mesh;
        Node.Resource = MeshName;
        return Recipe.Composite(Recipe.Name(RootStem), Document, {});
    };
    UMHCompositeAsset* RootA = MakeRoot(TEXT("async_stale_root_a"), ColdA.LogicalName);
    UMHCompositeAsset* RootB = MakeRoot(TEXT("async_stale_root_b"), ColdB.LogicalName);
    UMHCompositeAsset* RootDestroyed = MakeRoot(TEXT("async_stale_root_destroyed"), ColdDestroyed.LogicalName);
    if (RootA == nullptr || RootB == nullptr || RootDestroyed == nullptr) return false;
    Registry->InvalidateAll();

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("stale candidate world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(true); };
    UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(World);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    AMHCompositeActor* DestroyedActor = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("instance pool"), Pool) || !TestNotNull(TEXT("stale candidate actor"), Actor) ||
        !TestNotNull(TEXT("destroyed pending actor"), DestroyedActor)) return false;
    for (AMHCompositeActor* Entry : {Actor, DestroyedActor})
    {
        Entry->SetAutoSeed(false);
        Entry->SetAutoAppearanceSeed(false);
        Entry->SetSeed(23);
        Entry->SetAppearanceSeed(29);
    }

    int32 GeneratedResourceNotifications = 0;
    MHSetGeneratedResourceChangedObserverForTests(
        [&GeneratedResourceNotifications](const FMHResourceKey&) { ++GeneratedResourceNotifications; });
    ON_SCOPE_EXIT { MHSetGeneratedResourceChangedObserverForTests({}); };
    MHResetPlacementStageMetrics();
    Actor->SetCompositeAsset(RootA);
    bool bPassed = TestTrue(TEXT("first candidate waits for A"), Actor->IsPreviewLoading());
    Actor->SetCompositeAsset(RootB);
    bPassed &= TestTrue(TEXT("replacement candidate waits for B"), Actor->IsPreviewLoading());
    DestroyedActor->SetCompositeAsset(RootDestroyed);
    bPassed &= TestTrue(TEXT("second actor has a pending candidate before destroy"), DestroyedActor->IsPreviewLoading());
    DestroyedActor->Destroy();

    bPassed &= TestTrue(TEXT("all obsolete/current loads flush"), Registry->FlushAsyncLoadsForTests());
    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    bPassed &= TestTrue(TEXT("only the latest root B plan commits"),
        Plan != nullptr && Plan->Leaves.Num() == 1 && Plan->Leaves[0].Resource == ColdB.LogicalName);
    const FMHEndpointPrototype& ReadyA = Registry->Resolve(FColdMesh::Key(ColdA.LogicalName));
    const FMHEndpointPrototype& ReadyB = Registry->Resolve(FColdMesh::Key(ColdB.LogicalName));
    bPassed &= TestEqual(TEXT("obsolete ready mesh A has no rendered row"),
        CountRowsForMesh(*Actor, Cast<UStaticMesh>(ReadyA.Object.Get())), 0);
    bPassed &= TestEqual(TEXT("latest ready mesh B has one rendered row"),
        CountRowsForMesh(*Actor, Cast<UStaticMesh>(ReadyB.Object.Get())), 1);
    bPassed &= TestEqual(TEXT("only the latest candidate is compiled"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::CompilePlacement).Calls, 1ull);
    bPassed &= TestEqual(TEXT("the two replacement candidates resolve once each"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::ResolveCompositePlan).Calls, 3ull);
    bPassed &= TestEqual(TEXT("destroyed pending actor receives no late pooled row"), Pool->NumLiveInstances(*DestroyedActor), 0);
    bPassed &= TestEqual(TEXT("ordinary readiness emits no generated-resource notification"), GeneratedResourceNotifications, 0);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
