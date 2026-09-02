#include "MHGoldenRoot.h"
#include "MHRecipeTestFixture.h"

#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "Composite/MHMaterializeLayout.h"
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"

namespace UE::MimirComposite::Tests
{
namespace
{

FString LeafResourceKey(const EMHRandomSemanticKind Kind, const FString& Resource)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh: return TEXT("static_mesh:") + Resource;
    case EMHRandomSemanticKind::Actor: return TEXT("actor:") + Resource;
    case EMHRandomSemanticKind::GameObj: return TEXT("gameobj:") + Resource;
    default: return FString();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// R2b-1 (Recipe Model v2 §2.5): MHMaterializeLayout is a pure function over a
// compiled recipe. Its leaves must equal the reference wrapper's leaves times
// the actor transform on every golden fixture, it must succeed without any
// endpoint asset existing, and it must refuse an unrepresentable transform.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterializeLayoutParityTest,
    "Mimir.V5.Composite.Recipe.MaterializeLayoutParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterializeLayoutParityTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    UMHCompiledRecipeRegistry* Registry = UMHCompiledRecipeRegistry::Get();
    if (Registry == nullptr)
    {
        AddError(TEXT("compiled recipe registry subsystem is missing"));
        return false;
    }

    struct FCase
    {
        FString Label;
        TSharedPtr<FJsonObject> Fixture;
        TArray<int32> Seeds;
    };
    TArray<FCase> Cases;
    TSharedPtr<FJsonObject> Shared;
    if (!RecipeLoadJson(*this, FPaths::Combine(GoldenRoot, TEXT("v5/random_stream_1_vectors.json")), Shared)) return false;
    {
        FCase& Case = Cases.AddDefaulted_GetRef();
        Case.Label = TEXT("random_stream_1");
        const TSharedPtr<FJsonObject>* FixtureObject = nullptr;
        if (!Shared->TryGetObjectField(TEXT("fixture"), FixtureObject) || FixtureObject == nullptr ||
            !RecipeReadSeeds(Shared, TEXT("seed_set"), Case.Seeds)) return false;
        Case.Fixture = *FixtureObject;
    }
    TSharedPtr<FJsonObject> Appearance;
    if (!RecipeLoadJson(*this, FPaths::Combine(GoldenRoot, TEXT("v5/appearance/appearance_1_vectors.json")), Appearance)) return false;
    TArray<int32> LayoutSeeds;
    if (!RecipeReadSeeds(Appearance, TEXT("seed_set"), LayoutSeeds)) return false;
    const TArray<TSharedPtr<FJsonValue>>* Scenarios = nullptr;
    if (!Appearance->TryGetArrayField(TEXT("scenarios"), Scenarios) || Scenarios == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Scenarios)
    {
        const TSharedPtr<FJsonObject> Scenario = Value->AsObject();
        const TSharedPtr<FJsonObject>* FixtureObject = nullptr;
        FCase& Case = Cases.AddDefaulted_GetRef();
        if (!Scenario.IsValid() || !Scenario->TryGetStringField(TEXT("name"), Case.Label) ||
            !Scenario->TryGetObjectField(TEXT("fixture"), FixtureObject) || FixtureObject == nullptr) return false;
        Case.Fixture = *FixtureObject;
        Case.Seeds = LayoutSeeds;
    }

    // Identity, a rotated translation, and a non-uniform scale with a negative
    // yaw: the actor transform must multiply every leaf, never be ignored.
    const TArray<FTransform> ActorTransforms = {
        FTransform::Identity,
        FTransform(FRotator(0.0, 37.5, 0.0), FVector(1234.0, -560.0, 42.0)),
        FTransform(FRotator(10.0, -120.0, 5.0), FVector(-30.0, 0.0, 250.0), FVector(2.0, 0.5, 1.25)),
    };

