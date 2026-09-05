#include "MHRecipeTestFixture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHInstancePool.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Root = [mesh A] [mesh A, shifted] [mesh B]; editor world with an instance pool. */
struct FPoolPlacementFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    UMHInstancePoolSubsystem* Pool = nullptr;
    UMHCompositeAsset* Root = nullptr;
    /** Same leaves, mesh B first: the first row belongs to a mesh a reimport of A does not touch. */
    UMHCompositeAsset* RootBFirst = nullptr;
    FString MeshA, MeshB;
    UStaticMesh* MeshObjectA = nullptr;
    UStaticMesh* MeshObjectB = nullptr;

    explicit FPoolPlacementFixture(FAutomationTestBase& Test) : Recipe(Test) {}
    ~FPoolPlacementFixture()
    {
        if (GEditor != nullptr && GEditor->Trans != nullptr) GEditor->Trans->Reset(INVTEXT("MH pool test teardown"));
        if (World != nullptr) World->DestroyWorld(true);
    }

    bool Build(FAutomationTestBase& Test)
    {
        MeshA = Recipe.Name(TEXT("pool_place_a"));
        MeshB = Recipe.Name(TEXT("pool_place_b"));
        MeshObjectA = Recipe.Mesh(MeshA);
        MeshObjectB = Recipe.Mesh(MeshB);
        FMHCompositeDocument Document;
        for (int32 Index = 0; Index < 3; ++Index)
        {
            FMHCompositeNode& Node = Document.Nodes.AddDefaulted_GetRef();
            Node.Kind = EMHCompositeNodeKind::Mesh;
            Node.Resource = Index < 2 ? MeshA : MeshB;
            Node.Transform.TranslationCm = FVector(Index * 100.0, 0.0, 0.0);
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("pool_place_root")), Document, {});
        if (Root == nullptr) return false;
        FMHCompositeDocument BFirst;
        for (int32 Index = 0; Index < 3; ++Index)
        {
            FMHCompositeNode& Node = BFirst.Nodes.AddDefaulted_GetRef();
            Node.Kind = EMHCompositeNodeKind::Mesh;
            Node.Resource = Index == 0 ? MeshB : MeshA;
            Node.Transform.TranslationCm = FVector(Index * 100.0, 0.0, 0.0);
        }
        RootBFirst = Recipe.Composite(Recipe.Name(TEXT("pool_place_root_bfirst")), BFirst, {});
        if (RootBFirst == nullptr) return false;
        World = UWorld::CreateWorld(EWorldType::Editor, false);
        if (!Test.TestNotNull(TEXT("pool placement world"), World)) return false;
        Pool = UMHInstancePoolSubsystem::Get(World);
        return Test.TestNotNull(TEXT("instance pool of the editor world"), Pool);
    }

    AMHCompositeActor* Spawn(const FVector& Location, const int32 Seed = 3, UMHCompositeAsset* Asset = nullptr)
    {
        FActorSpawnParameters Params;
        Params.ObjectFlags = RF_Transactional;
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), FTransform(Location), Params);
        if (Actor == nullptr) return nullptr;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(Seed);
        Actor->SetAppearanceSeed(5);
        Actor->SetCompositeAsset(Asset != nullptr ? Asset : Root);
        return Actor;
    }

    static FMHResourceKey Key(const FString& Name)
    {
        FMHResourceKey Result;
        Result.Kind = EMHResourceKind::StaticMesh;
        Result.LogicalName = Name;
        return Result;
    }

    UInstancedStaticMeshComponent* Bucket(const UStaticMesh& Mesh) const
    {
        TArray<UInstancedStaticMeshComponent*> Components;
        Pool->GetBucketComponents(Mesh, Components);
        return Components.Num() == 1 ? Components[0] : nullptr;
    }

    static int32 ActorOwnedISM(const AMHCompositeActor& Actor)
    {
        int32 Count = 0;
        for (const UActorComponent* Component : Actor.GetDerivedComponents())
            if (Cast<UInstancedStaticMeshComponent>(Component) != nullptr) ++Count;
        return Count;
    }

    /** Every mesh row of Actor is a valid pool instance rendering the plan's world matrix under the pool's bucket. */
    bool RowsAreLive(FAutomationTestBase& Test, const AMHCompositeActor& Actor, const TCHAR* Phase) const
    {
        const FMHResolvedCompositePlan* Plan = Actor.GetResolvedPlan();
        const TArray<FMHCompositeLeafMaterialization>& Rows = Actor.GetLeafMaterializations();
        if (!Test.TestNotNull(FString(Phase) + TEXT(": resident plan"), Plan) ||
            !Test.TestEqual(FString(Phase) + TEXT(": rows are plan-aligned"), Rows.Num(), Plan->Leaves.Num())) return false;
        const FMatrix Basis = Actor.GetActorTransform().ToMatrixWithScale();
        bool bPassed = true;
        for (int32 Index = 0; Index < Rows.Num(); ++Index)
        {
            const FMHCompositeLeafMaterialization& Row = Rows[Index];
            const FMHResolvedCompositeLeaf& Leaf = Plan->Leaves[Index];
            if (Leaf.Kind != EMHRandomSemanticKind::Mesh) continue;
            const FString Where = FString::Printf(TEXT("%s: row %d"), Phase, Index);
            bPassed &= Test.TestTrue(Where + TEXT(" carries a pool handle"), Row.Handle.IsSet() && Pool->IsValidHandle(Row.Handle));
            bPassed &= Test.TestTrue(Where + TEXT(" is instanced"), Row.IsInstanced());
            UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
            bPassed &= Test.TestTrue(Where + TEXT(" renders in a pool bucket"), Bucket != nullptr && Bucket->GetOwner() != nullptr && Bucket->GetOwner()->IsA<AMHInstancePoolActor>());
            if (Bucket == nullptr) continue;
            FTransform Instance;
            bPassed &= Test.TestTrue(Where + TEXT(" instance index is live"), Bucket->GetInstanceTransform(Row.InstanceIndex, Instance, true));
            bPassed &= Test.TestTrue(Where + TEXT(" renders the plan world matrix"),
                Instance.GetLocation().Equals(FTransform(Leaf.WorldMatrix * Basis).GetLocation(), 1e-2));
            AActor* Owner = nullptr;
            FString Path;
            bPassed &= Test.TestTrue(Where + TEXT(" reverse lookup names the actor and node path"),
                Pool->ReverseLookup(Bucket, Row.InstanceIndex, Owner, Path) && Owner == &Actor && Path == Row.NodePath);
            const FMHCompositeLeafMaterialization* Found = Actor.FindLeafMaterialization(Bucket, Row.InstanceIndex);
            bPassed &= Test.TestTrue(Where + TEXT(" FindLeafMaterialization resolves through the pool"), Found != nullptr && Found->NodePath == Row.NodePath);
        }
        return bPassed;
    }
};

} // namespace

