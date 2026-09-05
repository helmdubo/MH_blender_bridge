#include "Composite/MHInstancePool.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "CoreMinimal.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FPoolFixture
{
    UWorld* World = nullptr;
    UMHInstancePoolSubsystem* Pool = nullptr;
    UStaticMesh* MeshA = nullptr;
    UStaticMesh* MeshB = nullptr;
    AActor* OwnerA = nullptr;
    AActor* OwnerB = nullptr;
    FMHPoolBucketDescriptor DescA, DescB;
    float Channels[MH_APPEARANCE_CHANNELS] = {0.1f, 0.2f, 0.3f, 0.4f};

    ~FPoolFixture()
    {
        for (UStaticMesh* Mesh : {MeshA, MeshB})
        {
            if (IsValid(Mesh)) { Mesh->ClearFlags(RF_Public | RF_Standalone); Mesh->MarkAsGarbage(); }
        }
        if (World != nullptr) World->DestroyWorld(true);
    }

    static UStaticMesh* Mesh(const FString& Name)
    {
        UStaticMesh* Result = NewObject<UStaticMesh>(
            CreatePackage(*(TEXT("/Game/MimirCompositeTests/Pool/") + Name)), FName(*Name), RF_Public | RF_Standalone);
        return Result;
    }

    bool Build(FAutomationTestBase& Test)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower().Left(8);
        World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
        if (!Test.TestNotNull(TEXT("pool world"), World)) return false;
        Pool = UMHInstancePoolSubsystem::Get(World);
        if (!Test.TestNotNull(TEXT("pool subsystem exists for an editor world"), Pool)) return false;
        MeshA = Mesh(TEXT("pool_mesh_a_") + Suffix);
        MeshB = Mesh(TEXT("pool_mesh_b_") + Suffix);
        OwnerA = World->SpawnActor<AActor>();
        OwnerB = World->SpawnActor<AActor>();
        DescA = FMHPoolBucketDescriptor::FromMesh(*MeshA, 4, 0);
        DescB = FMHPoolBucketDescriptor::FromMesh(*MeshB, 4, 0);
        return OwnerA != nullptr && OwnerB != nullptr;
    }

    FMHInstanceHandle Add(AActor& Owner, const TCHAR* Path, const FMHPoolBucketDescriptor& Desc, const FVector& Location)
    {
        return Pool->Add(Owner, Path, *World->PersistentLevel, Desc, FTransform(Location).ToMatrixWithScale(), Channels);
    }

    bool Lookup(const FMHInstanceHandle& Handle, AActor*& OutOwner, FString& OutPath, int32& OutIndex) const
    {
        UInstancedStaticMeshComponent* Component = nullptr;
        OutIndex = INDEX_NONE;
        if (!Pool->GetInstance(Handle, Component, OutIndex) || Component == nullptr) return false;
        return Pool->ReverseLookup(Component, OutIndex, OutOwner, OutPath);
    }
};

} // namespace

