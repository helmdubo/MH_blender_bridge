#include "MHGoldenRoot.h"
#include "MHRecipeTestFixture.h"

#include "Canonical/MHCanonical.h"
#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

FMHResourceKey RecipeKey(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = Kind;
    Key.LogicalName = Name;
    return Key;
}

bool ContainsAsset(const TArray<TWeakObjectPtr<const UMHCompositeAsset>>& Assets, const UMHCompositeAsset* Asset)
{
    return Assets.ContainsByPredicate([Asset](const TWeakObjectPtr<const UMHCompositeAsset>& Value) { return Value.Get() == Asset; });
}

} // namespace

// ---------------------------------------------------------------------------
// Acceptance 1-5: the flat program of the shared golden fixture.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompiledRecipeStructureTest,
    "Mimir.V5.Composite.Recipe.CompiledRecipeStructure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompiledRecipeStructureTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    TSharedPtr<FJsonObject> Root;
    if (!RecipeLoadJson(*this, FPaths::Combine(GoldenRoot, TEXT("v5/random_stream_1_vectors.json")), Root)) return false;
    const TSharedPtr<FJsonObject>* FixtureObject = nullptr;
    FMHRandomSourceGraph Graph;
    if (!Root->TryGetObjectField(TEXT("fixture"), FixtureObject) || FixtureObject == nullptr || !RecipeReadFixture(*FixtureObject, Graph))
    {
        AddError(TEXT("shared resolver fixture is malformed"));
        return false;
    }
    UMHCompiledRecipeRegistry* Registry = UMHCompiledRecipeRegistry::Get();
    if (Registry == nullptr)
    {
        AddError(TEXT("compiled recipe registry subsystem is missing"));
        return false;
    }

    FRecipeFixture Fixture(*this);
    const FMHRandomSourceGraph Renamed = Fixture.Rename(Graph);
    UMHCompositeAsset* RootAsset = Fixture.Build(Renamed);
    if (RootAsset == nullptr) return false;
    UMHCompositeAsset* VariantA = Fixture.Composites.FindRef(Fixture.Name(TEXT("variant_a_cmp")));
    UMHCompositeAsset* VariantB = Fixture.Composites.FindRef(Fixture.Name(TEXT("variant_b_cmp")));
    if (VariantA == nullptr || VariantB == nullptr) return false;

    bool bPassed = true;
    FString Error;
    const uint32 GenerationBefore = Registry->GetGeneration();
    const FMHCompiledRecipe* Recipe = Registry->Compile(*RootAsset, Error);
    if (!TestNotNull(TEXT("root recipe compiles: ") + Error, Recipe)) return false;

    // 1. DFS components, subtree intervals, raw weights, canonical TRS, NodePath grammar.
    const FString RootName = RootAsset->LogicalName;
    bPassed &= TestEqual(TEXT("recipe names its asset"), Recipe->LogicalName, RootName);
    bPassed &= TestTrue(TEXT("recipe handle points at the asset"), Recipe->Asset.Get() == RootAsset);
    if (!TestEqual(TEXT("four components in DFS order"), Recipe->Components.Num(), 4)) return false;
    const TArray<FMHCompiledRecipeComponent>& Components = Recipe->Components;
    bPassed &= TestTrue(TEXT("[0] group"), Components[0].Kind == EMHRandomSemanticKind::Group);
    bPassed &= TestEqual(TEXT("[0] path"), Components[0].NodePath, RootName + TEXT(":nodes[0]"));
    bPassed &= TestEqual(TEXT("[0] begin"), Components[0].BeginInd, 0);
    bPassed &= TestEqual(TEXT("[0] end"), Components[0].EndInd, 2);
    bPassed &= TestEqual(TEXT("[0] parent"), Components[0].ParentIndex, static_cast<int32>(INDEX_NONE));
    bPassed &= TestTrue(TEXT("[0] authored translation"), Components[0].AuthoredTrs.TranslationCm == FVector3f(100.0f, 0.0f, 0.0f));
    bPassed &= TestTrue(TEXT("[0] fixed transform"), Components[0].TransformKind == EMHCompiledTransformKind::Matrix);
    bPassed &= TestTrue(TEXT("[1] mesh"), Components[1].Kind == EMHRandomSemanticKind::Mesh);
    bPassed &= TestEqual(TEXT("[1] resource"), Components[1].Resource, FString(TEXT("anchor_mesh")));
    bPassed &= TestEqual(TEXT("[1] resource key"), Components[1].ResourceKey, FString(TEXT("static_mesh:anchor_mesh")));
    bPassed &= TestEqual(TEXT("[1] path"), Components[1].NodePath, RootName + TEXT(":nodes[0]/children[0]"));
    bPassed &= TestEqual(TEXT("[1] begin"), Components[1].BeginInd, 1);
    bPassed &= TestEqual(TEXT("[1] end"), Components[1].EndInd, 2);
    bPassed &= TestEqual(TEXT("[1] parent"), Components[1].ParentIndex, 0);
    bPassed &= TestTrue(TEXT("[2] random"), Components[2].Kind == EMHRandomSemanticKind::Random);
    bPassed &= TestEqual(TEXT("[2] path"), Components[2].NodePath, RootName + TEXT(":nodes[1]"));
    bPassed &= TestEqual(TEXT("[2] begin"), Components[2].BeginInd, 2);
    bPassed &= TestEqual(TEXT("[2] end"), Components[2].EndInd, 4);
    bPassed &= TestEqual(TEXT("[2] profile"), Components[2].ProfileName, FString(TEXT("full_profile")));
    bPassed &= TestTrue(TEXT("[2] ranged transform"), Components[2].TransformKind == EMHCompiledTransformKind::Ranges);
    if (TestEqual(TEXT("[2] three raw options"), Components[2].Options.Num(), 3))
    {
        bPassed &= TestTrue(TEXT("[2] option 0 empty"), Components[2].Options[0].Kind == EMHRandomSemanticKind::Empty);
        bPassed &= TestEqual(TEXT("[2] option 0 raw weight"), Components[2].Options[0].WeightRaw, 0.0f);
        bPassed &= TestTrue(TEXT("[2] option 1 composite"), Components[2].Options[1].Kind == EMHRandomSemanticKind::Composite);
        bPassed &= TestEqual(TEXT("[2] option 1 raw weight"), Components[2].Options[1].WeightRaw, 1.0f);
        bPassed &= TestEqual(TEXT("[2] option 2 raw weight"), Components[2].Options[2].WeightRaw, 3.0f);
        bPassed &= TestEqual(TEXT("[2] option 2 resource key"), Components[2].Options[2].ResourceKey, TEXT("composite:") + VariantB->LogicalName);
        // 2. Nested composites are references by handle, resolved transitively.
        const int32 RefA = Components[2].Options[1].NestedRecipe;
        const int32 RefB = Components[2].Options[2].NestedRecipe;
        if (TestTrue(TEXT("nested options carry reference handles"), Recipe->References.IsValidIndex(RefA) && Recipe->References.IsValidIndex(RefB)))
        {
            bPassed &= TestTrue(TEXT("reference a is variant_a"), Recipe->References[RefA].Asset.Get() == VariantA);
            bPassed &= TestTrue(TEXT("reference b is variant_b"), Recipe->References[RefB].Asset.Get() == VariantB);
            bPassed &= TestEqual(TEXT("reference a revision"), Recipe->References[RefA].RecipeRevision, Registry->GetRecipeRevision(*VariantA));
        }
    }
    else
    {
        bPassed = false;
    }
    bPassed &= TestEqual(TEXT("[3] path"), Components[3].NodePath, RootName + TEXT(":nodes[1]/children[0]"));
    bPassed &= TestEqual(TEXT("[3] begin"), Components[3].BeginInd, 3);
    bPassed &= TestEqual(TEXT("[3] end"), Components[3].EndInd, 4);
    bPassed &= TestEqual(TEXT("[3] parent"), Components[3].ParentIndex, 2);
    bPassed &= TestEqual(TEXT("[3] profile"), Components[3].ProfileName, FString(TEXT("offset_only")));
    bPassed &= TestEqual(TEXT("two inlined profiles"), Recipe->Profiles.Num(), 2);
    bPassed &= TestEqual(TEXT("two references"), Recipe->References.Num(), 2);
    bPassed &= TestTrue(TEXT("root recipe is generated"), Recipe->bGenerated);
    const FMHCompiledRecipe* RecipeA = Registry->Find(*VariantA);
    const FMHCompiledRecipe* RecipeB = Registry->Find(*VariantB);
    bPassed &= TestNotNull(TEXT("variant_a compiled transitively"), RecipeA);
    bPassed &= TestNotNull(TEXT("variant_b compiled transitively"), RecipeB);
    if (RecipeA != nullptr) bPassed &= TestFalse(TEXT("variant_a has nothing a seed can change"), RecipeA->bGenerated);
    if (RecipeB != nullptr) bPassed &= TestTrue(TEXT("variant_b is generated"), RecipeB->bGenerated);
    bPassed &= TestTrue(TEXT("compilation advanced the generation"), Registry->GetGeneration() > GenerationBefore);
    bPassed &= TestTrue(TEXT("seed classification of the root is topology"), Registry->GetSeedAffectsResult(*RootAsset, Error) == EMHCompositeSeedEffect::Topology);

    // NodePath grammar agrees with the resolver on the same recipe.
    FMHResolvedCompositePlan Plan;
    if (TestTrue(TEXT("preview resolves the compiled root: ") + Error, MHResolveRecipePreview(*Recipe, 0, 0, Plan, Error)) && Plan.Nodes.Num() >= 3)
    {
        bPassed &= TestEqual(TEXT("resolver node 0 path"), Plan.Nodes[0].NodePath, Components[0].NodePath);
        bPassed &= TestEqual(TEXT("resolver node 1 path"), Plan.Nodes[1].NodePath, Components[1].NodePath);
        bPassed &= TestEqual(TEXT("resolver node 2 path"), Plan.Nodes[2].NodePath, Components[2].NodePath);
    }
    else
    {
        bPassed = false;
    }

    // 3. RecipeRevision: PostEditChange invalidates; the cache key is asset + revision.
    const uint32 RevisionBefore = Registry->GetRecipeRevision(*RootAsset);
    bPassed &= TestEqual(TEXT("compiled recipe carries the current revision"), Recipe->RecipeRevision, RevisionBefore);
    bPassed &= TestTrue(TEXT("cached recipe is found while current"), Registry->Find(*RootAsset) == Recipe);
    RootAsset->PostEditChange();
    bPassed &= TestEqual(TEXT("PostEditChange bumps the revision"), Registry->GetRecipeRevision(*RootAsset), RevisionBefore + 1);
    bPassed &= TestNull(TEXT("stale recipe is not served"), Registry->Find(*RootAsset));
    const FMHCompiledRecipe* Recompiled = Registry->Compile(*RootAsset, Error);
    if (TestNotNull(TEXT("root recompiles: ") + Error, Recompiled))
    {
        bPassed &= TestEqual(TEXT("recompiled recipe carries the new revision"), Recompiled->RecipeRevision, RevisionBefore + 1);
        bPassed &= TestEqual(TEXT("recompiled program is unchanged"), Recompiled->Components.Num(), 4);
    }
    bPassed &= TestNotNull(TEXT("child recipes survive a parent recompilation"), Registry->Find(*VariantA));

    // 5. Reverse index: rematerialize localization only.
    bPassed &= TestTrue(TEXT("anchor mesh dependents include the root"),
        ContainsAsset(Registry->GetDependents(RecipeKey(EMHResourceKind::StaticMesh, TEXT("anchor_mesh"))), RootAsset));
    const TArray<TWeakObjectPtr<const UMHCompositeAsset>> VariantDependents = Registry->GetDependents(RecipeKey(EMHResourceKind::StaticMesh, TEXT("variant_a_mesh")));
    bPassed &= TestTrue(TEXT("variant mesh dependents include variant_a"), ContainsAsset(VariantDependents, VariantA));
    bPassed &= TestFalse(TEXT("variant mesh dependents do not climb to the root"), ContainsAsset(VariantDependents, RootAsset));
    bPassed &= TestTrue(TEXT("variant_a composite dependents include the root"),
        ContainsAsset(Registry->GetDependents(RecipeKey(EMHResourceKind::Composite, VariantA->LogicalName)), RootAsset));

    // 4. Cycles fail closed with the frozen diagnostic.
    {
        FMHCompositeDocument DocumentA;
        FMHCompositeNode& ToB = DocumentA.Nodes.AddDefaulted_GetRef();
        ToB.Kind = EMHCompositeNodeKind::Composite;
        ToB.Resource = Fixture.Name(TEXT("cycle_b"));
        FMHCompositeDocument DocumentB;
        FMHCompositeNode& ToA = DocumentB.Nodes.AddDefaulted_GetRef();
        ToA.Kind = EMHCompositeNodeKind::Composite;
        ToA.Resource = Fixture.Name(TEXT("cycle_a"));
        UMHCompositeAsset* CycleA = Fixture.Composite(Fixture.Name(TEXT("cycle_a")), DocumentA, {});
        UMHCompositeAsset* CycleB = Fixture.Composite(Fixture.Name(TEXT("cycle_b")), DocumentB, {});
        if (CycleA != nullptr && CycleB != nullptr)
        {
            Error.Reset();
            bPassed &= TestNull(TEXT("cyclic recipe does not compile"), Registry->Compile(*CycleA, Error));
            bPassed &= TestTrue(TEXT("cycle names its diagnostic: ") + Error, Error.Contains(TEXT("MH_E_COMPOSITE_CYCLE")));
        }
        else
        {
            bPassed = false;
        }
    }
    return bPassed;
}

