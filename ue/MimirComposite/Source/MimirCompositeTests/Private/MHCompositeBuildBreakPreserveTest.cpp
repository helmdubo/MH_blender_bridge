#include "MHRecipeTestFixture.h"

#include "Components/StaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Random/MHRandomStream.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Root recipe with two mesh leaves at distinct transforms, placed and previewed. */
struct FPreserveFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    AMHCompositeActor* Actor = nullptr;
    UMHCompositeAsset* Root = nullptr;
    FString MeshA, MeshB;
    const FTransform ActorTransform = FTransform(FRotator(0.0, 15.0, 0.0), FVector(300.0, 100.0, 0.0));

    explicit FPreserveFixture(FAutomationTestBase& Test) : Recipe(Test) {}
    ~FPreserveFixture()
    {
        if (World != nullptr)
        {
            if (GEditor != nullptr && GEditor->Trans != nullptr) GEditor->Trans->Reset(INVTEXT("MH preserve test teardown"));
            World->DestroyWorld(false);
        }
    }

    bool Build(FAutomationTestBase& Test)
    {
        MeshA = Recipe.Name(TEXT("preserve_mesh_a"));
        MeshB = Recipe.Name(TEXT("preserve_mesh_b"));
        Recipe.Mesh(MeshA);
        Recipe.Mesh(MeshB);
        FMHCompositeDocument Document;
        {
            FMHCompositeNode& A = Document.Nodes.AddDefaulted_GetRef();
            A.Kind = EMHCompositeNodeKind::Mesh;
            A.Resource = MeshA;
            A.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
            FMHCompositeNode& B = Document.Nodes.AddDefaulted_GetRef();
            B.Kind = EMHCompositeNodeKind::Mesh;
            B.Resource = MeshB;
            B.Transform.TranslationCm = FVector(0.0, 200.0, 50.0);
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("preserve_root")), Document, {});
        if (Root == nullptr) return false;
        World = UWorld::CreateWorld(EWorldType::Editor, false);
        if (!Test.TestNotNull(TEXT("preserve editor world"), World)) return false;
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transactional;
        Actor = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), ActorTransform, SpawnParameters);
        if (!Test.TestNotNull(TEXT("preserve composite actor"), Actor)) return false;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(7);
        Actor->SetAppearanceSeed(91);
        Actor->SetCompositeAsset(Root);
        return Test.TestTrue(TEXT("preview builds: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty()) &&
            Test.TestNotNull(TEXT("resident plan"), Actor->GetResolvedPlan());
    }
};

} // namespace

// Audit 2026-09-04 §7.C: a mesh leaf broken out into an AStaticMeshActor must
// carry the four resolved appearance channels as Custom Primitive Data, exactly
// as the ISM instance did; otherwise materials reading those channels change
// colour/variation on Break.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHBreakMeshLeafKeepsAppearanceTest,
    "Mimir.V5.Composite.Break.MeshLeafKeepsAppearance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHBreakMeshLeafKeepsAppearanceTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FPreserveFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    const int32 BaseIndex = GetDefault<UMHCompositeSettings>()->AppearanceCustomDataBaseIndex;

    // Expected: one leaf per mesh, with its resolved channels and world matrix.
    struct FExpectedLeaf { FString Resource; FMatrix World; float Channels[MH_APPEARANCE_CHANNELS]; };
    TArray<FExpectedLeaf> Expected;
    bool bAnyNonZero = false;
    for (const FMHResolvedCompositeLeaf& Leaf : Fixture.Actor->GetResolvedPlan()->Leaves)
    {
        FExpectedLeaf& Row = Expected.AddDefaulted_GetRef();
        Row.Resource = Leaf.Resource;
        Row.World = Leaf.WorldMatrix * Fixture.ActorTransform.ToMatrixWithScale();
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        {
            Row.Channels[Channel] = Leaf.AppearanceChannels[Channel];
            bAnyNonZero |= !FMath::IsNearlyZero(Leaf.AppearanceChannels[Channel]);
        }
    }
    bool bPassed = TestEqual(TEXT("two resolved leaves"), Expected.Num(), 2);
    bPassed &= TestTrue(TEXT("fixture appearance is not all zero (otherwise the test proves nothing)"), bAnyNonZero);

    TArray<AActor*> Broken;
    TArray<FString> Warnings;
    FString Error;
    if (!TestTrue(TEXT("Break succeeds: ") + Error, Subsystem->BreakComposites({Fixture.Actor}, Broken, Warnings, Error))) return false;
    bPassed &= TestEqual(TEXT("Break emits two mesh actors"), Broken.Num(), 2);
    for (AActor* Spawned : Broken)
    {
        AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Spawned);
        if (!TestNotNull(TEXT("broken leaf is a StaticMeshActor"), MeshActor)) { bPassed = false; continue; }
        const FMatrix World = MeshActor->GetActorTransform().ToMatrixWithScale();
        const FExpectedLeaf* Leaf = Expected.FindByPredicate([&World](const FExpectedLeaf& Row)
            { return MHMatrixElementsWithinTrsTolerance(World, Row.World); });
        if (!TestNotNull(TEXT("broken leaf matches a resolved leaf by world transform"), Leaf)) { bPassed = false; continue; }
        const UStaticMeshComponent* Component = MeshActor->GetStaticMeshComponent();
        const TArray<float>& Data = Component->GetCustomPrimitiveData().Data;
        bPassed &= TestTrue(TEXT("broken leaf carries custom primitive data up to the appearance channels"),
            Data.Num() >= BaseIndex + MH_APPEARANCE_CHANNELS);
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS && Data.IsValidIndex(BaseIndex + Channel); ++Channel)
        {
            bPassed &= TestTrue(FString::Printf(TEXT("%s channel %d survives Break"), *Leaf->Resource, Channel),
                FMath::IsNearlyEqual(Data[BaseIndex + Channel], Leaf->Channels[Channel], 1.0e-6f));
        }
    }
    return bPassed;
}

