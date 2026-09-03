#include "MHRecipeTestFixture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectIterator.h"
#include "Random/MHRandomStream.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/**
 * R4-pre fixture (docs/reference_notes/dagor_composite_build_break_20260903.md §6):
 * root = [mesh A] [random{child_cmp(mesh C) w1, empty w0}] [composite nested_cmp(mesh B)] [group{mesh D}].
 * One Dagor-style split layer yields: StaticMeshActor(A), AMHCompositeActor(child_cmp),
 * AMHCompositeActor(nested_cmp), StaticMeshActor(D) — groups are structure, not entities.
 */
struct FBreakFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    AMHCompositeActor* Actor = nullptr;
    UMHCompositeAsset* Root = nullptr;
    UMHCompositeAsset* Child = nullptr;
    UMHCompositeAsset* Nested = nullptr;
    FString MeshA, MeshB, MeshC, MeshD;
    const FTransform ActorTransform = FTransform(FRotator(0.0, 30.0, 0.0), FVector(500.0, -200.0, 25.0));

    explicit FBreakFixture(FAutomationTestBase& Test) : Recipe(Test) {}

    ~FBreakFixture()
    {
        if (World != nullptr)
        {
            if (GEditor != nullptr && GEditor->Trans != nullptr) GEditor->Trans->Reset(INVTEXT("MH Break test teardown"));
            World->DestroyWorld(false);
        }
    }

    bool Build(FAutomationTestBase& Test)
    {
        MeshA = Recipe.Name(TEXT("break_mesh_a"));
        MeshB = Recipe.Name(TEXT("break_mesh_b"));
        MeshC = Recipe.Name(TEXT("break_mesh_c"));
        MeshD = Recipe.Name(TEXT("break_mesh_d"));
        for (const FString& Name : {MeshA, MeshB, MeshC, MeshD}) Recipe.Mesh(Name);

        FMHCompositeDocument ChildDocument;
        {
            FMHCompositeNode& Leaf = ChildDocument.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshC;
            Leaf.Transform.TranslationCm = FVector(0.0, 50.0, 0.0);
        }
        Child = Recipe.Composite(Recipe.Name(TEXT("break_child_cmp")), ChildDocument, {});
        FMHCompositeDocument NestedDocument;
        {
            FMHCompositeNode& Leaf = NestedDocument.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshB;
            Leaf.Transform.TranslationCm = FVector(0.0, 0.0, 75.0);
        }
        Nested = Recipe.Composite(Recipe.Name(TEXT("break_nested_cmp")), NestedDocument, {});
        if (Child == nullptr || Nested == nullptr) return false;

        FMHCompositeDocument RootDocument;
        {
            FMHCompositeNode& Anchor = RootDocument.Nodes.AddDefaulted_GetRef();
            Anchor.Kind = EMHCompositeNodeKind::Mesh;
            Anchor.Resource = MeshA;
            Anchor.Name = TEXT("anchor");
            Anchor.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
            FMHCompositeNode& Random = RootDocument.Nodes.AddDefaulted_GetRef();
            Random.Kind = EMHCompositeNodeKind::Random;
            Random.Name = TEXT("random");
            Random.Transform.TranslationCm = FVector(0.0, 200.0, 0.0);
            FMHCompositeOption& Selected = Random.Options.AddDefaulted_GetRef();
            Selected.Kind = EMHCompositeOptionKind::Composite;
            Selected.Resource = Child->LogicalName;
            Selected.Weight = 1.0f;
            FMHCompositeOption& Unselected = Random.Options.AddDefaulted_GetRef();
            Unselected.Kind = EMHCompositeOptionKind::Empty;
            Unselected.Weight = 0.0f;
            FMHCompositeNode& NestedNode = RootDocument.Nodes.AddDefaulted_GetRef();
            NestedNode.Kind = EMHCompositeNodeKind::Composite;
            NestedNode.Resource = Nested->LogicalName;
            NestedNode.Name = TEXT("nested");
            NestedNode.Transform.TranslationCm = FVector(-150.0, 0.0, 0.0);
            NestedNode.Transform.RotationQuat = FQuat(FRotator(0.0, 90.0, 0.0));
            FMHCompositeNode& Group = RootDocument.Nodes.AddDefaulted_GetRef();
            Group.Name = TEXT("group");
            Group.Transform.TranslationCm = FVector(0.0, 0.0, 300.0);
            FMHCompositeNode& Grouped = Group.Children.AddDefaulted_GetRef();
            Grouped.Kind = EMHCompositeNodeKind::Mesh;
            Grouped.Resource = MeshD;
            Grouped.Transform.TranslationCm = FVector(10.0, 0.0, 0.0);
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("break_root_cmp")), RootDocument, {});
        if (Root == nullptr) return false;

        World = UWorld::CreateWorld(EWorldType::Editor, false);
        if (!Test.TestNotNull(TEXT("break editor world"), World)) return false;
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transactional;
        Actor = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), ActorTransform, SpawnParameters);
        if (!Test.TestNotNull(TEXT("break composite actor"), Actor)) return false;
        Actor->SetActorLabel(TEXT("BreakRoot"));
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(21);
        Actor->SetAppearanceSeed(34);
        Actor->SetCompositeAsset(Root);
        return Test.TestTrue(TEXT("preview builds: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty()) &&
            Test.TestNotNull(TEXT("resident plan"), Actor->GetResolvedPlan());
    }

    /** World matrix of the resolved node at a root-level path, times the actor transform. */
    bool NodeWorld(const FString& NodePath, FMatrix& OutWorld) const
    {
        const FMHResolvedCompositePlan* Plan = Actor != nullptr ? Actor->GetResolvedPlan() : nullptr;
        if (Plan == nullptr) return false;
        const FMHResolvedCompositeNode* Node = Plan->Nodes.FindByPredicate([&](const FMHResolvedCompositeNode& Value) { return Value.NodePath == NodePath; });
        if (Node == nullptr) return false;
        OutWorld = Node->WorldMatrix * ActorTransform.ToMatrixWithScale();
        return true;
    }
};