// ---------------------------------------------------------------------------
// Acceptance 6: shadow parity on every golden fixture and seed (§2.3 CI gate).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRecipeShadowParityTest,
    "Mimir.V5.Composite.Recipe.RecipeShadowParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRecipeShadowParityTest::RunTest(const FString& Parameters)
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
        TArray<int32> AppearanceSeeds;
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
        Case.AppearanceSeeds = Case.Seeds;
    }
    TSharedPtr<FJsonObject> Appearance;
    if (!RecipeLoadJson(*this, FPaths::Combine(GoldenRoot, TEXT("v5/appearance/appearance_1_vectors.json")), Appearance)) return false;
    TArray<int32> AppearanceSeeds;
    TArray<int32> LayoutSeeds;
    if (!RecipeReadSeeds(Appearance, TEXT("appearance_seed_set"), AppearanceSeeds) || !RecipeReadSeeds(Appearance, TEXT("seed_set"), LayoutSeeds)) return false;
    {
        const TSharedPtr<FJsonObject>* Synthetic = nullptr;
        const TSharedPtr<FJsonObject>* FixtureObject = nullptr;
        if (!Appearance->TryGetObjectField(TEXT("synthetic"), Synthetic) || Synthetic == nullptr ||
            !(*Synthetic)->TryGetObjectField(TEXT("fixture"), FixtureObject) || FixtureObject == nullptr) return false;
        FCase& Case = Cases.AddDefaulted_GetRef();
        Case.Label = TEXT("appearance_synthetic");
        Case.Fixture = *FixtureObject;
        Case.Seeds = LayoutSeeds;
        Case.AppearanceSeeds = AppearanceSeeds;
    }
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
        Case.AppearanceSeeds = AppearanceSeeds;
    }

    bool bPassed = true;
    int32 Pairs = 0;
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
            for (const int32 AppearanceSeed : Case.AppearanceSeeds)
            {
                const FString Label = FString::Printf(TEXT("%s seed %d appearance %d"), *Case.Label, Seed, AppearanceSeed);
                FMHResolvedCompositePlan Reference;
                FMHResolvedCompositePlan Preview;
                Error.Reset();
                if (!TestTrue(Label + TEXT(": reference wrapper resolves: ") + Error, MHResolveCompositePlan(Renamed, Seed, AppearanceSeed, Reference, Error)))
                {
                    bPassed = false;
                    continue;
                }
                Error.Reset();
                if (!TestTrue(Label + TEXT(": preview path resolves: ") + Error, MHResolveRecipePreview(*Recipe, Seed, AppearanceSeed, Preview, Error)))
                {
                    bPassed = false;
                    continue;
                }
                TArray<FString> Mismatches;
                const bool bParity = MHCompareRecipeShadowParity(Reference, Preview, Mismatches);
                if (!TestTrue(Label + TEXT(": zero shadow mismatches"), bParity && Mismatches.Num() == 0))
                {
                    bPassed = false;
                    for (int32 Index = 0; Index < FMath::Min(Mismatches.Num(), 8); ++Index) AddError(Label + TEXT(": ") + Mismatches[Index]);
                }
                bPassed &= TestTrue(Label + TEXT(": preview carries no closure"), Preview.Closure.ClosureHash.IsEmpty() && Preview.Closure.Resources.Num() == 0);
                ++Pairs;
            }
        }
    }
    bPassed &= TestTrue(TEXT("parity covered the golden seed matrix"), Pairs >= 7 * 7);
    return bPassed;
}