// Audit 2026-09-04 §7.B, owner decision 2026-09-04: Build warns, never
// refuses, about selected state the recipe grammar cannot carry — a child
// composite's seeds (its random subtree re-rolls under the new parent) and a
// StaticMeshActor's material overrides / custom primitive data. The preflight is
// pure: it assembles the document and the warnings without touching source or
// the scene.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHBuildPreflightWarnsAboutLostStateTest,
    "Mimir.V5.Composite.Build.PreflightWarnsAboutLostState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHBuildPreflightWarnsAboutLostStateTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FPreserveFixture Fixture(*this);
    if (!Fixture.Build(*this)) return false;
    if (Fixture.Root->SourceRelativePath.IsEmpty()) Fixture.Root->SourceRelativePath = Fixture.Root->LogicalName + TEXT(".composite");
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transactional;

    // A plain managed mesh actor: representable, no warning.
    AStaticMeshActor* Plain = Fixture.World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FVector(0.0, 0.0, 0.0)), SpawnParameters);
    Plain->GetStaticMeshComponent()->SetStaticMesh(Cast<UStaticMesh>(Fixture.Recipe.Mesh(Fixture.Recipe.Name(TEXT("preserve_plain")))));
    // A managed mesh actor with a material override and custom primitive data: state the grammar cannot carry.
    AStaticMeshActor* Overridden = Fixture.World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FVector(50.0, 0.0, 0.0)), SpawnParameters);
    Overridden->GetStaticMeshComponent()->SetStaticMesh(Cast<UStaticMesh>(Fixture.Recipe.Mesh(Fixture.Recipe.Name(TEXT("preserve_overridden")))));
    UMaterial* Override = NewObject<UMaterial>(GetTransientPackage(), FName(*Fixture.Recipe.Name(TEXT("preserve_override_mat"))));
    Overridden->GetStaticMeshComponent()->SetMaterial(0, Override);
    Overridden->GetStaticMeshComponent()->SetCustomPrimitiveDataFloat(0, 0.5f);

    const TArray<AActor*> Selection = {Plain, Overridden, Fixture.Actor};
    FMHCompositeDocument Document;
    TArray<FString> Warnings;
    FString Error;
    bool bPassed = TestTrue(TEXT("preflight admits the selection: ") + Error,
        MHPreflightBuildComposite(Selection, *GetDefault<UMHCompositeSettings>(), Document, Warnings, Error));
    bPassed &= TestEqual(TEXT("preflight assembles one node per actor"), Document.Nodes.Num(), 3);
    const auto WarningFor = [&Warnings](const AActor* Target, const TCHAR* Needle)
    {
        return Warnings.ContainsByPredicate([&](const FString& Warning)
            { return Warning.Contains(Target->GetPathName()) && Warning.Contains(Needle); });
    };
    bPassed &= TestTrue(TEXT("child composite warns that its seeds are not representable and its random subtree re-rolls"),
        WarningFor(Fixture.Actor, TEXT("seed")));
    bPassed &= TestTrue(TEXT("overridden mesh warns about its material override"), WarningFor(Overridden, TEXT("material override")));
    bPassed &= TestTrue(TEXT("overridden mesh warns about its custom primitive data"), WarningFor(Overridden, TEXT("custom primitive data")));
    bPassed &= TestFalse(TEXT("plain mesh raises no warning"),
        Warnings.ContainsByPredicate([&](const FString& Warning) { return Warning.Contains(Plain->GetPathName()); }));
    bPassed &= TestTrue(TEXT("preflight touches no actor"), IsValid(Plain) && IsValid(Overridden) && IsValid(Fixture.Actor));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
