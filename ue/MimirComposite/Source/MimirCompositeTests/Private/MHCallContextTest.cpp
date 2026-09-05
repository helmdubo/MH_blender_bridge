#include "MHRecipeTestFixture.h"

#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeRuntimeBridge.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "Composite/MHProofCache.h"
#include "Composite/MHRuntimeCompositeInput.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Random/MHRandomStream.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/**
 * Audit 2026-09-04 §7.A fixture: root = [composite child], child = [random{mesh a w1, mesh b w1}].
 * With equal weights the selected variant depends only on the stream namespace,
 * which is exactly what a Break changes.
 */
struct FCallContextFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    AMHCompositeActor* Actor = nullptr;
    UMHCompositeAsset* Root = nullptr;
    UMHCompositeAsset* Child = nullptr;
    FString MeshA, MeshB;
    const FTransform ActorTransform = FTransform(FRotator(0.0, 20.0, 0.0), FVector(250.0, -50.0, 10.0));

    explicit FCallContextFixture(FAutomationTestBase& Test) : Recipe(Test) {}
    ~FCallContextFixture()
    {
        if (World != nullptr)
        {
            if (GEditor != nullptr && GEditor->Trans != nullptr) GEditor->Trans->Reset(INVTEXT("MH call context teardown"));
            World->DestroyWorld(false);
        }
    }

    bool BuildAssets()
    {
        MeshA = Recipe.Name(TEXT("ctx_mesh_a"));
        MeshB = Recipe.Name(TEXT("ctx_mesh_b"));
        Recipe.Mesh(MeshA);
        Recipe.Mesh(MeshB);
        FMHCompositeDocument ChildDocument;
        {
            FMHCompositeNode& Random = ChildDocument.Nodes.AddDefaulted_GetRef();
            Random.Kind = EMHCompositeNodeKind::Random;
            Random.Transform.TranslationCm = FVector(0.0, 40.0, 0.0);
            for (const FString& Mesh : {MeshA, MeshB})
            {
                FMHCompositeOption& Option = Random.Options.AddDefaulted_GetRef();
                Option.Kind = EMHCompositeOptionKind::Mesh;
                Option.Resource = Mesh;
                Option.Weight = 1.0f;
            }
        }
        Child = Recipe.Composite(Recipe.Name(TEXT("ctx_child")), ChildDocument, {});
        FMHCompositeDocument RootDocument;
        {
            FMHCompositeNode& Nested = RootDocument.Nodes.AddDefaulted_GetRef();
            Nested.Kind = EMHCompositeNodeKind::Composite;
            Nested.Resource = Child != nullptr ? Child->LogicalName : FString();
            Nested.Transform.TranslationCm = FVector(120.0, 0.0, 0.0);
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("ctx_root")), RootDocument, {});
        return Child != nullptr && Root != nullptr;
    }

    bool BuildActor(FAutomationTestBase& Test, const int32 Seed, const int32 AppearanceSeed)
    {
        World = UWorld::CreateWorld(EWorldType::Editor, false);
        if (!Test.TestNotNull(TEXT("call context world"), World)) return false;
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transactional;
        Actor = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), ActorTransform, SpawnParameters);
        if (!Test.TestNotNull(TEXT("call context actor"), Actor)) return false;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(Seed);
        Actor->SetAppearanceSeed(AppearanceSeed);
        Actor->SetCompositeAsset(Root);
        return Test.TestTrue(TEXT("preview builds: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty()) &&
            Test.TestNotNull(TEXT("resident plan"), Actor->GetResolvedPlan());
    }
};

struct FLeafSnapshot
{
    FString Resource;
    FMatrix World;
    float Channels[MH_APPEARANCE_CHANNELS] = {};
};

/** Leaves of Plan whose NodePath lies under PathPrefix, in plan order, in world space of ActorWorld. */
TArray<FLeafSnapshot> LeavesUnder(const FMHResolvedCompositePlan& Plan, const FString& PathPrefix, const FMatrix& ActorWorld)
{
    TArray<FLeafSnapshot> Result;
    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
    {
        if (!PathPrefix.IsEmpty() && !Leaf.Origin.StartsWith(PathPrefix)) continue;
        FLeafSnapshot& Row = Result.AddDefaulted_GetRef();
        Row.Resource = Leaf.Resource;
        Row.World = Leaf.WorldMatrix * ActorWorld;
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel) Row.Channels[Channel] = Leaf.AppearanceChannels[Channel];
    }
    return Result;
}