// KICKOFF §5 R5 / 16 §2.8: removing one instance never breaks the reverse
// lookup of the others (swap-remove keeps both maps in step), handles are
// stable identities, a removed handle is refused, and one bucket serves every
// compatible instance of a level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHInstancePoolHandleStabilityTest,
    "Mimir.V5.Composite.Pool.HandleStability",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHInstancePoolHandleStabilityTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolFixture F;
    if (!F.Build(*this)) return false;
    const FMHInstanceHandle H0 = F.Add(*F.OwnerA, TEXT("a:nodes[0]"), F.DescA, FVector(0, 0, 0));
    const FMHInstanceHandle H1 = F.Add(*F.OwnerB, TEXT("b:nodes[0]"), F.DescA, FVector(100, 0, 0));
    const FMHInstanceHandle H2 = F.Add(*F.OwnerA, TEXT("a:nodes[1]"), F.DescA, FVector(200, 0, 0));
    const FMHInstanceHandle HB = F.Add(*F.OwnerB, TEXT("b:nodes[1]"), F.DescB, FVector(300, 0, 0));
    bool bPassed = TestTrue(TEXT("handles are set"), H0.IsSet() && H1.IsSet() && H2.IsSet() && HB.IsSet());
    bPassed &= TestEqual(TEXT("compatible instances share one bucket, the other mesh gets its own"), F.Pool->NumBuckets(), 2);
    bPassed &= TestEqual(TEXT("same bucket for the same descriptor"), H0.BucketId, H2.BucketId);
    bPassed &= TestNotEqual(TEXT("different descriptor is a different bucket"), H0.BucketId, HB.BucketId);

    UInstancedStaticMeshComponent* Component = nullptr;
    int32 Index = INDEX_NONE;
    bPassed &= TestTrue(TEXT("handle resolves to a live ISM instance"), F.Pool->GetInstance(H1, Component, Index) && Component != nullptr && Index != INDEX_NONE);
    bPassed &= TestTrue(TEXT("bucket component is a transient pool actor's ISM"), Component != nullptr && Component->GetOwner() != nullptr && Component->GetOwner()->IsA<AMHInstancePoolActor>());
    bPassed &= TestEqual(TEXT("bucket A renders three instances"), Component != nullptr ? Component->GetInstanceCount() : -1, 3);

    // Swap-remove the middle instance: the last ISM instance moves into its
    // place; the handles and reverse lookups of the survivors are unchanged.
    bPassed &= TestTrue(TEXT("remove H1"), F.Pool->Remove(H1));
    bPassed &= TestFalse(TEXT("removed handle is no longer valid"), F.Pool->IsValidHandle(H1));
    bPassed &= TestFalse(TEXT("removed handle cannot be updated"), F.Pool->Update(H1, FMatrix::Identity));
    bPassed &= TestEqual(TEXT("bucket A renders two instances after remove"), Component != nullptr ? Component->GetInstanceCount() : -1, 2);
    for (const TPair<FMHInstanceHandle, FString> Expected : {TPair<FMHInstanceHandle, FString>(H0, TEXT("a:nodes[0]")), TPair<FMHInstanceHandle, FString>(H2, TEXT("a:nodes[1]"))})
    {
        AActor* Owner = nullptr;
        FString Path;
        bPassed &= TestTrue(TEXT("survivor reverse lookup succeeds"), F.Lookup(Expected.Key, Owner, Path, Index));
        bPassed &= TestEqual(TEXT("survivor reverse lookup names its owner"), Owner, F.OwnerA);
        bPassed &= TestEqual(TEXT("survivor reverse lookup names its node path"), Path, Expected.Value);
        bPassed &= TestTrue(TEXT("survivor instance index is inside the ISM"), Component != nullptr && Component->IsValidInstance(Index));
    }
    // World transforms survive the swap.
    bPassed &= TestTrue(TEXT("H2 keeps its transform after the swap"), F.Pool->GetInstance(H2, Component, Index) && [&]
    {
        FTransform T;
        return Component->GetInstanceTransform(Index, T, true) && T.GetLocation().Equals(FVector(200, 0, 0), 1e-3);
    }());

    // A new Add reuses the free slot with a new generation: the old handle stays dead.
    const FMHInstanceHandle H3 = F.Add(*F.OwnerB, TEXT("b:nodes[2]"), F.DescA, FVector(400, 0, 0));
    bPassed &= TestTrue(TEXT("new instance is valid"), F.Pool->IsValidHandle(H3));
    bPassed &= TestFalse(TEXT("old handle stays dead after slot reuse"), F.Pool->IsValidHandle(H1));
    bPassed &= TestTrue(TEXT("new handle differs from the dead one"), !(H3 == H1));

    // Update moves the instance without touching identity.
    bPassed &= TestTrue(TEXT("update H0"), F.Pool->Update(H0, FTransform(FVector(0, 50, 0)).ToMatrixWithScale()));
    bPassed &= TestTrue(TEXT("updated instance moved"), F.Pool->GetInstance(H0, Component, Index) && [&]
    {
        FTransform T;
        return Component->GetInstanceTransform(Index, T, true) && T.GetLocation().Equals(FVector(0, 50, 0), 1e-3);
    }());
    return bPassed;
}