// ---------------------------------------------------------------------------
// Acceptance 7: applied-graph reference vs compiled-recipe preview on real
// generated assets with mesh receipts (the diagnostic command path).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRecipeShadowParityAppliedTest,
    "Mimir.V5.Composite.Recipe.RecipeShadowParityApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRecipeShadowParityAppliedTest::RunTest(const FString& Parameters)
{
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr) return false;
    FRecipeFixture Fixture(*this);
    const FString MeshA = Fixture.Name(TEXT("recipe_mesh_a"));
    const FString MeshB = Fixture.Name(TEXT("recipe_mesh_b"));
    const FString MeshC = Fixture.Name(TEXT("recipe_mesh_c"));
    Fixture.Mesh(MeshA);
    Fixture.Mesh(MeshB);
    Fixture.Mesh(MeshC);

    FMHCompositeDocument ChildDocument;
    {
        FMHCompositeNode& Leaf = ChildDocument.Nodes.AddDefaulted_GetRef();
        Leaf.Kind = EMHCompositeNodeKind::Mesh;
        Leaf.Resource = MeshC;
        Leaf.Name = TEXT("child leaf");
        Leaf.Transform.TranslationCm = FVector(0.0, 50.0, 0.0);
        Leaf.Transform.RotationQuat = FQuat(FRotator(0.0, -30.0, 0.0));
        Leaf.Transform.Scale = FVector(1.0, 2.0, 0.5);
    }
    const FString ChildName = Fixture.Name(TEXT("recipe_child_cmp"));
    if (Fixture.Composite(ChildName, ChildDocument, {}) == nullptr) return false;

    FMHPlacementProfile Profile;
    Profile.LogicalName = TEXT("recipe_profile");
    Profile.bHasOffsetCm = true;
    Profile.OffsetCm = {{10.0f, 5.0f}, {0.0f, 2.0f}, {-3.0f, 1.0f}};
    Profile.bHasRotationDeg = true;
    Profile.RotationDeg = {{0.0f, 15.0f}, {-45.0f, 10.0f}, {0.0f, 0.0f}};
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
        Anchor.Name = TEXT("anchor");
        FMHCompositeNode& Random = RootDocument.Nodes.AddDefaulted_GetRef();
        Random.Kind = EMHCompositeNodeKind::Random;
        Random.Name = TEXT("random");
        Random.Profile = Profile.LogicalName;
        Random.bAppearanceSeedBoundary = true;
        Random.Options.Add({EMHCompositeOptionKind::Mesh, MeshB, 1.0f});
        Random.Options.Add({EMHCompositeOptionKind::Composite, ChildName, 2.0f});
        Random.Options.Add({EMHCompositeOptionKind::Empty, FString(), 0.5f});
    }
    const FString RootName = Fixture.Name(TEXT("recipe_root_cmp"));
    UMHCompositeAsset* RootAsset = Fixture.Composite(RootName, RootDocument, {Profile});
    if (RootAsset == nullptr) return false;

    bool bPassed = true;
    for (const int32 Seed : {0, 1, 42, 123, 2147483647})
    {
        for (const int32 AppearanceSeed : {0, 42, 1024})
        {
            const FString Label = FString::Printf(TEXT("seed %d appearance %d"), Seed, AppearanceSeed);
            TArray<FString> Mismatches;
            FString Error;
            const bool bParity = MHRunRecipeShadowParity(*RootAsset, *Settings, Seed, AppearanceSeed, Mismatches, Error);
            if (!TestTrue(Label + TEXT(": applied-graph shadow parity: ") + Error, bParity && Mismatches.Num() == 0))
            {
                bPassed = false;
                for (int32 Index = 0; Index < FMath::Min(Mismatches.Num(), 8); ++Index) AddError(Label + TEXT(": ") + Mismatches[Index]);
            }
        }
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