bool SameLeaves(FAutomationTestBase& Test, const TCHAR* What, const TArray<FLeafSnapshot>& Expected, const TArray<FLeafSnapshot>& Actual)
{
    bool bPassed = Test.TestEqual(FString(What) + TEXT(": leaf count"), Actual.Num(), Expected.Num());
    for (int32 Index = 0; Index < Expected.Num() && Index < Actual.Num(); ++Index)
    {
        bPassed &= Test.TestEqual(FString::Printf(TEXT("%s: leaf %d resource"), What, Index), Actual[Index].Resource, Expected[Index].Resource);
        bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: leaf %d world matrix"), What, Index),
            MHMatrixElementsWithinTrsTolerance(Actual[Index].World, Expected[Index].World));
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        {
            bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: leaf %d appearance channel %d"), What, Index, Channel),
                FMath::IsNearlyEqual(Actual[Index].Channels[Channel], Expected[Index].Channels[Channel], 1.0e-6f));
        }
    }
    return bPassed;
}

} // namespace

// Resolver level (OPEN-R4P-1, owner decision 2026-09-04): resolving the child
// recipe on its own with the call context of its place inside the parent
// (stream namespace = the node path that referenced it, appearance boundary =
// the parent's boundary) reproduces the parent's subtree exactly: same
// decisions, same draws, same appearance. An empty context is today's path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCallContextReproducesSubtreeTest,
    "Mimir.V5.Random.CallContextReproducesSubtree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCallContextReproducesSubtreeTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FCallContextFixture Fixture(*this);
    if (!Fixture.BuildAssets()) return false;
    UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get();
    if (!TestNotNull(TEXT("recipe registry"), Recipes)) return false;
    FString Error;
    const FMHCompiledRecipe* RootRecipe = Recipes->Compile(*Fixture.Root, Error);
    const FMHCompiledRecipe* ChildRecipe = Recipes->Compile(*Fixture.Child, Error);
    if (!TestNotNull(TEXT("root recipe: ") + Error, RootRecipe) || !TestNotNull(TEXT("child recipe: ") + Error, ChildRecipe)) return false;

    bool bPassed = true;
    // Two seeds so that at least one of them picks mesh_b in the parent: the
    // audit's example (seed 0) selects mesh_b inside the root and mesh_a standalone.
    for (const int32 Seed : {0, 17})
    {
        FMHResolvedCompositePlan RootPlan;
        if (!TestTrue(TEXT("root resolves: ") + Error, MHResolveRecipePreview(*RootRecipe, Seed, 34, RootPlan, Error))) return false;
        const FString NestedPath = Fixture.Root->LogicalName + TEXT(":nodes[0]>") + Fixture.Child->LogicalName;
        const TArray<FLeafSnapshot> Expected = LeavesUnder(RootPlan, NestedPath, FMatrix::Identity);
        bPassed &= TestEqual(TEXT("root subtree has one leaf"), Expected.Num(), 1);

        FMHResolveCallContext Context;
        Context.NodePathPrefix = NestedPath;
        Context.AppearanceBoundaryPath = Fixture.Root->LogicalName;
        FMHResolvedCompositePlan ChildPlan;
        if (!TestTrue(TEXT("child resolves with context: ") + Error, MHResolveRecipePreview(*ChildRecipe, Seed, 34, Context, ChildPlan, Error))) return false;
        // The child's leaves are expressed in the child's own space; the parent
        // placed the child at its node transform, so compare through it.
        const FMatrix ChildWorld = RootPlan.Nodes.IsValidIndex(0) ? RootPlan.Nodes[0].WorldMatrix : FMatrix::Identity;
        bPassed &= SameLeaves(*this, *FString::Printf(TEXT("seed %d, child with context"), Seed), Expected, LeavesUnder(ChildPlan, FString(), ChildWorld));
        bPassed &= TestEqual(TEXT("child decision path carries the parent namespace"),
            ChildPlan.Decisions.IsValidIndex(0) ? ChildPlan.Decisions[0].NodePath : FString(), NestedPath + TEXT(":nodes[0]"));
        bPassed &= TestTrue(TEXT("child draw equals the parent draw"),
            RootPlan.Decisions.IsValidIndex(0) && ChildPlan.Decisions.IsValidIndex(0) && RootPlan.Decisions[0].RawU32 == ChildPlan.Decisions[0].RawU32);

        // Without a context the child is its own root: the audit's divergence.
        FMHResolvedCompositePlan Standalone;
        if (!TestTrue(TEXT("child resolves standalone"), MHResolveRecipePreview(*ChildRecipe, Seed, 34, Standalone, Error))) return false;
        bPassed &= TestEqual(TEXT("standalone child keeps today's namespace"),
            Standalone.Decisions.IsValidIndex(0) ? Standalone.Decisions[0].NodePath : FString(), Fixture.Child->LogicalName + TEXT(":nodes[0]"));
    }
    return bPassed;
}

