#include "MHRecipeTestFixture.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Performance/MHPerformanceTrace.h"
#include "Source/MHSourceResolver.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Root composite with a fixed mesh, a profiled random node over a mesh, a nested composite and an empty option. */
struct FActorPreviewFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    AMHCompositeActor* Actor = nullptr;
    UMHCompositeAsset* Root = nullptr;
    FString MeshA;
    FString MeshB;
    FString MeshC;
    FString ChildName;

    explicit FActorPreviewFixture(FAutomationTestBase& Test) : Recipe(Test) {}

    ~FActorPreviewFixture()
    {
        if (World != nullptr) World->DestroyWorld(true);
    }

    bool Build(FAutomationTestBase& Test)
    {
        MeshA = Recipe.Name(TEXT("preview_mesh_a"));
        MeshB = Recipe.Name(TEXT("preview_mesh_b"));
        MeshC = Recipe.Name(TEXT("preview_mesh_c"));
        Recipe.Mesh(MeshA);
        Recipe.Mesh(MeshB);
        Recipe.Mesh(MeshC);
        FMHCompositeDocument ChildDocument;
        {
            FMHCompositeNode& Leaf = ChildDocument.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshC;
            Leaf.Transform.TranslationCm = FVector(0.0, 50.0, 0.0);
        }
        ChildName = Recipe.Name(TEXT("preview_child_cmp"));
        if (Recipe.Composite(ChildName, ChildDocument, {}) == nullptr) return false;
        FMHPlacementProfile Profile;
        Profile.LogicalName = TEXT("preview_profile");
        Profile.bHasOffsetCm = true;
        Profile.OffsetCm = {{10.0f, 5.0f}, {0.0f, 2.0f}, {-3.0f, 1.0f}};
        Profile.bHasUniformScale = true;
        Profile.UniformScale = {1.0f, 0.25f};
        FMHCompositeDocument RootDocument;
        {
            FMHCompositeNode& Group = RootDocument.Nodes.AddDefaulted_GetRef();
            Group.Name = TEXT("group");
            Group.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
            FMHCompositeNode& Anchor = Group.Children.AddDefaulted_GetRef();
            Anchor.Kind = EMHCompositeNodeKind::Mesh;
            Anchor.Resource = MeshA;
            FMHCompositeNode& Random = RootDocument.Nodes.AddDefaulted_GetRef();
            Random.Kind = EMHCompositeNodeKind::Random;
            Random.Profile = Profile.LogicalName;
            Random.Options.Add({EMHCompositeOptionKind::Mesh, MeshB, 1.0f});
            Random.Options.Add({EMHCompositeOptionKind::Composite, ChildName, 2.0f});
            Random.Options.Add({EMHCompositeOptionKind::Empty, FString(), 0.5f});
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("preview_root_cmp")), RootDocument, {Profile});
        if (Root == nullptr) return false;
        World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
        if (!Test.TestNotNull(TEXT("preview world"), World)) return false;
        Actor = World->SpawnActor<AMHCompositeActor>();
        if (!Test.TestNotNull(TEXT("composite actor"), Actor)) return false;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(7);
        Actor->SetAppearanceSeed(11);
        return true;
    }
};

FMHResourceKey PreviewMeshKey(const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::StaticMesh;
    Key.LogicalName = Name;
    return Key;
}

} // namespace

