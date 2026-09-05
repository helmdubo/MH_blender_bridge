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

} // namespace UE::MimirComposite::Tests