// Break of a nested composite hands the child actor the call context of its
// place in the parent, so the child reproduces the parent's subtree: the same
// selected variant, the same world matrices, the same appearance channels.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHBreakNestedCompositeReproducesResultTest,
    "Mimir.V5.Composite.Break.NestedCompositeReproducesResult",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHBreakNestedCompositeReproducesResultTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    bool bPassed = true;
    for (const int32 Seed : {0, 17})
    {
        FCallContextFixture Fixture(*this);
        if (!Fixture.BuildAssets() || !Fixture.BuildActor(*this, Seed, 34)) return false;
        const FString NestedPath = Fixture.Root->LogicalName + TEXT(":nodes[0]>") + Fixture.Child->LogicalName;
        const TArray<FLeafSnapshot> Expected = LeavesUnder(*Fixture.Actor->GetResolvedPlan(), NestedPath, Fixture.ActorTransform.ToMatrixWithScale());
        bPassed &= TestEqual(TEXT("parent renders one nested leaf"), Expected.Num(), 1);

        TArray<AActor*> Broken;
        TArray<FString> Warnings;
        FString Error;
        if (!TestTrue(TEXT("Break succeeds: ") + Error, Subsystem->BreakComposites({Fixture.Actor}, Broken, Warnings, Error))) return false;
        AMHCompositeActor* ChildActor = Broken.Num() == 1 ? Cast<AMHCompositeActor>(Broken[0]) : nullptr;
        if (!TestNotNull(TEXT("Break emits the nested composite actor"), ChildActor)) { bPassed = false; continue; }
        bPassed &= TestEqual(TEXT("child actor stream namespace"), ChildActor->GetCallContext().StreamNamespace, NestedPath);
        bPassed &= TestEqual(TEXT("child actor appearance boundary"), ChildActor->GetCallContext().AppearanceBoundary, Fixture.Root->LogicalName);
        bPassed &= TestTrue(TEXT("child previews: ") + ChildActor->GetLastPlacementError(), ChildActor->GetResolvedPlan() != nullptr);
        if (ChildActor->GetResolvedPlan() != nullptr)
        {
            bPassed &= SameLeaves(*this, *FString::Printf(TEXT("seed %d, broken child"), Seed), Expected,
                LeavesUnder(*ChildActor->GetResolvedPlan(), FString(), ChildActor->GetActorTransform().ToMatrixWithScale()));
        }

        // The context is the actor's record: proof and the runtime transport
        // resolve through it as well, so no plane diverges from the preview.
        UMHProofCacheSubsystem* Proofs = UMHProofCacheSubsystem::Get();
        FMHProofResult Proof;
        Error.Reset();
        if (TestNotNull(TEXT("proof cache"), Proofs) && TestTrue(TEXT("child proof builds: ") + Error, Proofs->BuildProofNow(*ChildActor, Proof, Error)) && Proof.Plan.IsValid())
        {
            bPassed &= SameLeaves(*this, TEXT("child proof plan"), Expected,
                LeavesUnder(*Proof.Plan, FString(), ChildActor->GetActorTransform().ToMatrixWithScale()));
        }
        else
        {
            bPassed = false;
        }
        FMHRuntimeCompositeInput Input;
        Error.Reset();
        if (TestTrue(TEXT("child runtime input builds: ") + Error, MHBuildRuntimeCompositeInput(*ChildActor, Input, Error)))
        {
            bPassed &= TestEqual(TEXT("runtime input carries the stream namespace"), Input.CallContextNodePathPrefix, NestedPath);
            bPassed &= TestEqual(TEXT("runtime input carries the appearance boundary"), Input.CallContextAppearanceBoundary, Fixture.Root->LogicalName);
        }
        else
        {
            bPassed = false;
        }
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