// 16 §2.8 owner operations: HideOwner hides only that owner's instances
// (an ISM SetVisibility would hide everyone), ShowOwner restores them under the
// same handles, MoveOwner moves only that owner, RemoveOwner frees its slots.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHInstancePoolOwnerOperationsTest,
    "Mimir.V5.Composite.Pool.OwnerOperations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHInstancePoolOwnerOperationsTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolFixture F;
    if (!F.Build(*this)) return false;
    const FMHInstanceHandle A0 = F.Add(*F.OwnerA, TEXT("a:nodes[0]"), F.DescA, FVector(0, 0, 0));
    const FMHInstanceHandle A1 = F.Add(*F.OwnerA, TEXT("a:nodes[1]"), F.DescA, FVector(10, 0, 0));
    const FMHInstanceHandle B0 = F.Add(*F.OwnerB, TEXT("b:nodes[0]"), F.DescA, FVector(20, 0, 0));
    UInstancedStaticMeshComponent* Component = nullptr;
    int32 Index = INDEX_NONE;
    bool bPassed = TestTrue(TEXT("bucket"), F.Pool->GetInstance(B0, Component, Index) && Component != nullptr);
    bPassed &= TestEqual(TEXT("three instances before hide"), Component != nullptr ? Component->GetInstanceCount() : -1, 3);

    F.Pool->HideOwner(*F.OwnerA);
    bPassed &= TestEqual(TEXT("HideOwner removes only A's instances from the ISM"), Component != nullptr ? Component->GetInstanceCount() : -1, 1);
    bPassed &= TestTrue(TEXT("bucket component stays visible for B"), Component != nullptr && Component->IsVisible());
    bPassed &= TestTrue(TEXT("hidden handles stay valid"), F.Pool->IsValidHandle(A0) && F.Pool->IsValidHandle(A1));
    bPassed &= TestEqual(TEXT("A has no live instances while hidden"), F.Pool->NumLiveInstances(*F.OwnerA), 0);
    AActor* Owner = nullptr;
    FString Path;
    bPassed &= TestTrue(TEXT("B's reverse lookup survives A's hide"), F.Lookup(B0, Owner, Path, Index) && Owner == F.OwnerB && Path == TEXT("b:nodes[0]"));

    F.Pool->ShowOwner(*F.OwnerA);
    bPassed &= TestEqual(TEXT("ShowOwner restores A's instances"), Component != nullptr ? Component->GetInstanceCount() : -1, 3);
    bPassed &= TestTrue(TEXT("A1 keeps its transform through hide/show"), F.Pool->GetInstance(A1, Component, Index) && [&]
    {
        FTransform T;
        return Component->GetInstanceTransform(Index, T, true) && T.GetLocation().Equals(FVector(10, 0, 0), 1e-3);
    }());
    bPassed &= TestTrue(TEXT("A1 reverse lookup after show"), F.Lookup(A1, Owner, Path, Index) && Owner == F.OwnerA && Path == TEXT("a:nodes[1]"));

    F.Pool->MoveOwner(*F.OwnerA, FTransform(FVector(0, 0, 5)).ToMatrixWithScale());
    bPassed &= TestTrue(TEXT("MoveOwner moves A0"), F.Pool->GetInstance(A0, Component, Index) && [&]
    {
        FTransform T;
        return Component->GetInstanceTransform(Index, T, true) && T.GetLocation().Equals(FVector(0, 0, 5), 1e-3);
    }());
    bPassed &= TestTrue(TEXT("MoveOwner leaves B0 alone"), F.Pool->GetInstance(B0, Component, Index) && [&]
    {
        FTransform T;
        return Component->GetInstanceTransform(Index, T, true) && T.GetLocation().Equals(FVector(20, 0, 0), 1e-3);
    }());

    // Bulk: one render-state refresh for the whole scope.
    F.Pool->ResetMetricsForTests();
    F.Pool->BeginBulk();
    for (int32 Step = 0; Step < 10; ++Step) F.Pool->Update(A0, FTransform(FVector(0, 0, 5 + Step)).ToMatrixWithScale());
    bPassed &= TestEqual(TEXT("no refresh inside the bulk scope"), F.Pool->GetMetrics().RenderStateRefreshes, 0ull);
    F.Pool->EndBulk();
    bPassed &= TestEqual(TEXT("exactly one refresh per touched bucket at EndBulk"), F.Pool->GetMetrics().RenderStateRefreshes, 1ull);

    F.Pool->RemoveOwner(*F.OwnerA);
    bPassed &= TestEqual(TEXT("RemoveOwner frees A's instances"), Component != nullptr ? Component->GetInstanceCount() : -1, 1);
    bPassed &= TestFalse(TEXT("A's handles are dead after RemoveOwner"), F.Pool->IsValidHandle(A0) || F.Pool->IsValidHandle(A1));
    bPassed &= TestTrue(TEXT("B0 survives RemoveOwner(A)"), F.Lookup(B0, Owner, Path, Index) && Owner == F.OwnerB);
    return bPassed;
}

