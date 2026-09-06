#include "MHCompositeEditFixture.h"

#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHInstancePool.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Pool handles of the rows of Actor whose node path starts with Prefix. */
TArray<FMHInstanceHandle> HandlesUnder(const AMHCompositeActor& Actor, const FString& Prefix)
{
    TArray<FMHInstanceHandle> Handles;
    for (const FMHCompositeLeafMaterialization& Row : Actor.GetLeafMaterializations())
    {
        if (Row.NodePath.StartsWith(Prefix) && Row.Handle.IsSet()) Handles.Add(Row.Handle);
    }
    return Handles;
}

struct FSuppressionScene
{
    FCompositeEditFixture F;
    UMHInstancePoolSubsystem* Pool = nullptr;
    FString FirstPrefix, SecondPrefix;
    TArray<FVector> FirstBefore, SecondBefore, BBefore, ForeignBefore;
    TArray<FMHInstanceHandle> SecondHandles;
    int32 LiveBefore = 0;

    explicit FSuppressionScene(FAutomationTestBase& Test) : F(Test) {}

    bool Build(FAutomationTestBase& Test)
    {
        if (!F.Build(Test)) return false;
        Pool = UMHInstancePoolSubsystem::Get(F.World);
        const FMHResolvedCompositeNode* First = FCompositeEditFixture::Invocation(*F.A, 0);
        const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
        if (!Test.TestNotNull(TEXT("pool"), Pool) || !Test.TestNotNull(TEXT("first"), First) || !Test.TestNotNull(TEXT("second"), Second)) return false;
        FirstPrefix = First->NodePath + TEXT(">");
        SecondPrefix = Second->NodePath + TEXT(">");
        FirstBefore = FCompositeEditFixture::LeafWorldsUnder(*F.A, FirstPrefix);
        SecondBefore = FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix);
        BBefore = FCompositeEditFixture::LeafWorldsUnder(*F.B, F.Root->LogicalName);
        ForeignBefore = F.ForeignWorlds();
        SecondHandles = HandlesUnder(*F.A, SecondPrefix);
        LiveBefore = Pool->NumLiveInstances(*F.A);
        return Test.TestTrue(TEXT("the second invocation has pooled rows"), SecondHandles.Num() >= 2 && SecondHandles.Num() == SecondBefore.Num());
    }

    bool OthersUntouched() const
    {
        return FCompositeEditFixture::SameLocations(FirstBefore, FCompositeEditFixture::LeafWorldsUnder(*F.A, FirstPrefix)) &&
            FCompositeEditFixture::SameLocations(BBefore, FCompositeEditFixture::LeafWorldsUnder(*F.B, F.Root->LogicalName)) &&
            FCompositeEditFixture::SameLocations(ForeignBefore, F.ForeignWorlds());
    }
};

} // namespace