int32 CountISM(const AMHCompositeActor& Actor)
{
    int32 Count = 0;
    for (const UActorComponent* Component : Actor.GetDerivedComponents())
    {
        if (Cast<UInstancedStaticMeshComponent>(Component) != nullptr) ++Count;
    }
    return Count;
}

} // namespace

// Break removes exactly one layer of the recipe (Dagor "Split composites"):
// top-level meshes become StaticMeshActors, nested composites stay composite
// actors, a random node yields only its selected variant, groups are promoted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHBreakTopLayerOnlyTest,
    "Mimir.V5.Composite.Break.TopLayerOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHBreakTopLayerOnlyTest::RunTest(const FString& Parameters)
{
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FBreakFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    const FString RootName = Fixture.Root->LogicalName;
    FMatrix AnchorWorld, RandomWorld, NestedWorld;
    bool bPassed = TestTrue(TEXT("anchor node resolved"), Fixture.NodeWorld(RootName + TEXT(":nodes[0]"), AnchorWorld));
    bPassed &= TestTrue(TEXT("random node resolved"), Fixture.NodeWorld(RootName + TEXT(":nodes[1]"), RandomWorld));
    bPassed &= TestTrue(TEXT("nested node resolved"), Fixture.NodeWorld(RootName + TEXT(":nodes[2]"), NestedWorld));
    const FMHResolvedCompositePlan* Plan = Fixture.Actor->GetResolvedPlan();
    const FMHResolvedCompositeLeaf* GroupedLeaf = Plan != nullptr
        ? Plan->Leaves.FindByPredicate([&](const FMHResolvedCompositeLeaf& Leaf) { return Leaf.Resource == Fixture.MeshD; }) : nullptr;
    bPassed &= TestNotNull(TEXT("grouped leaf resolved"), GroupedLeaf);
    const FMatrix GroupedWorld = GroupedLeaf != nullptr ? GroupedLeaf->WorldMatrix * Fixture.ActorTransform.ToMatrixWithScale() : FMatrix::Identity;

    TArray<AActor*> Broken;
    TArray<FString> Warnings;
    FString Error;
    if (!TestTrue(TEXT("Break succeeds: ") + Error, Subsystem->BreakComposites({Fixture.Actor}, Broken, Warnings, Error))) return false;

    TArray<AStaticMeshActor*> Meshes;
    TArray<AMHCompositeActor*> Composites;
    for (AActor* Spawned : Broken)
    {
        if (AStaticMeshActor* Mesh = Cast<AStaticMeshActor>(Spawned)) Meshes.Add(Mesh);
        else if (AMHCompositeActor* Composite = Cast<AMHCompositeActor>(Spawned)) Composites.Add(Composite);
    }
    bPassed &= TestEqual(TEXT("one layer: four actors"), Broken.Num(), 4);
    bPassed &= TestEqual(TEXT("two top-level meshes (anchor, promoted group child)"), Meshes.Num(), 2);
    bPassed &= TestEqual(TEXT("two nested composites survive as composite actors"), Composites.Num(), 2);

    const auto FindMesh = [&](const FString& LogicalName) -> AStaticMeshActor*
    {
        for (AStaticMeshActor* Mesh : Meshes)
        {
            const UStaticMesh* StaticMesh = Mesh->GetStaticMeshComponent() != nullptr ? Mesh->GetStaticMeshComponent()->GetStaticMesh() : nullptr;
            if (StaticMesh != nullptr && StaticMesh->GetName() == LogicalName) return Mesh;
        }
        return nullptr;
    };
    const auto FindComposite = [&](const UMHCompositeAsset* Asset) -> AMHCompositeActor*
    {
        for (AMHCompositeActor* Composite : Composites) if (Composite->GetCompositeAsset() == Asset) return Composite;
        return nullptr;
    };
    AStaticMeshActor* Anchor = FindMesh(Fixture.MeshA);
    AStaticMeshActor* Grouped = FindMesh(Fixture.MeshD);
    AMHCompositeActor* ChildActor = FindComposite(Fixture.Child);
    AMHCompositeActor* NestedActor = FindComposite(Fixture.Nested);
    bPassed &= TestNotNull(TEXT("anchor mesh actor"), Anchor);
    bPassed &= TestNotNull(TEXT("promoted grouped mesh actor"), Grouped);
    bPassed &= TestNotNull(TEXT("selected random variant as composite actor"), ChildActor);
    bPassed &= TestNotNull(TEXT("nested composite as composite actor"), NestedActor);
    bPassed &= TestNull(TEXT("mesh C of the selected variant is not flattened"), FindMesh(Fixture.MeshC));
    bPassed &= TestNull(TEXT("mesh B of the nested composite is not flattened"), FindMesh(Fixture.MeshB));
    if (Anchor != nullptr) bPassed &= TestTrue(TEXT("anchor keeps its layout world transform"),
        MHMatrixElementsWithinTrsTolerance(Anchor->GetActorTransform().ToMatrixWithScale(), AnchorWorld));
    if (Grouped != nullptr) bPassed &= TestTrue(TEXT("promoted child keeps its layout world transform"),
        MHMatrixElementsWithinTrsTolerance(Grouped->GetActorTransform().ToMatrixWithScale(), GroupedWorld));
    if (ChildActor != nullptr)
    {
        bPassed &= TestTrue(TEXT("variant actor keeps the random node world transform"),
            MHMatrixElementsWithinTrsTolerance(ChildActor->GetActorTransform().ToMatrixWithScale(), RandomWorld));
        // OPEN-R4P-1 fail-closed rule: seeds are forwarded from the parent as-is.
        bPassed &= TestEqual(TEXT("variant actor forwards the layout seed"), ChildActor->GetSeed(), 21);
        bPassed &= TestEqual(TEXT("variant actor forwards the appearance seed"), ChildActor->GetAppearanceSeed(), 34);
        bPassed &= TestFalse(TEXT("variant actor does not auto-reseed"), ChildActor->GetAutoSeed());
        bPassed &= TestTrue(TEXT("variant actor previews: ") + ChildActor->GetLastPlacementError(), ChildActor->GetResolvedPlan() != nullptr);
    }
    if (NestedActor != nullptr)
    {
        bPassed &= TestTrue(TEXT("nested actor keeps the nested node world transform"),
            MHMatrixElementsWithinTrsTolerance(NestedActor->GetActorTransform().ToMatrixWithScale(), NestedWorld));
        bPassed &= TestEqual(TEXT("nested actor forwards the layout seed"), NestedActor->GetSeed(), 21);
        bPassed &= TestEqual(TEXT("nested actor forwards the appearance seed"), NestedActor->GetAppearanceSeed(), 34);
    }
    bPassed &= TestTrue(TEXT("original composite actor is retired"), !IsValid(Fixture.Actor) || Fixture.Actor->IsActorBeingDestroyed());
    return bPassed;
}