// KICKOFF §5 R5 / 16 §2.8: static leaves materialize through the level's
// instance pool. Two placements of one mesh share one bucket on the pool
// actor; the composite actor owns no ISM; rows carry stable handles; moving,
// hiding, reseeding and destroying one placement never disturbs the other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPoolPlacementMaterializesThroughPoolTest,
    "Mimir.V5.Composite.Pool.PlacementMaterializesThroughPool",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPoolPlacementMaterializesThroughPoolTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolPlacementFixture F(*this);
    if (!F.Build(*this)) return false;
    AMHCompositeActor* A = F.Spawn(FVector(0, 0, 0));
    AMHCompositeActor* B = F.Spawn(FVector(0, 1000, 0));
    if (!TestNotNull(TEXT("actor A"), A) || !TestNotNull(TEXT("actor B"), B)) return false;
    bool bPassed = TestTrue(TEXT("A previews: ") + A->GetLastPlacementError(), A->GetLastPlacementError().IsEmpty());
    bPassed &= TestTrue(TEXT("B previews: ") + B->GetLastPlacementError(), B->GetLastPlacementError().IsEmpty());

    UInstancedStaticMeshComponent* BucketA = F.Bucket(*F.MeshObjectA);
    UInstancedStaticMeshComponent* BucketB = F.Bucket(*F.MeshObjectB);
    bPassed &= TestNotNull(TEXT("one pool bucket renders mesh A for both placements"), BucketA);
    bPassed &= TestNotNull(TEXT("one pool bucket renders mesh B"), BucketB);
    bPassed &= TestEqual(TEXT("bucket A holds both placements' instances"), BucketA != nullptr ? BucketA->GetInstanceCount() : -1, 4);
    bPassed &= TestEqual(TEXT("bucket B holds both placements' instances"), BucketB != nullptr ? BucketB->GetInstanceCount() : -1, 2);
    bPassed &= TestEqual(TEXT("A owns no ISM bucket"), FPoolPlacementFixture::ActorOwnedISM(*A), 0);
    bPassed &= TestEqual(TEXT("B owns no ISM bucket"), FPoolPlacementFixture::ActorOwnedISM(*B), 0);
    bPassed &= F.RowsAreLive(*this, *A, TEXT("initial A"));
    bPassed &= F.RowsAreLive(*this, *B, TEXT("initial B"));
    const int32 Buckets = F.Pool->NumBuckets();

    // Moving A moves only A's instances, inside the same buckets.
    A->SetActorLocation(FVector(0, 0, 250));
    bPassed &= F.RowsAreLive(*this, *A, TEXT("moved A"));
    bPassed &= F.RowsAreLive(*this, *B, TEXT("B after A moved"));
    bPassed &= TestEqual(TEXT("moving creates no bucket"), F.Pool->NumBuckets(), Buckets);
    bPassed &= TestTrue(TEXT("bucket A keeps its component through the move"), F.Bucket(*F.MeshObjectA) == BucketA);

    // Hiding A in the editor hides only A's instances; B stays rendered.
    A->SetIsTemporarilyHiddenInEditor(true);
    bPassed &= TestEqual(TEXT("hidden A has no live instances"), F.Pool->NumLiveInstances(*A), 0);
    bPassed &= TestEqual(TEXT("bucket A renders B's instances while A is hidden"), BucketA != nullptr ? BucketA->GetInstanceCount() : -1, 2);
    bPassed &= F.RowsAreLive(*this, *B, TEXT("B while A hidden"));
    A->SetIsTemporarilyHiddenInEditor(false);
    bPassed &= TestEqual(TEXT("shown A is live again"), F.Pool->NumLiveInstances(*A), 3);
    bPassed &= F.RowsAreLive(*this, *A, TEXT("shown A"));

    // Reseed A: the same buckets serve the new layout.
    A->SetSeed(A->GetSeed() + 1);
    bPassed &= F.RowsAreLive(*this, *A, TEXT("reseeded A"));
    bPassed &= TestTrue(TEXT("bucket A keeps its component through the reseed"), F.Bucket(*F.MeshObjectA) == BucketA);
    bPassed &= TestEqual(TEXT("reseed creates no bucket"), F.Pool->NumBuckets(), Buckets);

    // Destroying A frees only A's instances.
    A->Destroy();
    bPassed &= TestEqual(TEXT("bucket A renders only B after A is destroyed"), BucketA != nullptr ? BucketA->GetInstanceCount() : -1, 2);
    bPassed &= TestEqual(TEXT("bucket B renders only B after A is destroyed"), BucketB != nullptr ? BucketB->GetInstanceCount() : -1, 1);
    bPassed &= F.RowsAreLive(*this, *B, TEXT("B after A destroyed"));
    return bPassed;
}

