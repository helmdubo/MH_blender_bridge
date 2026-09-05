#include "MHRecipeTestFixture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Root = [mesh A] [mesh B] [composite child(mesh C)], one placement, previewed. */
struct FReconcileFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    AMHCompositeActor* Actor = nullptr;
    UMHCompositeAsset* Root = nullptr;
    UMHCompositeAsset* Child = nullptr;
    FString MeshA, MeshB, MeshC;
    UStaticMesh* MeshObjectA = nullptr;
    UStaticMesh* MeshObjectB = nullptr;

    explicit FReconcileFixture(FAutomationTestBase& Test) : Recipe(Test) {}
    ~FReconcileFixture() { if (World != nullptr) World->DestroyWorld(true); }

    bool Build(FAutomationTestBase& Test)
    {
        MeshA = Recipe.Name(TEXT("reconcile_mesh_a"));
        MeshB = Recipe.Name(TEXT("reconcile_mesh_b"));
        MeshC = Recipe.Name(TEXT("reconcile_mesh_c"));
        MeshObjectA = Recipe.Mesh(MeshA);
        MeshObjectB = Recipe.Mesh(MeshB);
        Recipe.Mesh(MeshC);
        FMHCompositeDocument ChildDocument;
        {
            FMHCompositeNode& Leaf = ChildDocument.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshC;
        }
        Child = Recipe.Composite(Recipe.Name(TEXT("reconcile_child")), ChildDocument, {});
        if (Child == nullptr) return false;
        FMHCompositeDocument RootDocument;
        for (const FString& Mesh : {MeshA, MeshB})
        {
            FMHCompositeNode& Node = RootDocument.Nodes.AddDefaulted_GetRef();
            Node.Kind = EMHCompositeNodeKind::Mesh;
            Node.Resource = Mesh;
            Node.Transform.TranslationCm = FVector(RootDocument.Nodes.Num() * 100.0, 0.0, 0.0);
        }
        {
            FMHCompositeNode& Nested = RootDocument.Nodes.AddDefaulted_GetRef();
            Nested.Kind = EMHCompositeNodeKind::Composite;
            Nested.Resource = Child->LogicalName;
            Nested.Transform.TranslationCm = FVector(0.0, 300.0, 0.0);
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("reconcile_root")), RootDocument, {});
        if (Root == nullptr) return false;
        World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
        if (!Test.TestNotNull(TEXT("reconcile world"), World)) return false;
        Actor = World->SpawnActor<AMHCompositeActor>();
        if (!Test.TestNotNull(TEXT("reconcile actor"), Actor)) return false;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(3);
        Actor->SetAppearanceSeed(5);
        Actor->SetCompositeAsset(Root);
        return Test.TestTrue(TEXT("preview builds: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty()) &&
            Test.TestNotNull(TEXT("resident plan"), Actor->GetResolvedPlan());
    }

    static FMHResourceKey Key(const EMHResourceKind Kind, const FString& Name)
    {
        FMHResourceKey Result;
        Result.Kind = Kind;
        Result.LogicalName = Name;
        return Result;
    }

    /** ISM bucket that renders LogicalName, or nullptr. */
    UInstancedStaticMeshComponent* Bucket(const FString& LogicalName) const
    {
        for (UActorComponent* Component : Actor->GetDerivedComponents())
        {
            UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(Component);
            if (Ism != nullptr && Ism->GetStaticMesh() != nullptr && Ism->GetStaticMesh()->GetName() == LogicalName) return Ism;
        }
        return nullptr;
    }

    TSet<UActorComponent*> DerivedSet() const
    {
        TSet<UActorComponent*> Result;
        for (UActorComponent* Component : Actor->GetDerivedComponents()) Result.Add(Component);
        return Result;
    }
};

} // namespace