// Map load (the single build point) builds the preview from the compiled
// recipe: no applied graph, no closure, no definition cache (§2.6, §4).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHActorPreviewNoProofOnLoadTest,
    "Mimir.V5.Composite.Recipe.ActorPreviewNoProofOnLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHActorPreviewNoProofOnLoadTest::RunTest(const FString& Parameters)
{
    FActorPreviewFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    MHResetPlacementStageMetrics();
    MHResetDefinitionCacheMetrics();
    Fixture.Actor->SetCompositeAsset(Fixture.Root);
    {
        FMHMapLoadInitialBuildScope Scope(*Fixture.Actor);
        Fixture.Actor->RebuildComposite();
        Scope.Complete(*Fixture.Actor);
    }
    bool bPassed = TestTrue(TEXT("preview builds without error: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty());
    // Seed 7 may select the empty option: the fixed anchor leaf is the floor.
    bPassed &= TestTrue(TEXT("leaves materialized"), Fixture.Actor->GetLeafMaterializations().Num() >= 1);
    const FMHPlacementStageMetrics Stages = MHGetPlacementStageMetrics();
    bPassed &= TestEqual(TEXT("no applied graph (proof plane) on map load"), Stages.Get(EMHPlacementStage::BuildAppliedGraph).Calls, 0ull);
    bPassed &= TestTrue(TEXT("layout ran once"), Stages.Get(EMHPlacementStage::ResolveCompositePlan).Calls >= 1);
    // The definition-cache counters now report the recipe cache: one miss
    // (the compile behind SetCompositeAsset) and hits for every later build.
    const FMHDefinitionCacheMetrics Cache = MHGetDefinitionCacheMetrics();
    bPassed &= TestEqual(TEXT("recipe compiled exactly once"), Cache.Misses, 1ull);
    bPassed &= TestTrue(TEXT("later builds hit the recipe cache"), Cache.Hits >= 1ull);
    bPassed &= TestEqual(TEXT("no closure build behind a cache hit"), Cache.ClosureHitBuilds, 0ull);
    bPassed &= TestTrue(TEXT("resolved plan is resident"), Fixture.Actor->GetResolvedPlan() != nullptr);
    return bPassed;
}

// Moving the actor updates the basis of the resident plan: no Layout, no proof.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHActorMoveNoLayoutTest,
    "Mimir.V5.Composite.Recipe.ActorMoveNoLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHActorMoveNoLayoutTest::RunTest(const FString& Parameters)
{
    FActorPreviewFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    Fixture.Actor->SetCompositeAsset(Fixture.Root);
    Fixture.Actor->RebuildComposite();
    if (!TestTrue(TEXT("preview builds: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty())) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = Fixture.Actor->GetTopLevelPlacementComponents();
    if (!TestTrue(TEXT("authored handles exist"), Handles.Num() >= 1 && Handles[0] != nullptr)) return false;
    const FVector Before = Handles[0]->GetComponentLocation();
    const uint32 Rebuilds = Fixture.Actor->GetPlacementRebuildCount();
    MHResetPlacementStageMetrics();
    const FVector Delta(250.0, -75.0, 30.0);
    Fixture.Actor->SetActorLocation(Fixture.Actor->GetActorLocation() + Delta);
    bool bPassed = TestTrue(TEXT("handle moved with the actor"), Handles[0]->GetComponentLocation().Equals(Before + Delta, 1e-3));
    const FMHPlacementStageMetrics Stages = MHGetPlacementStageMetrics();
    bPassed &= TestEqual(TEXT("move runs no layout"), Stages.Get(EMHPlacementStage::ResolveCompositePlan).Calls, 0ull);
    bPassed &= TestEqual(TEXT("move runs no proof"), Stages.Get(EMHPlacementStage::BuildAppliedGraph).Calls, 0ull);
    bPassed &= TestEqual(TEXT("move is not a rebuild"), Fixture.Actor->GetPlacementRebuildCount(), Rebuilds);
    bPassed &= TestTrue(TEXT("no error after move: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty());
    return bPassed;
}

// Reimport notifications reach the actor through the recipe dependency set,
// nested composites included; unrelated keys do not rebuild it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHActorRecipeDependentsTest,
    "Mimir.V5.Composite.Recipe.ActorReimportViaRecipeDependents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHActorRecipeDependentsTest::RunTest(const FString& Parameters)
{
    FActorPreviewFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    Fixture.Actor->SetCompositeAsset(Fixture.Root);
    Fixture.Actor->RebuildComposite();
    if (!TestTrue(TEXT("preview builds: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty())) return false;
    bool bPassed = TestTrue(TEXT("depends on the nested composite mesh"), Fixture.Actor->DependsOnResource(PreviewMeshKey(Fixture.MeshC)));
    bPassed &= TestFalse(TEXT("does not depend on a foreign mesh"), Fixture.Actor->DependsOnResource(PreviewMeshKey(Fixture.Recipe.Name(TEXT("foreign_mesh")))));
    const uint32 Rebuilds = Fixture.Actor->GetPlacementRebuildCount();
    MHNotifyGeneratedResourceChanged(PreviewMeshKey(Fixture.MeshC));
    bPassed &= TestEqual(TEXT("nested mesh change rebuilds the placement"), Fixture.Actor->GetPlacementRebuildCount(), Rebuilds + 1);
    MHNotifyGeneratedResourceChanged(PreviewMeshKey(Fixture.Recipe.Name(TEXT("foreign_mesh"))));
    bPassed &= TestEqual(TEXT("foreign change leaves the placement alone"), Fixture.Actor->GetPlacementRebuildCount(), Rebuilds + 1);
    bPassed &= TestTrue(TEXT("no error after notifications: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty());
    return bPassed;
}

// R2b-3 (Recipe Model v2 §2.10, §7.2): the actor carries no proof state. No
// signature property, no definition cache subsystem, no compact signed state;
// the resident preview plan and the proof cache are the only two authorities.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHActorHasNoProofStateTest,
    "Mimir.V5.Composite.Recipe.ActorHasNoProofState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHActorHasNoProofStateTest::RunTest(const FString& Parameters)
{
    bool bPassed = TestNull(TEXT("actor exposes no ResolvedSignature property"),
        FindFProperty<FProperty>(AMHCompositeActor::StaticClass(), TEXT("ResolvedSignature")));
    bPassed &= TestNull(TEXT("definition cache subsystem class is gone"),
        FindFirstObject<UClass>(TEXT("MHCompositeDefinitionSubsystem"), EFindFirstObjectOptions::ExactClass));
    FActorPreviewFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    Fixture.Actor->SetCompositeAsset(Fixture.Root);
    Fixture.Actor->RebuildComposite();
    bPassed &= TestTrue(TEXT("preview builds: ") + Fixture.Actor->GetLastPlacementError(), Fixture.Actor->GetLastPlacementError().IsEmpty());
    const FMHResolvedCompositePlan* Plan = Fixture.Actor->GetResolvedPlan();
    bPassed &= TestNotNull(TEXT("resident preview plan"), Plan);
    if (Plan != nullptr)
    {
        bPassed &= TestTrue(TEXT("resident plan carries no closure or signature"),
            Plan->Closure.Resources.IsEmpty() && Plan->ResolvedSignature.IsEmpty() && Plan->PlacementSignature.IsEmpty());
        bPassed &= TestEqual(TEXT("resident plan seed"), Plan->Seed, Fixture.Actor->GetSeed());
    }
    bPassed &= TestTrue(TEXT("preview revision advanced"), Fixture.Actor->GetPreviewRevision() >= 1u);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