// OPEN-R-1 / KICKOFF §5 R5 "undo восстанавливает": only the composite actor
// is transactional; the pool restores its materialization from the actor's
// record in PostEditUndo. No duplicate instances, same handles semantics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPoolUndoRestoresPooledPlacementTest,
    "Mimir.V5.Composite.Pool.UndoRestoresPooledPlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPoolUndoRestoresPooledPlacementTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (GEditor == nullptr || GEditor->Trans == nullptr) return false;
    FPoolPlacementFixture F(*this);
    if (!F.Build(*this)) return false;
    GEditor->Trans->Reset(INVTEXT("MH pool undo test start"));
    AMHCompositeActor* A = F.Spawn(FVector(0, 0, 0));
    AMHCompositeActor* B = F.Spawn(FVector(0, 1000, 0));
    if (!TestNotNull(TEXT("actor A"), A) || !TestNotNull(TEXT("actor B"), B)) return false;
    bool bPassed = F.RowsAreLive(*this, *A, TEXT("initial A"));
    UInstancedStaticMeshComponent* BucketA = F.Bucket(*F.MeshObjectA);
    const int32 InstancesBefore = BucketA != nullptr ? BucketA->GetInstanceCount() : -1;
    TMap<FString, FVector> LocationsBefore;
    for (const FMHCompositeLeafMaterialization& Row : A->GetLeafMaterializations())
    {
        FTransform T;
        if (UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
            Bucket != nullptr && Bucket->GetInstanceTransform(Row.InstanceIndex, T, true))
            LocationsBefore.Add(Row.NodePath, T.GetLocation());
    }
    bPassed &= TestEqual(TEXT("every A row has a location"), LocationsBefore.Num(), 3);

    GEditor->BeginTransaction(INVTEXT("MH pool test move"));
    A->Modify();
    A->SetActorLocation(FVector(0, 0, 300));
    GEditor->EndTransaction();
    bPassed &= F.RowsAreLive(*this, *A, TEXT("moved A"));
    bPassed &= TestTrue(TEXT("undo of the move succeeds"), GEditor->UndoTransaction());
    bPassed &= TestTrue(TEXT("actor location restored"), A->GetActorLocation().Equals(FVector::ZeroVector, 1e-3));
    bPassed &= F.RowsAreLive(*this, *A, TEXT("A after undo"));
    bPassed &= TestEqual(TEXT("no duplicate instances after undo"), F.Bucket(*F.MeshObjectA) != nullptr ? F.Bucket(*F.MeshObjectA)->GetInstanceCount() : -1, InstancesBefore);
    bPassed &= TestEqual(TEXT("A renders its three leaves after undo"), F.Pool->NumLiveInstances(*A), 3);
    for (const FMHCompositeLeafMaterialization& Row : A->GetLeafMaterializations())
    {
        FTransform T;
        UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
        const FVector* Before = LocationsBefore.Find(Row.NodePath);
        bPassed &= TestTrue(TEXT("undo restores the instance location of ") + Row.NodePath,
            Before != nullptr && Bucket != nullptr && Bucket->GetInstanceTransform(Row.InstanceIndex, T, true) && T.GetLocation().Equals(*Before, 1e-2));
    }
    bPassed &= F.RowsAreLive(*this, *B, TEXT("B after A's undo"));
    return bPassed;
}