// 16 §4 row 2: a mesh reimport that keeps the interface (payload only) refreshes
// buckets in place — no placement rebuild, no recompiled recipe, the same
// component objects. Row 4/5: material and texture changes touch nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHReconcileSameInterfaceTest,
    "Mimir.V5.Composite.Reconcile.SameInterfaceReimportKeepsBuckets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHReconcileSameInterfaceTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FReconcileFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get();
    if (!TestNotNull(TEXT("recipe registry"), Recipes)) return false;
    const uint32 Rebuilds = Fixture.Actor->GetPlacementRebuildCount();
    const uint32 RootRevision = Recipes->GetRecipeRevision(*Fixture.Root);
    const TSet<UActorComponent*> Before = Fixture.DerivedSet();
    UInstancedStaticMeshComponent* BucketA = Fixture.Bucket(Fixture.MeshA);
    bool bPassed = TestNotNull(TEXT("bucket for mesh A exists"), BucketA);
    const int32 InstancesA = BucketA != nullptr ? BucketA->GetInstanceCount() : -1;
    const int32 Leaves = Fixture.Actor->GetLeafMaterializations().Num();

    // Payload-only reimport: receipt hash moves, interface identical.
    Cast<UMHStaticMeshImportData>(Fixture.MeshObjectA->GetAssetImportData())->SourceHash = MHRawPayloadHash({0x72, 0x33, 0x62, 0x21});
    MHResetPlacementStageMetrics();
    MHNotifyGeneratedResourceChanged(FReconcileFixture::Key(EMHResourceKind::StaticMesh, Fixture.MeshA));
    bPassed &= TestEqual(TEXT("payload reimport is not a placement rebuild"), Fixture.Actor->GetPlacementRebuildCount(), Rebuilds);
    bPassed &= TestEqual(TEXT("payload reimport runs no layout"), MHGetPlacementStageMetrics().Get(EMHPlacementStage::ResolveCompositePlan).Calls, 0ull);
    bPassed &= TestEqual(TEXT("payload reimport recompiles no recipe"), Recipes->GetRecipeRevision(*Fixture.Root), RootRevision);
    bPassed &= TestTrue(TEXT("payload reimport keeps the same derived component objects"), Fixture.DerivedSet().Difference(Before).IsEmpty() && Before.Difference(Fixture.DerivedSet()).IsEmpty());
    bPassed &= TestEqual(TEXT("bucket A keeps its instances"), BucketA != nullptr ? BucketA->GetInstanceCount() : -2, InstancesA);
    bPassed &= TestEqual(TEXT("leaf count is unchanged"), Fixture.Actor->GetLeafMaterializations().Num(), Leaves);
    bPassed &= TestTrue(TEXT("registry saw the payload change"),
        UMHEndpointPrototypeRegistry::Get()->GetLastInterfaceDelta(FReconcileFixture::Key(EMHResourceKind::StaticMesh, Fixture.MeshA)).bPayload);

    // Material and texture changes: nothing in the placement.
    MHNotifyGeneratedResourceChanged(FReconcileFixture::Key(EMHResourceKind::Material, Fixture.Recipe.Name(TEXT("reconcile_material"))));
    MHNotifyGeneratedResourceChanged(FReconcileFixture::Key(EMHResourceKind::Texture, Fixture.Recipe.Name(TEXT("reconcile_texture"))));
    bPassed &= TestEqual(TEXT("material/texture notifications rebuild nothing"), Fixture.Actor->GetPlacementRebuildCount(), Rebuilds);
    bPassed &= TestTrue(TEXT("no error after notifications: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty());
    return bPassed;
}

// 16 §4 row 3: a changed bucket descriptor migrates only the buckets that
// render that mesh; every other bucket keeps its component object, and the
// placement is still not rebuilt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHReconcileDescriptorChangeTest,
    "Mimir.V5.Composite.Reconcile.DescriptorChangeMigratesOnlyAffectedBucket",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHReconcileDescriptorChangeTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FReconcileFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    const uint32 Rebuilds = Fixture.Actor->GetPlacementRebuildCount();
    UInstancedStaticMeshComponent* BucketA = Fixture.Bucket(Fixture.MeshA);
    UInstancedStaticMeshComponent* BucketB = Fixture.Bucket(Fixture.MeshB);
    bool bPassed = TestNotNull(TEXT("bucket A"), BucketA) && TestNotNull(TEXT("bucket B"), BucketB);
    const int32 Leaves = Fixture.Actor->GetLeafMaterializations().Num();

    // Interface change on A: a second material slot.
    UMaterial* Slot1 = NewObject<UMaterial>(GetTransientPackage(), FName(*Fixture.Recipe.Name(TEXT("reconcile_slot1"))));
    Fixture.MeshObjectA->GetStaticMaterials().Add(FStaticMaterial(Slot1, TEXT("slot1"), TEXT("slot1")));
    MHNotifyGeneratedResourceChanged(FReconcileFixture::Key(EMHResourceKind::StaticMesh, Fixture.MeshA));
    const FMHEndpointInterfaceDelta Delta = UMHEndpointPrototypeRegistry::Get()->GetLastInterfaceDelta(FReconcileFixture::Key(EMHResourceKind::StaticMesh, Fixture.MeshA));
    bPassed &= TestTrue(TEXT("registry classified a descriptor change"), Delta.bBucketDescriptor);
    bPassed &= TestEqual(TEXT("descriptor change is not a placement rebuild"), Fixture.Actor->GetPlacementRebuildCount(), Rebuilds);
    bPassed &= TestEqual(TEXT("bucket B keeps its component object"), Fixture.Bucket(Fixture.MeshB), BucketB);
    UInstancedStaticMeshComponent* MigratedA = Fixture.Bucket(Fixture.MeshA);
    bPassed &= TestNotNull(TEXT("mesh A still has a bucket after migration"), MigratedA);
    bPassed &= TestTrue(TEXT("migrated bucket A renders one instance"), MigratedA != nullptr && MigratedA->GetInstanceCount() == 1);
    bPassed &= TestEqual(TEXT("leaf count is unchanged"), Fixture.Actor->GetLeafMaterializations().Num(), Leaves);
    bPassed &= TestTrue(TEXT("no error after migration: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty());
    return bPassed;
}

// 16 §4 row 1: a child recipe reimport recompiles that recipe only; the parent
// recipe keeps its revision (KICKOFF §5 R3: parent_recipes_recompiled == 0).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHReconcileChildRecipeTest,
    "Mimir.V5.Composite.Reconcile.ChildRecipeReimportKeepsParentRecipe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHReconcileChildRecipeTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FReconcileFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get();
    if (!TestNotNull(TEXT("recipe registry"), Recipes)) return false;
    const uint32 RootRevision = Recipes->GetRecipeRevision(*Fixture.Root);
    const uint32 ChildRevision = Recipes->GetRecipeRevision(*Fixture.Child);
    UInstancedStaticMeshComponent* BucketA = Fixture.Bucket(Fixture.MeshA);
    MHNotifyGeneratedResourceChanged(FReconcileFixture::Key(EMHResourceKind::Composite, Fixture.Child->LogicalName));
    bool bPassed = TestEqual(TEXT("child recipe revision advances"), Recipes->GetRecipeRevision(*Fixture.Child), ChildRevision + 1u);
    bPassed &= TestEqual(TEXT("parent recipe is not recompiled"), Recipes->GetRecipeRevision(*Fixture.Root), RootRevision);
    // Only the nested subtree is rematerialized: the buckets of the parent's own
    // leaves keep their component objects.
    bPassed &= TestEqual(TEXT("parent-level bucket A keeps its component object"), Fixture.Bucket(Fixture.MeshA), BucketA);
    bPassed &= TestTrue(TEXT("no error after child reimport: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty());
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