    bool bPassed = true;
    int32 Compared = 0;
    for (const FCase& Case : Cases)
    {
        FMHRandomSourceGraph Graph;
        if (!RecipeReadFixture(Case.Fixture, Graph))
        {
            AddError(Case.Label + TEXT(": fixture is malformed"));
            return false;
        }
        FRecipeFixture Fixture(*this);
        const FMHRandomSourceGraph Renamed = Fixture.Rename(Graph);
        // Purity: no endpoint asset of the fixture exists (no mesh, no actor
        // class); only the composite assets do. Materialization must not care.
        UMHCompositeAsset* RootAsset = Fixture.Build(Renamed);
        if (RootAsset == nullptr) return false;
        FString Error;
        const FMHCompiledRecipe* Recipe = Registry->Compile(*RootAsset, Error);
        if (!TestNotNull(Case.Label + TEXT(": recipe compiles: ") + Error, Recipe))
        {
            bPassed = false;
            continue;
        }
        for (const int32 Seed : Case.Seeds)
        {
            const int32 AppearanceSeed = Seed ^ 0x5A5A;
            FMHResolvedCompositePlan Reference;
            Error.Reset();
            if (!TestTrue(Case.Label + TEXT(": reference resolves: ") + Error, MHResolveCompositePlan(Renamed, Seed, AppearanceSeed, Reference, Error)))
            {
                bPassed = false;
                continue;
            }
            for (int32 TransformIndex = 0; TransformIndex < ActorTransforms.Num(); ++TransformIndex)
            {
                const FTransform& ActorTransform = ActorTransforms[TransformIndex];
                const FString Label = FString::Printf(TEXT("%s seed %d transform %d"), *Case.Label, Seed, TransformIndex);
                const FMHMaterializeResult Result = MHMaterializeLayout(*Recipe, Seed, AppearanceSeed, ActorTransform);
                if (!TestTrue(Label + TEXT(": materializes: ") + Result.Error, Result.Succeeded()))
                {
                    bPassed = false;
                    continue;
                }
                bPassed &= TestEqual(Label + TEXT(": seed"), Result.Seed, Seed);
                bPassed &= TestEqual(Label + TEXT(": appearance seed"), Result.AppearanceSeed, AppearanceSeed);
                if (!TestEqual(Label + TEXT(": leaf count"), Result.Placements.Num(), Reference.Leaves.Num()))
                {
                    bPassed = false;
                    continue;
                }
                const FMatrix ActorMatrix = ActorTransform.ToMatrixWithScale();
                for (int32 Index = 0; Index < Reference.Leaves.Num(); ++Index)
                {
                    const FMHResolvedCompositeLeaf& Leaf = Reference.Leaves[Index];
                    const FMHLeafPlacement& Placement = Result.Placements[Index];
                    const FString Where = FString::Printf(TEXT("%s leaf %d"), *Label, Index);
                    bPassed &= TestTrue(Where + TEXT(": kind"), Placement.Kind == Leaf.Kind);
                    bPassed &= TestEqual(Where + TEXT(": resource"), Placement.Resource, Leaf.Resource);
                    bPassed &= TestEqual(Where + TEXT(": resource key"), Placement.ResourceKey, LeafResourceKey(Leaf.Kind, Leaf.Resource));
                    bPassed &= TestEqual(Where + TEXT(": node path"), Placement.NodePath, Leaf.Origin);
                    bPassed &= TestEqual(Where + TEXT(": display name"), Placement.DisplayName, Leaf.DisplayName);
                    bPassed &= TestEqual(Where + TEXT(": root node index"), Placement.RootNodeIndex, Leaf.RootNodeIndex);
                    bPassed &= TestEqual(Where + TEXT(": owning node index"), Placement.OwningResolvedNodeIndex, Leaf.OwningResolvedNodeIndex);
                    bPassed &= TestEqual(Where + TEXT(": boundary"), Placement.AppearanceBoundaryPath, Leaf.AppearanceBoundaryPath);
                    bPassed &= TestTrue(Where + TEXT(": world matrix"), Placement.WorldMatrix.Equals(Leaf.WorldMatrix * ActorMatrix, 0.0));
                    bPassed &= TestFalse(Where + TEXT(": not overridden before R6"), Placement.bOverridden);
                    for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
                    {
                        bPassed &= TestTrue(*FString::Printf(TEXT("%s: channel %d"), *Where, Channel),
                            Placement.AppearanceChannels[Channel] == Leaf.AppearanceChannels[Channel]);
                    }
                }
                if (TestTrue(Label + TEXT(": resident plan"), Result.Plan.IsValid()))
                {
                    bPassed &= TestTrue(Label + TEXT(": resident plan has no closure"),
                        Result.Plan->Closure.ClosureHash.IsEmpty() && Result.Plan->Closure.Resources.Num() == 0);
                    bPassed &= TestTrue(Label + TEXT(": resident plan has no signature"),
                        Result.Plan->ResolvedSignature.IsEmpty() && Result.Plan->PlacementSignature.IsEmpty());
                    bPassed &= TestEqual(Label + TEXT(": resident plan decisions"), Result.Plan->Decisions.Num(), Reference.Decisions.Num());
                }
                else
                {
                    bPassed = false;
                }
                ++Compared;
            }
        }
    }
    bPassed &= TestTrue(TEXT("parity covered the golden matrix"), Compared >= 5 * 7 * 3);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterializeLayoutAdmissionTest,
    "Mimir.V5.Composite.Recipe.MaterializeLayoutAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterializeLayoutAdmissionTest::RunTest(const FString& Parameters)
{
    UMHCompiledRecipeRegistry* Registry = UMHCompiledRecipeRegistry::Get();
    if (Registry == nullptr)
    {
        AddError(TEXT("compiled recipe registry subsystem is missing"));
        return false;
    }
    FRecipeFixture Fixture(*this);
    FMHCompositeDocument Document;
    {
        FMHCompositeNode& Group = Document.Nodes.AddDefaulted_GetRef();
        Group.Name = TEXT("group");
        Group.Transform.TranslationCm = FVector(10.0, 20.0, 30.0);
        FMHCompositeNode& Leaf = Group.Children.AddDefaulted_GetRef();
        Leaf.Kind = EMHCompositeNodeKind::Mesh;
        Leaf.Resource = Fixture.Name(TEXT("materialize_mesh"));
        Leaf.Transform.Scale = FVector(1.0, 2.0, 3.0);
    }
    UMHCompositeAsset* Asset = Fixture.Composite(Fixture.Name(TEXT("materialize_cmp")), Document, {});
    if (Asset == nullptr) return false;
    FString Error;
    const FMHCompiledRecipe* Recipe = Registry->Compile(*Asset, Error);
    if (!TestNotNull(TEXT("recipe compiles: ") + Error, Recipe)) return false;

    bool bPassed = true;
    // A representable transform materializes exactly one leaf.
    const FMHMaterializeResult Good = MHMaterializeLayout(*Recipe, 3, 4, FTransform(FVector(100.0, 0.0, 0.0)));
    bPassed &= TestTrue(TEXT("representable transform materializes: ") + Good.Error, Good.Succeeded());
    bPassed &= TestEqual(TEXT("one leaf"), Good.Placements.Num(), 1);
    if (Good.Placements.Num() == 1)
    {
        bPassed &= TestTrue(TEXT("leaf translation includes the actor"),
            Good.Placements[0].WorldMatrix.GetOrigin().Equals(FVector(110.0, 20.0, 30.0), 1e-6));
    }
    // The same admission rule as the placement compiler: a rotated actor with
    // a non-uniform scale shears the non-uniformly scaled leaf, and a sheared
    // matrix cannot round-trip through FTransform.
    const FTransform Unrepresentable(FRotator(0.0, 45.0, 0.0), FVector::ZeroVector, FVector(2.0, 1.0, 1.0));
    FString AdmissionError;
    FMHResolvedCompositePlan Probe;
    if (Good.Plan.IsValid()) Probe = *Good.Plan;
    const bool bReferenceRefuses = !MHValidateResolvedPlacementTransforms(Probe, Unrepresentable, AdmissionError);
    const FMHMaterializeResult Bad = MHMaterializeLayout(*Recipe, 3, 4, Unrepresentable);
    bPassed &= TestTrue(TEXT("reference admission refuses the probe transform"), bReferenceRefuses);
    bPassed &= TestFalse(TEXT("unrepresentable transform is refused"), Bad.Succeeded());
    bPassed &= TestTrue(TEXT("refusal names the transform code: ") + Bad.Error, Bad.Error.Contains(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM")));
    bPassed &= TestEqual(TEXT("refusal materializes nothing"), Bad.Placements.Num(), 0);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