// 16 §4 row 3 on the pool: a bucket-descriptor reimport migrates the shared
// bucket once for every placement of the level, no placement rebuild; both
// actors' rows follow the migrated component.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPoolReimportMigratesSharedBucketOnceTest,
    "Mimir.V5.Composite.Pool.ReimportMigratesSharedBucketOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPoolReimportMigratesSharedBucketOnceTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolPlacementFixture F(*this);
    if (!F.Build(*this)) return false;
    AMHCompositeActor* A = F.Spawn(FVector(0, 0, 0));
    AMHCompositeActor* B = F.Spawn(FVector(0, 1000, 0));
    if (!TestNotNull(TEXT("actor A"), A) || !TestNotNull(TEXT("actor B"), B)) return false;
    UInstancedStaticMeshComponent* OldA = F.Bucket(*F.MeshObjectA);
    UInstancedStaticMeshComponent* OldB = F.Bucket(*F.MeshObjectB);
    bool bPassed = TestNotNull(TEXT("bucket A"), OldA) && TestNotNull(TEXT("bucket B"), OldB);
    const uint32 RebuildsA = A->GetPlacementRebuildCount();
    const uint32 RebuildsB = B->GetPlacementRebuildCount();
    F.Pool->ResetMetricsForTests();

    UMaterial* Slot1 = NewObject<UMaterial>(GetTransientPackage(), FName(*F.Recipe.Name(TEXT("pool_slot1"))));
    F.MeshObjectA->GetStaticMaterials().Add(FStaticMaterial(Slot1, TEXT("slot1"), TEXT("slot1")));
    MHNotifyGeneratedResourceChanged(FPoolPlacementFixture::Key(F.MeshA));
    const FMHEndpointInterfaceDelta Delta = UMHEndpointPrototypeRegistry::Get()->GetLastInterfaceDelta(FPoolPlacementFixture::Key(F.MeshA));
    bPassed &= TestTrue(TEXT("registry classified a descriptor change"), Delta.bBucketDescriptor);
    bPassed &= TestEqual(TEXT("the shared bucket migrates exactly once"), F.Pool->GetMetrics().BucketsMigrated, 1ull);
    bPassed &= TestEqual(TEXT("A is not rebuilt"), A->GetPlacementRebuildCount(), RebuildsA);
    bPassed &= TestEqual(TEXT("B is not rebuilt"), B->GetPlacementRebuildCount(), RebuildsB);
    UInstancedStaticMeshComponent* NewA = F.Bucket(*F.MeshObjectA);
    bPassed &= TestTrue(TEXT("mesh A has a new bucket component"), NewA != nullptr && NewA != OldA);
    bPassed &= TestFalse(TEXT("the old bucket is destroyed"), IsValid(OldA));
    bPassed &= TestTrue(TEXT("mesh B keeps its bucket"), F.Bucket(*F.MeshObjectB) == OldB);
    bPassed &= TestEqual(TEXT("migrated bucket renders both placements"), NewA != nullptr ? NewA->GetInstanceCount() : -1, 4);
    bPassed &= F.RowsAreLive(*this, *A, TEXT("A after migration"));
    bPassed &= F.RowsAreLive(*this, *B, TEXT("B after migration"));
    return bPassed;
}