// Break is a preview-plane operation (Dagor split reads pool fields only):
// no applied graph, no closure, no Asset Registry tag query.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHBreakNoProofTest,
    "Mimir.V5.Composite.Break.NoProofNoTagQueries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHBreakNoProofTest::RunTest(const FString& Parameters)
{
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FBreakFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    MHResetPlacementStageMetrics();
    MHResetEndpointResolveMetrics();
    TArray<AActor*> Broken;
    TArray<FString> Warnings;
    FString Error;
    bool bPassed = TestTrue(TEXT("Break succeeds: ") + Error, Subsystem->BreakComposites({Fixture.Actor}, Broken, Warnings, Error));
    bPassed &= TestEqual(TEXT("Break builds no applied graph"), MHGetPlacementStageMetrics().Get(EMHPlacementStage::BuildAppliedGraph).Calls, 0ull);
    bPassed &= TestEqual(TEXT("Break makes no Asset Registry tag queries"), MHGetEndpointResolveMetrics().AssetRegistryTagQueries, 0ull);
    bPassed &= TestEqual(TEXT("Break reads no live receipt tags"), MHGetEndpointResolveMetrics().LiveReceiptTagReads, 0ull);
    return bPassed;
}

// Undo after Break restores the placement from its record (asset, seeds,
// transform): the same number of derived components and buckets as before,
// no duplicates; Redo breaks it again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHBreakUndoRestoresPlacementTest,
    "Mimir.V5.Composite.Break.UndoRestoresPlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHBreakUndoRestoresPlacementTest::RunTest(const FString& Parameters)
{
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem) || GEditor->Trans == nullptr) return false;
    FBreakFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    GEditor->Trans->Reset(INVTEXT("MH Break test start"));
    const TWeakObjectPtr<AMHCompositeActor> Original = Fixture.Actor;
    const int32 DerivedBefore = Fixture.Actor->GetDerivedComponents().Num();
    const int32 LeavesBefore = Fixture.Actor->GetLeafMaterializations().Num();
    const int32 BucketsBefore = CountISM(*Fixture.Actor);
    const uint32 RevisionBefore = Fixture.Actor->GetPreviewRevision();

    TArray<AActor*> Broken;
    TArray<FString> Warnings;
    FString Error;
    bool bPassed = TestTrue(TEXT("Break succeeds: ") + Error, Subsystem->BreakComposites({Fixture.Actor}, Broken, Warnings, Error));
    bPassed &= TestTrue(TEXT("Break is one undoable transaction"), GEditor->Trans->CanUndo());
    bPassed &= TestTrue(TEXT("Undo of Break succeeds"), GEditor->UndoTransaction());

    AMHCompositeActor* Restored = Original.Get();
    if (!TestNotNull(TEXT("original composite actor is back"), Restored) || !TestTrue(TEXT("restored actor is live"), IsValid(Restored) && !Restored->IsActorBeingDestroyed())) return false;
    bPassed &= TestTrue(TEXT("restored actor previews: ") + Restored->GetLastPlacementError(), Restored->GetResolvedPlan() != nullptr);
    bPassed &= TestEqual(TEXT("restored placement has the same derived component count"), Restored->GetDerivedComponents().Num(), DerivedBefore);
    bPassed &= TestEqual(TEXT("restored placement has the same leaf count"), Restored->GetLeafMaterializations().Num(), LeavesBefore);
    bPassed &= TestEqual(TEXT("restored placement has the same bucket count"), CountISM(*Restored), BucketsBefore);
    bPassed &= TestTrue(TEXT("restored placement rebuilt its preview"), Restored->GetPreviewRevision() > RevisionBefore);
    int32 LiveBroken = 0;
    for (AActor* Spawned : Broken) if (IsValid(Spawned) && !Spawned->IsActorBeingDestroyed()) ++LiveBroken;
    bPassed &= TestEqual(TEXT("broken actors are gone after Undo"), LiveBroken, 0);
    // Every derived component belongs to the restored actor exactly once.
    TSet<const UActorComponent*> Seen;
    for (const UActorComponent* Component : Restored->GetDerivedComponents())
    {
        bPassed &= TestTrue(TEXT("derived component is owned by the restored actor"), Component != nullptr && Component->GetOwner() == Restored);
        bPassed &= TestFalse(TEXT("derived component listed once"), Seen.Contains(Component));
        Seen.Add(Component);
    }
    int32 OwnedComponents = 0;
    for (const UActorComponent* Component : Restored->GetComponents())
    {
        if (Component != nullptr && Component != Restored->GetRootComponent()) ++OwnedComponents;
    }
    bPassed &= TestEqual(TEXT("no orphan components survive Undo"), OwnedComponents, DerivedBefore);
    // Field defect (field_r2b3_break_20260903 #3): components restored by the
    // transaction outside the actor's bookkeeping stay registered in the world.
    int32 RegisteredUnderActor = 0;
    int32 UntrackedRegistered = 0;
    ForEachObjectWithOuter(Restored, [&](UObject* Object)
    {
        const UActorComponent* Component = Cast<UActorComponent>(Object);
        if (Component == nullptr || Component == Restored->GetRootComponent() || !Component->IsRegistered()) return;
        ++RegisteredUnderActor;
        if (!Seen.Contains(Component)) ++UntrackedRegistered;
    }, false);
    bPassed &= TestEqual(TEXT("registered components under the actor equal the derived count"), RegisteredUnderActor, DerivedBefore);
    bPassed &= TestEqual(TEXT("no registered component escapes the actor bookkeeping"), UntrackedRegistered, 0);
    int32 WorldISM = 0;
    for (TObjectIterator<UInstancedStaticMeshComponent> It; It; ++It)
    {
        if (It->GetWorld() == Fixture.World && It->IsRegistered()) ++WorldISM;
    }
    bPassed &= TestEqual(TEXT("registered ISM buckets in the world equal the count before Break"), WorldISM, BucketsBefore);

    bPassed &= TestTrue(TEXT("Redo of Break succeeds"), GEditor->RedoTransaction());
    bPassed &= TestTrue(TEXT("Redo retires the composite again"), !IsValid(Original.Get()) || Original->IsActorBeingDestroyed());
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