// 16 §4 / R5b-0: a mesh reimport reconciles the pool per bucket, never per
// owner. Payload/bounds only refresh; a bucket-descriptor change migrates the
// bucket to a fresh ISM while every handle (hidden ones included) survives with
// its owner, node path and transform; collision recreates physics; an empty
// delta and an unrelated mesh touch nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHInstancePoolReconcileMeshTest,
    "Mimir.V5.Composite.Pool.ReconcileMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHInstancePoolReconcileMeshTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolFixture F;
    if (!F.Build(*this)) return false;
    const FMHInstanceHandle A0 = F.Add(*F.OwnerA, TEXT("a:nodes[0]"), F.DescA, FVector(0, 0, 0));
    const FMHInstanceHandle A1 = F.Add(*F.OwnerA, TEXT("a:nodes[1]"), F.DescA, FVector(10, 0, 0));
    const FMHInstanceHandle B0 = F.Add(*F.OwnerB, TEXT("b:nodes[0]"), F.DescA, FVector(20, 0, 0));
    const FMHInstanceHandle BB = F.Add(*F.OwnerB, TEXT("b:nodes[1]"), F.DescB, FVector(30, 0, 0));
    TArray<UInstancedStaticMeshComponent*> BucketsA, BucketsB;
    F.Pool->GetBucketComponents(*F.MeshA, BucketsA);
    F.Pool->GetBucketComponents(*F.MeshB, BucketsB);
    bool bPassed = TestEqual(TEXT("one bucket renders mesh A"), BucketsA.Num(), 1);
    bPassed &= TestEqual(TEXT("one bucket renders mesh B"), BucketsB.Num(), 1);
    if (BucketsA.Num() != 1 || BucketsB.Num() != 1) return false;
    UInstancedStaticMeshComponent* OldA = BucketsA[0];
    UInstancedStaticMeshComponent* OldB = BucketsB[0];
    F.Pool->HideOwner(*F.OwnerB);
    bPassed &= TestEqual(TEXT("B hidden: bucket A renders A's two instances"), OldA->GetInstanceCount(), 2);

    // Empty delta: nothing.
    FMHEndpointInterfaceDelta None;
    F.Pool->ResetMetricsForTests();
    bPassed &= TestEqual(TEXT("empty delta touches no bucket"), F.Pool->ReconcileMesh(*F.MeshA, None), 0);
    bPassed &= TestEqual(TEXT("empty delta refreshes nothing"), F.Pool->GetMetrics().RenderStateRefreshes, 0ull);

    // Payload: refresh in place, same component objects.
    FMHEndpointInterfaceDelta Payload;
    Payload.bPayload = true;
    Payload.bBounds = true;
    bPassed &= TestEqual(TEXT("payload delta touches mesh A's bucket only"), F.Pool->ReconcileMesh(*F.MeshA, Payload), 1);
    bPassed &= TestEqual(TEXT("payload delta refreshes render state once"), F.Pool->GetMetrics().RenderStateRefreshes, 1ull);
    F.Pool->GetBucketComponents(*F.MeshA, BucketsA);
    bPassed &= TestTrue(TEXT("payload delta keeps bucket A's component"), BucketsA.Num() == 1 && BucketsA[0] == OldA);

    // Collision: physics recreated, same component.
    FMHEndpointInterfaceDelta Collision;
    Collision.bCollisionInterface = true;
    bPassed &= TestEqual(TEXT("collision delta touches one bucket"), F.Pool->ReconcileMesh(*F.MeshA, Collision), 1);
    bPassed &= TestEqual(TEXT("collision delta refreshes physics once"), F.Pool->GetMetrics().PhysicsRefreshes, 1ull);

    // Descriptor: the bucket migrates to a new ISM; handles, owners, paths,
    // transforms and the hidden state all survive; mesh B is untouched.
    FMHEndpointInterfaceDelta Descriptor;
    Descriptor.bBucketDescriptor = true;
    bPassed &= TestEqual(TEXT("descriptor delta touches one bucket"), F.Pool->ReconcileMesh(*F.MeshA, Descriptor), 1);
    F.Pool->GetBucketComponents(*F.MeshA, BucketsA);
    bPassed &= TestTrue(TEXT("descriptor delta migrates bucket A to a new component"), BucketsA.Num() == 1 && BucketsA[0] != OldA && IsValid(BucketsA[0]));
    bPassed &= TestFalse(TEXT("the old bucket component is destroyed"), IsValid(OldA));
    F.Pool->GetBucketComponents(*F.MeshB, BucketsB);
    bPassed &= TestTrue(TEXT("mesh B's bucket keeps its component"), BucketsB.Num() == 1 && BucketsB[0] == OldB);
    bPassed &= TestEqual(TEXT("bucket count is unchanged"), F.Pool->NumBuckets(), 2);
    UInstancedStaticMeshComponent* NewA = BucketsA.Num() == 1 ? BucketsA[0] : nullptr;
    bPassed &= TestTrue(TEXT("migrated bucket lives on the pool actor"), NewA != nullptr && NewA->GetOwner() != nullptr && NewA->GetOwner()->IsA<AMHInstancePoolActor>());
    bPassed &= TestEqual(TEXT("migrated bucket renders the two live instances"), NewA != nullptr ? NewA->GetInstanceCount() : -1, 2);
    bPassed &= TestTrue(TEXT("handles survive migration"), F.Pool->IsValidHandle(A0) && F.Pool->IsValidHandle(A1) && F.Pool->IsValidHandle(B0));
    AActor* Owner = nullptr;
    FString Path;
    int32 Index = INDEX_NONE;
    bPassed &= TestTrue(TEXT("A1 reverse lookup after migration"), F.Lookup(A1, Owner, Path, Index) && Owner == F.OwnerA && Path == TEXT("a:nodes[1]"));
    UInstancedStaticMeshComponent* Component = nullptr;
    bPassed &= TestTrue(TEXT("A1 keeps its transform after migration"), F.Pool->GetInstance(A1, Component, Index) && Component == NewA && [&]
    {
        FTransform T;
        return Component->GetInstanceTransform(Index, T, true) && T.GetLocation().Equals(FVector(10, 0, 0), 1e-3);
    }());
    bPassed &= TestEqual(TEXT("B stays hidden through migration"), F.Pool->NumLiveInstances(*F.OwnerB), 0);
    F.Pool->ShowOwner(*F.OwnerB);
    bPassed &= TestEqual(TEXT("ShowOwner after migration lands in the new bucket"), NewA != nullptr ? NewA->GetInstanceCount() : -1, 3);
    bPassed &= TestTrue(TEXT("B0 reverse lookup after show"), F.Lookup(B0, Owner, Path, Index) && Owner == F.OwnerB && Path == TEXT("b:nodes[0]"));
    // The migrated bucket still serves the descriptor: a new Add joins it.
    const FMHInstanceHandle A2 = F.Add(*F.OwnerA, TEXT("a:nodes[2]"), F.DescA, FVector(40, 0, 0));
    bPassed &= TestEqual(TEXT("new instance joins the migrated bucket"), A2.BucketId, A0.BucketId);
    bPassed &= TestEqual(TEXT("still two buckets"), F.Pool->NumBuckets(), 2);
    // Unrelated mesh: nothing.
    bPassed &= TestEqual(TEXT("unrelated mesh reconciles no bucket"), F.Pool->ReconcileMesh(*F.MeshB, None), 0);
    return bPassed;
}