// R5-F (audit 2026-09-05 §2.1): after a bucket migration every derived view of
// the placement (rows and the compatibility leaf array) must agree, so the next
// move is a plain basis update: no MH_E_PLACEMENT_STATE_DESYNC, no full
// rebuild. Covers the two orders that broke the one-shot refresh: the first
// row on the migrated mesh, and the first row on an unaffected mesh.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPoolReimportThenMoveTest,
    "Mimir.V5.Composite.Pool.ReimportThenMovePreservesMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPoolReimportThenMoveTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolPlacementFixture F(*this);
    if (!F.Build(*this)) return false;
    AMHCompositeActor* A = F.Spawn(FVector(0, 0, 0));
    AMHCompositeActor* B = F.Spawn(FVector(0, 1000, 0), 3, F.RootBFirst);
    if (!TestNotNull(TEXT("actor A"), A) || !TestNotNull(TEXT("actor B (mesh B first)"), B)) return false;
    bool bPassed = F.RowsAreLive(*this, *A, TEXT("initial A")) & F.RowsAreLive(*this, *B, TEXT("initial B"));
    const uint32 RebuildsA = A->GetPlacementRebuildCount();
    const uint32 RebuildsB = B->GetPlacementRebuildCount();
    const uint32 DesyncsA = A->GetPlacementDesyncCount();
    const uint32 DesyncsB = B->GetPlacementDesyncCount();
    const uint32 RevisionA = A->GetPreviewRevision();
    const uint32 RevisionB = B->GetPreviewRevision();

    UMaterial* Slot1 = NewObject<UMaterial>(GetTransientPackage(), FName(*F.Recipe.Name(TEXT("pool_move_slot1"))));
    F.MeshObjectA->GetStaticMaterials().Add(FStaticMaterial(Slot1, TEXT("slot1"), TEXT("slot1")));
    MHNotifyGeneratedResourceChanged(FPoolPlacementFixture::Key(F.MeshA));
    bPassed &= TestTrue(TEXT("registry classified a descriptor change"),
        UMHEndpointPrototypeRegistry::Get()->GetLastInterfaceDelta(FPoolPlacementFixture::Key(F.MeshA)).bBucketDescriptor);
    bPassed &= TestTrue(TEXT("A observed the migration (preview revision advanced)"), A->GetPreviewRevision() > RevisionA);
    bPassed &= TestTrue(TEXT("B observed the migration although its first leaf is mesh B"), B->GetPreviewRevision() > RevisionB);
    // Compatibility view agrees with the rows for every leaf.
    for (AMHCompositeActor* Actor : {A, B})
    {
        const TArray<FMHCompositeLeafMaterialization>& Rows = Actor->GetLeafMaterializations();
        const TArray<TObjectPtr<USceneComponent>>& Leaves = Actor->GetLeafPlacementComponents();
        bPassed &= TestEqual(TEXT("leaf view is plan-aligned"), Leaves.Num(), Rows.Num());
        for (int32 Index = 0; Index < Rows.Num() && Index < Leaves.Num(); ++Index)
            bPassed &= TestTrue(FString::Printf(TEXT("leaf view %d follows the migrated row"), Index), IsValid(Leaves[Index]) && Leaves[Index] == Rows[Index].Component);
    }

    // The move after the reimport is a basis update, nothing else.
    A->SetActorLocation(FVector(0, 0, 100));
    B->SetActorLocation(FVector(0, 1000, 100));
    bPassed &= TestEqual(TEXT("A: move after reimport records no desync"), A->GetPlacementDesyncCount(), DesyncsA);
    bPassed &= TestEqual(TEXT("A: move after reimport is not a rebuild"), A->GetPlacementRebuildCount(), RebuildsA);
    bPassed &= TestEqual(TEXT("B: move after reimport records no desync"), B->GetPlacementDesyncCount(), DesyncsB);
    bPassed &= TestEqual(TEXT("B: move after reimport is not a rebuild"), B->GetPlacementRebuildCount(), RebuildsB);
    bPassed &= F.RowsAreLive(*this, *A, TEXT("A moved after reimport"));
    bPassed &= F.RowsAreLive(*this, *B, TEXT("B moved after reimport"));
    return bPassed;
}