// CE-2a (spec §6.1): a suppression lease hides exactly the instances it names,
// orthogonally to owner visibility; neighbours, other owners and foreign ISMs
// never notice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditSuppressionLeaseTest,
    "Mimir.V5.Composite.EditMode.Suppression.LeaseHidesOnlyItsInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditSuppressionLeaseTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FSuppressionScene S(*this);
    if (!S.Build(*this)) return false;
    const int32 N = S.SecondHandles.Num();
    const FMHPoolSuppressionLease Lease = S.Pool->AcquireSuppression(S.SecondHandles);
    bool bPassed = TestTrue(TEXT("lease is set and names every handle"), Lease.IsSet() && Lease.Handles.Num() == N);
    bPassed &= TestEqual(TEXT("the owner lost exactly the suppressed instances"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore - N);
    for (const FMHInstanceHandle& Handle : S.SecondHandles)
    {
        UInstancedStaticMeshComponent* Component = nullptr;
        int32 InstanceIndex = INDEX_NONE;
        bPassed &= TestTrue(TEXT("the handle stays valid while suppressed"), S.Pool->IsValidHandle(Handle) && S.Pool->IsSuppressed(Handle));
        bPassed &= TestTrue(TEXT("no ISM instance renders a suppressed handle"), S.Pool->GetInstance(Handle, Component, InstanceIndex) && InstanceIndex == INDEX_NONE);
    }
    bPassed &= TestEqual(TEXT("the suppressed occurrence renders nothing"), FCompositeEditFixture::LeafWorldsUnder(*S.F.A, S.SecondPrefix).Num(), 0);
    bPassed &= TestTrue(TEXT("the other invocation, the other placement and the foreign ISM are untouched"), S.OthersUntouched());

    // Owner visibility is a separate axis: hide/show the owner around the lease.
    S.Pool->HideOwner(*S.F.A);
    bPassed &= TestEqual(TEXT("hidden owner renders nothing"), S.Pool->NumLiveInstances(*S.F.A), 0);
    S.Pool->ShowOwner(*S.F.A);
    bPassed &= TestEqual(TEXT("shown owner keeps the lease in force"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore - N);
    bPassed &= TestTrue(TEXT("still suppressed after show"), S.Pool->IsSuppressed(S.SecondHandles[0]));

    S.Pool->ReleaseSuppression(Lease);
    bPassed &= TestEqual(TEXT("release restores the instances"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore);
    bPassed &= TestFalse(TEXT("released handle is not suppressed"), S.Pool->IsSuppressed(S.SecondHandles[0]));
    bPassed &= TestTrue(TEXT("the occurrence renders where it did"), FCompositeEditFixture::SameLocations(S.SecondBefore, FCompositeEditFixture::LeafWorldsUnder(*S.F.A, S.SecondPrefix)));
    S.Pool->ReleaseSuppression(Lease);
    bPassed &= TestEqual(TEXT("a second release is a no-op"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore);
    bPassed &= TestTrue(TEXT("others still untouched"), S.OthersUntouched());
    return bPassed;
}

// CE-2a: a lease outlives nothing — slots reused after a rebuild carry a new
// generation, so a stale release never shows or hides the new instance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditSuppressionStaleTest,
    "Mimir.V5.Composite.EditMode.Suppression.StaleReleaseTouchesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditSuppressionStaleTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FSuppressionScene S(*this);
    if (!S.Build(*this)) return false;
    const int32 N = S.SecondHandles.Num();
    const FMHPoolSuppressionLease Lease = S.Pool->AcquireSuppression(S.SecondHandles);
    bool bPassed = TestEqual(TEXT("suppressed"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore - N);
    // The owner rebuilds: its rows are removed and re-added (new generations).
    S.F.A->RebuildComposite();
    bPassed &= TestEqual(TEXT("a rebuilt owner renders everything again"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore);
    bPassed &= TestFalse(TEXT("the old handle is dead"), S.Pool->IsValidHandle(S.SecondHandles[0]));
    S.Pool->ReleaseSuppression(Lease);
    bPassed &= TestEqual(TEXT("a stale release touches nothing"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore);
    bPassed &= TestTrue(TEXT("the occurrence renders where it did"), FCompositeEditFixture::SameLocations(S.SecondBefore, FCompositeEditFixture::LeafWorldsUnder(*S.F.A, S.SecondPrefix)));
    bPassed &= TestTrue(TEXT("others untouched"), S.OthersUntouched());
    return bPassed;
}

// CE-2a: bucket migration (mesh reimport) keeps the lease and the reverse lookup.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditSuppressionMigrationTest,
    "Mimir.V5.Composite.EditMode.Suppression.MigrationKeepsTheLease",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditSuppressionMigrationTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FSuppressionScene S(*this);
    if (!S.Build(*this)) return false;
    if (!TestNotNull(TEXT("mesh C asset"), S.F.MeshAssetC)) return false;
    const int32 N = S.SecondHandles.Num();
    const FMHPoolSuppressionLease Lease = S.Pool->AcquireSuppression(S.SecondHandles);
    bool bPassed = TestEqual(TEXT("suppressed"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore - N);
    FMHEndpointInterfaceDelta Delta;
    Delta.bBucketDescriptor = true;
    S.Pool->ResetMetricsForTests();
    bPassed &= TestTrue(TEXT("mesh C's buckets migrate"), S.Pool->ReconcileMesh(*S.F.MeshAssetC, Delta) >= 1 && S.Pool->GetMetrics().BucketsMigrated >= 1);
    bPassed &= TestEqual(TEXT("migration keeps the lease"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore - N);
    bPassed &= TestTrue(TEXT("handles survive migration, still suppressed"), S.Pool->IsValidHandle(S.SecondHandles[0]) && S.Pool->IsSuppressed(S.SecondHandles[0]));
    bPassed &= TestEqual(TEXT("the suppressed occurrence still renders nothing"), FCompositeEditFixture::LeafWorldsUnder(*S.F.A, S.SecondPrefix).Num(), 0);
    // Reverse lookup of a visible neighbour still resolves to the owner.
    const TArray<FMHInstanceHandle> FirstHandles = HandlesUnder(*S.F.A, S.FirstPrefix);
    UInstancedStaticMeshComponent* Component = nullptr;
    int32 InstanceIndex = INDEX_NONE;
    AActor* Owner = nullptr;
    FString NodePath;
    bPassed &= TestTrue(TEXT("a visible neighbour resolves after migration"),
        FirstHandles.Num() > 0 && S.Pool->GetInstance(FirstHandles[0], Component, InstanceIndex) && InstanceIndex != INDEX_NONE &&
        S.Pool->ReverseLookup(Component, InstanceIndex, Owner, NodePath) && Owner == S.F.A && NodePath.StartsWith(S.FirstPrefix));
    S.Pool->ReleaseSuppression(Lease);
    bPassed &= TestEqual(TEXT("release after migration restores the instances"), S.Pool->NumLiveInstances(*S.F.A), S.LiveBefore);
    bPassed &= TestTrue(TEXT("the occurrence renders where it did"), FCompositeEditFixture::SameLocations(S.SecondBefore, FCompositeEditFixture::LeafWorldsUnder(*S.F.A, S.SecondPrefix)));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