// 16 §2.8 bucket identity: the descriptor, never the live component state.
// A bucket whose component drifted from its descriptor (external policy or
// mesh mutation) is retired on the next Add: a fresh component configured from
// the descriptor takes its instances, handles survive, and Adds without drift
// never migrate anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHInstancePoolDriftedBucketTest,
    "Mimir.V5.Composite.Pool.DriftedBucketIsRetired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHInstancePoolDriftedBucketTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolFixture F;
    if (!F.Build(*this)) return false;
    F.Pool->ResetMetricsForTests();
    const FMHInstanceHandle H0 = F.Add(*F.OwnerA, TEXT("a:nodes[0]"), F.DescA, FVector(0, 0, 0));
    const FMHInstanceHandle H1 = F.Add(*F.OwnerB, TEXT("b:nodes[0]"), F.DescA, FVector(10, 0, 0));
    TArray<UInstancedStaticMeshComponent*> Buckets;
    F.Pool->GetBucketComponents(*F.MeshA, Buckets);
    bool bPassed = TestEqual(TEXT("one bucket for mesh A"), Buckets.Num(), 1);
    bPassed &= TestEqual(TEXT("adds without drift migrate nothing"), F.Pool->GetMetrics().BucketsMigrated, 0ull);
    if (Buckets.Num() != 1) return false;
    UInstancedStaticMeshComponent* C0 = Buckets[0];
    const bool bDescriptorCastShadow = F.DescA.bCastShadow;

    // Policy drift on the shared component.
    C0->CastShadow = !C0->CastShadow;
    const FMHInstanceHandle H2 = F.Add(*F.OwnerA, TEXT("a:nodes[1]"), F.DescA, FVector(20, 0, 0));
    F.Pool->GetBucketComponents(*F.MeshA, Buckets);
    bPassed &= TestTrue(TEXT("drifted bucket is retired for a fresh component"), Buckets.Num() == 1 && Buckets[0] != C0 && IsValid(Buckets[0]));
    bPassed &= TestFalse(TEXT("drifted component is destroyed"), IsValid(C0));
    bPassed &= TestEqual(TEXT("one migration"), F.Pool->GetMetrics().BucketsMigrated, 1ull);
    bPassed &= TestEqual(TEXT("still one bucket"), F.Pool->NumBuckets(), 1);
    UInstancedStaticMeshComponent* C1 = Buckets.Num() == 1 ? Buckets[0] : nullptr;
    bPassed &= TestTrue(TEXT("fresh component follows the descriptor"), C1 != nullptr && C1->CastShadow == bDescriptorCastShadow && C1->GetStaticMesh() == F.MeshA);
    bPassed &= TestEqual(TEXT("fresh component renders every instance"), C1 != nullptr ? C1->GetInstanceCount() : -1, 3);
    bPassed &= TestTrue(TEXT("handles survive the retirement"), F.Pool->IsValidHandle(H0) && F.Pool->IsValidHandle(H1) && F.Pool->IsValidHandle(H2));
    UInstancedStaticMeshComponent* Component = nullptr;
    int32 Index = INDEX_NONE;
    bPassed &= TestTrue(TEXT("H1 keeps its transform in the fresh component"), F.Pool->GetInstance(H1, Component, Index) && Component == C1 && [&]
    {
        FTransform T;
        return Component->GetInstanceTransform(Index, T, true) && T.GetLocation().Equals(FVector(10, 0, 0), 1e-3);
    }());
    AActor* Owner = nullptr;
    FString Path;
    bPassed &= TestTrue(TEXT("H0 reverse lookup after retirement"), F.Lookup(H0, Owner, Path, Index) && Owner == F.OwnerA && Path == TEXT("a:nodes[0]"));

    // Endpoint drift: the component renders another mesh than its descriptor.
    if (C1 != nullptr) C1->SetStaticMesh(F.MeshB);
    const FMHInstanceHandle H3 = F.Add(*F.OwnerB, TEXT("b:nodes[1]"), F.DescA, FVector(30, 0, 0));
    F.Pool->GetBucketComponents(*F.MeshA, Buckets);
    bPassed &= TestTrue(TEXT("mesh drift retires the bucket again"), Buckets.Num() == 1 && Buckets[0] != C1 && IsValid(Buckets[0]) && Buckets[0]->GetStaticMesh() == F.MeshA);
    bPassed &= TestEqual(TEXT("two migrations"), F.Pool->GetMetrics().BucketsMigrated, 2ull);
    bPassed &= TestEqual(TEXT("fresh component renders four instances"), Buckets.Num() == 1 ? Buckets[0]->GetInstanceCount() : -1, 4);
    bPassed &= TestTrue(TEXT("H3 is live"), F.Pool->IsValidHandle(H3));
    // No drift: the next Add reuses the component.
    UInstancedStaticMeshComponent* C2 = Buckets.Num() == 1 ? Buckets[0] : nullptr;
    F.Add(*F.OwnerA, TEXT("a:nodes[2]"), F.DescA, FVector(40, 0, 0));
    F.Pool->GetBucketComponents(*F.MeshA, Buckets);
    bPassed &= TestTrue(TEXT("no drift, no migration"), Buckets.Num() == 1 && Buckets[0] == C2 && F.Pool->GetMetrics().BucketsMigrated == 2ull);
    return bPassed;
}