// R5-F (audit §2.3): Undo while Placement Edit Mode is active must not leave
// the placement empty behind a session that blocks its own rebuild. The
// session ends, the preview is restored from the actor's record.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPoolUndoDuringEditTest,
    "Mimir.V5.Composite.Pool.UndoDuringEditRestoresPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPoolUndoDuringEditTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (GEditor == nullptr || GEditor->Trans == nullptr) return false;
    FPoolPlacementFixture F(*this);
    if (!F.Build(*this)) return false;
    GEditor->Trans->Reset(INVTEXT("MH pool undo-in-edit test start"));
    AMHCompositeActor* A = F.Spawn(FVector(0, 0, 0));
    if (!TestNotNull(TEXT("actor A"), A)) return false;
    bool bPassed = F.RowsAreLive(*this, *A, TEXT("initial A"));

    GEditor->BeginTransaction(INVTEXT("MH pool test move before edit"));
    A->Modify();
    A->SetActorLocation(FVector(0, 0, 300));
    GEditor->EndTransaction();
    A->SetPlacementEditMode(true);
    bPassed &= TestTrue(TEXT("edit session is active"), A->IsPlacementEditMode());
    bPassed &= TestTrue(TEXT("undo succeeds during the edit session"), GEditor->UndoTransaction());
    bPassed &= TestFalse(TEXT("undo ends the edit session instead of starving it"), A->IsPlacementEditMode());
    bPassed &= TestTrue(TEXT("actor location restored"), A->GetActorLocation().Equals(FVector::ZeroVector, 1e-3));
    bPassed &= TestNotNull(TEXT("preview restored after undo: ") + A->GetLastPlacementError(), A->GetResolvedPlan());
    bPassed &= TestEqual(TEXT("A renders its three leaves after undo"), F.Pool->NumLiveInstances(*A), 3);
    bPassed &= F.RowsAreLive(*this, *A, TEXT("A after undo during edit"));
    // A fresh session still works on the restored preview.
    A->SetPlacementEditMode(true);
    bPassed &= TestTrue(TEXT("a new edit session starts"), A->IsPlacementEditMode());
    A->SetPlacementEditMode(false);
    bPassed &= F.RowsAreLive(*this, *A, TEXT("A after the new session"));
    return bPassed;
}