// R5-F (audit §2.4): a mesh-interface migration re-derives only the mesh
// side of the descriptor; the bucket's placement policy (collision, render,
// mobility, visibility, appearance layout) survives, and the next Add with the
// same descriptor still lands in that bucket.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHInstancePoolMigrationPolicyTest,
    "Mimir.V5.Composite.Pool.MigrationPreservesPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHInstancePoolMigrationPolicyTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolFixture F;
    if (!F.Build(*this)) return false;
    FMHPoolBucketDescriptor Custom = F.DescA;
    Custom.bCastShadow = !Custom.bCastShadow;
    Custom.bVisibleInRayTracing = !Custom.bVisibleInRayTracing;
    Custom.Mobility = Custom.Mobility == EComponentMobility::Static ? EComponentMobility::Movable : EComponentMobility::Static;
    const FMHInstanceHandle H0 = F.Add(*F.OwnerA, TEXT("a:nodes[0]"), Custom, FVector(0, 0, 0));
    TArray<UInstancedStaticMeshComponent*> Buckets;
    F.Pool->GetBucketComponents(*F.MeshA, Buckets);
    bool bPassed = TestTrue(TEXT("custom bucket exists"), H0.IsSet() && Buckets.Num() == 1);
    if (Buckets.Num() != 1) return false;
    bPassed &= TestTrue(TEXT("custom policy applied"), Buckets[0]->CastShadow == Custom.bCastShadow && Buckets[0]->Mobility == Custom.Mobility);

    FMHEndpointInterfaceDelta Descriptor;
    Descriptor.bBucketDescriptor = true;
    bPassed &= TestEqual(TEXT("descriptor delta migrates the bucket"), F.Pool->ReconcileMesh(*F.MeshA, Descriptor), 1);
    F.Pool->GetBucketComponents(*F.MeshA, Buckets);
    bPassed &= TestTrue(TEXT("migrated component keeps the custom policy"), Buckets.Num() == 1 &&
        Buckets[0]->CastShadow == Custom.bCastShadow && Buckets[0]->bVisibleInRayTracing == Custom.bVisibleInRayTracing && Buckets[0]->Mobility == Custom.Mobility);
    const FMHInstanceHandle H1 = F.Add(*F.OwnerB, TEXT("b:nodes[0]"), Custom, FVector(10, 0, 0));
    bPassed &= TestEqual(TEXT("the same descriptor still addresses the migrated bucket"), H1.BucketId, H0.BucketId);
    bPassed &= TestEqual(TEXT("no second bucket for the custom descriptor"), F.Pool->NumBuckets(), 1);
    bPassed &= TestTrue(TEXT("H0 survives"), F.Pool->IsValidHandle(H0));
    return bPassed;
}