// R5b-2a on placements: F / focus frames the placement's pooled instances
// (GetComponentsBoundingBox), and selecting the composite actor in the editor
// highlights only its instances on the shared bucket.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPoolPlacementSelectionAndBoundsTest,
    "Mimir.V5.Composite.Pool.SelectedPlacementHighlightsAndBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPoolPlacementSelectionAndBoundsTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (GEditor == nullptr) return false;
    FPoolPlacementFixture F(*this);
    if (!F.Build(*this)) return false;
    AMHCompositeActor* A = F.Spawn(FVector(0, 0, 0));
    AMHCompositeActor* B = F.Spawn(FVector(0, 5000, 0));
    if (!TestNotNull(TEXT("actor A"), A) || !TestNotNull(TEXT("actor B"), B)) return false;
    bool bPassed = F.RowsAreLive(*this, *A, TEXT("initial A")) & F.RowsAreLive(*this, *B, TEXT("initial B"));

    const FBox BoundsA = A->GetComponentsBoundingBox(true);
    bPassed &= TestTrue(TEXT("A's bounds are valid without own primitives"), BoundsA.IsValid != 0);
    const FMHResolvedCompositePlan* PlanA = A->GetResolvedPlan();
    if (PlanA != nullptr)
    {
        for (const FMHResolvedCompositeLeaf& Leaf : PlanA->Leaves)
            bPassed &= TestTrue(TEXT("A's bounds cover its leaf ") + Leaf.Origin, BoundsA.IsInsideOrOn(FTransform(Leaf.WorldMatrix * A->GetActorTransform().ToMatrixWithScale()).GetLocation()));
    }
    bPassed &= TestFalse(TEXT("A's bounds exclude B's placement"), BoundsA.IsInsideOrOn(B->GetActorLocation()));

    const auto RowsHighlighted = [](const AMHCompositeActor& Actor, const bool bExpected)
    {
        for (const FMHCompositeLeafMaterialization& Row : Actor.GetLeafMaterializations())
        {
            const UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
            if (Bucket == nullptr || Row.InstanceIndex == INDEX_NONE || Bucket->IsInstanceSelected(Row.InstanceIndex) != bExpected) return false;
        }
        return true;
    };
    GEditor->SelectNone(false, true, false);
    GEditor->SelectActor(A, true, true, true);
    bPassed &= TestTrue(TEXT("selecting A highlights A's instances"), RowsHighlighted(*A, true));
    bPassed &= TestTrue(TEXT("selecting A leaves B's instances unhighlighted"), RowsHighlighted(*B, false));
    GEditor->SelectActor(A, false, true, true);
    GEditor->SelectActor(B, true, true, true);
    bPassed &= TestTrue(TEXT("selection moved to B"), RowsHighlighted(*A, false) && RowsHighlighted(*B, true));
    // A silent SelectNone (bNoteSelectionChange = false) suppresses every
    // selection notification by engine contract; the pool mirrors notified
    // changes, as the details panel and outliner do.
    GEditor->SelectNone(true, true, false);
    bPassed &= TestTrue(TEXT("no selection, no highlight"), RowsHighlighted(*A, false) && RowsHighlighted(*B, false));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