// R5-F (audit §2.6): the pool actor is a transient editor representation, not
// an editor-only/hidden-in-game actor; otherwise Game View (G) hides every
// pooled instance. Transient flags alone keep it out of save, cook and PIE.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHInstancePoolActorVisibleInGameViewTest,
    "Mimir.V5.Composite.Pool.PoolActorVisibleInGameView",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHInstancePoolActorVisibleInGameViewTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPoolFixture F;
    if (!F.Build(*this)) return false;
    const FMHInstanceHandle H0 = F.Add(*F.OwnerA, TEXT("a:nodes[0]"), F.DescA, FVector(0, 0, 0));
    UInstancedStaticMeshComponent* Component = nullptr;
    int32 Index = INDEX_NONE;
    if (!TestTrue(TEXT("instance"), F.Pool->GetInstance(H0, Component, Index) && Component != nullptr)) return false;
    const AActor* PoolActor = Component->GetOwner();
    bool bPassed = TestNotNull(TEXT("pool actor"), PoolActor);
    if (PoolActor == nullptr) return false;
    bPassed &= TestFalse(TEXT("pool actor is not hidden in game (Game View renders it)"), PoolActor->IsHidden());
    bPassed &= TestFalse(TEXT("pool actor is not editor-only"), PoolActor->IsEditorOnly());
    bPassed &= TestFalse(TEXT("bucket component is not editor-only"), Component->IsEditorOnly());
    bPassed &= TestTrue(TEXT("pool actor is transient"), PoolActor->HasAnyFlags(RF_Transient));
    bPassed &= TestTrue(TEXT("pool actor is not duplicated into PIE"), PoolActor->HasAnyFlags(RF_DuplicateTransient));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
