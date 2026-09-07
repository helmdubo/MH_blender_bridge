#include "MHRecipeTestFixture.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "CoreMinimal.h"
#include "FileHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/Linker.h"
#include "UObject/LinkerLoad.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FSelectedMeshPropertyView
{
    FArrayProperty* Array = nullptr;
    FObjectProperty* Inner = nullptr;

    bool TryBind()
    {
        Array = FindFProperty<FArrayProperty>(AMHCompositeActor::StaticClass(), TEXT("SelectedMeshDependencies"));
        if (Array == nullptr) return false;
        Inner = CastField<FObjectProperty>(Array->Inner);
        return Inner != nullptr;
    }

    bool Bind(FAutomationTestBase& Test)
    {
        if (!TryBind())
        {
            Test.AddError(TEXT("persisted SelectedMeshDependencies property is not present yet"));
            return false;
        }
        bool bPassed = true;
        bPassed &= Test.TestFalse(TEXT("selected dependency array is not transient"),
            EnumHasAnyFlags(Array->PropertyFlags, CPF_Transient));
        bPassed &= Test.TestFalse(TEXT("selected dependency array is not duplicate transient"),
            EnumHasAnyFlags(Array->PropertyFlags, CPF_DuplicateTransient));
        bPassed &= Test.TestEqual(TEXT("selected dependency element class"),
            Inner->PropertyClass.Get(), UStaticMesh::StaticClass());
        return bPassed;
    }

    TArray<UStaticMesh*> Values(const AMHCompositeActor& Actor) const
    {
        TArray<UStaticMesh*> Result;
        if (Array == nullptr || Inner == nullptr) return Result;
        const void* ValuePtr = Array->ContainerPtrToValuePtr<void>(&Actor);
        FScriptArrayHelper Helper(Array, const_cast<void*>(ValuePtr));
        for (int32 Index = 0; Index < Helper.Num(); ++Index)
        {
            Result.Add(Cast<UStaticMesh>(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index))));
        }
        return Result;
    }
};

UMHCompositeAsset* BuildRandomRoot(
    FRecipeFixture& Recipe,
    const FString& RootName,
    const FString& SelectedMesh,
    const FString& UnselectedMesh)
{
    FMHCompositeDocument Document;
    FMHCompositeNode& Random = Document.Nodes.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    FMHCompositeOption& Selected = Random.Options.AddDefaulted_GetRef();
    Selected.Kind = EMHCompositeOptionKind::Mesh;
    Selected.Resource = SelectedMesh;
    Selected.Weight = 1.0f;
    FMHCompositeOption& Unselected = Random.Options.AddDefaulted_GetRef();
    Unselected.Kind = EMHCompositeOptionKind::Mesh;
    Unselected.Resource = UnselectedMesh;
    Unselected.Weight = 0.0f;
    // A second selected leaf proves the persisted dependency collection is
    // unique by logical mesh rather than one entry per materialized leaf.
    FMHCompositeNode& Duplicate = Document.Nodes.AddDefaulted_GetRef();
    Duplicate.Kind = EMHCompositeNodeKind::Mesh;
    Duplicate.Resource = SelectedMesh;
    return Recipe.Composite(RootName, Document, {});
}

bool SpawnActor(
    FAutomationTestBase& Test,
    UWorld*& OutWorld,
    AMHCompositeActor*& OutActor,
    UMHCompositeAsset& Asset)
{
    OutWorld = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!Test.TestNotNull(TEXT("dependency world"), OutWorld)) return false;
    OutActor = OutWorld->SpawnActor<AMHCompositeActor>();
    if (!Test.TestNotNull(TEXT("dependency actor"), OutActor)) return false;
    OutActor->SetAutoSeed(false);
    OutActor->SetAutoAppearanceSeed(false);
    OutActor->SetSeed(5);
    OutActor->SetAppearanceSeed(9);
    OutActor->SetCompositeAsset(&Asset);
    return Test.TestNotNull(TEXT("dependency actor resolves"), OutActor->GetResolvedPlan());
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSavedSelectedMeshDependenciesTest,
    "Mimir.V5.Composite.Persistence.SelectedMeshDependenciesAreDeduplicatedAndSelectedOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSavedSelectedMeshDependenciesTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FSelectedMeshPropertyView Property;
    if (!Property.Bind(*this)) return false;
    FRecipeFixture Recipe(*this);
    const FString SelectedName = Recipe.Name(TEXT("saved_selected_mesh"));
    const FString UnselectedName = Recipe.Name(TEXT("saved_unselected_mesh"));
    UStaticMesh* Selected = Recipe.Mesh(SelectedName);
    UStaticMesh* Unselected = Recipe.Mesh(UnselectedName);
    UMHCompositeAsset* Root = BuildRandomRoot(Recipe, Recipe.Name(TEXT("saved_dependency_root")),
        SelectedName, UnselectedName);
    if (!TestNotNull(TEXT("dependency root"), Root)) return false;
    UWorld* World = nullptr;
    AMHCompositeActor* Actor = nullptr;
    if (!SpawnActor(*this, World, Actor, *Root)) return false;
    const TArray<UStaticMesh*> Saved = Property.Values(*Actor);
    bool bPassed = TestEqual(TEXT("one unique selected dependency"), Saved.Num(), 1);
    bPassed &= TestTrue(TEXT("selected mesh is persisted"), Saved.Contains(Selected));
    bPassed &= TestFalse(TEXT("unselected mesh is not persisted"), Saved.Contains(Unselected));
    Actor->Destroy();
    World->DestroyWorld(true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSavedSelectedMeshDependenciesReplaceTest,
    "Mimir.V5.Composite.Persistence.SelectedMeshDependenciesReplaceOnAssetChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSavedSelectedMeshDependenciesReplaceTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FSelectedMeshPropertyView Property;
    if (!Property.Bind(*this)) return false;
    FRecipeFixture Recipe(*this);
    const FString NameA = Recipe.Name(TEXT("saved_replace_mesh_a"));
    const FString NameB = Recipe.Name(TEXT("saved_replace_mesh_b"));
    UStaticMesh* MeshA = Recipe.Mesh(NameA);
    UStaticMesh* MeshB = Recipe.Mesh(NameB);
    UMHCompositeAsset* RootA = BuildRandomRoot(Recipe, Recipe.Name(TEXT("saved_replace_root_a")), NameA, NameB);
    UMHCompositeAsset* RootB = BuildRandomRoot(Recipe, Recipe.Name(TEXT("saved_replace_root_b")), NameB, NameA);
    if (!TestNotNull(TEXT("replacement root A"), RootA) || !TestNotNull(TEXT("replacement root B"), RootB)) return false;
    UWorld* World = nullptr;
    AMHCompositeActor* Actor = nullptr;
    if (!SpawnActor(*this, World, Actor, *RootA)) return false;
    bool bPassed = TestTrue(TEXT("first selected mesh is saved"), Property.Values(*Actor).Contains(MeshA));
    Actor->SetCompositeAsset(RootB);
    const TArray<UStaticMesh*> Replaced = Property.Values(*Actor);
    bPassed &= TestEqual(TEXT("replacement remains deduplicated"), Replaced.Num(), 1);
    bPassed &= TestTrue(TEXT("replacement selected mesh is saved"), Replaced.Contains(MeshB));
    bPassed &= TestFalse(TEXT("old selected mesh is removed"), Replaced.Contains(MeshA));
    Actor->Destroy();
    World->DestroyWorld(true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSavedSelectedMeshDependenciesRoundTripTest,
    "Mimir.V5.Composite.Persistence.SelectedMeshDependenciesSurviveMapRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSavedSelectedMeshDependenciesRoundTripTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FSelectedMeshPropertyView Property;
    const bool bPropertyAvailable = Property.TryBind();
    if (!bPropertyAvailable)
    {
        AddError(TEXT("persisted SelectedMeshDependencies property is not present yet"));
    }
    FRecipeFixture Recipe(*this);
    const FString SelectedName = Recipe.Name(TEXT("saved_roundtrip_selected"));
    const FString UnselectedName = Recipe.Name(TEXT("saved_roundtrip_unselected"));
    UStaticMesh* Selected = Recipe.Mesh(SelectedName);
    UStaticMesh* Unselected = Recipe.Mesh(UnselectedName);
    UMHCompositeAsset* Root = BuildRandomRoot(Recipe, Recipe.Name(TEXT("saved_roundtrip_root")), SelectedName, UnselectedName);
    if (!TestNotNull(TEXT("roundtrip root"), Root)) return false;

    TArray<TStrongObjectPtr<UObject>> AssetPins;
    TArray<FString> FixtureFiles;
    AssetPins.Reserve(Recipe.Assets.Num());
    FixtureFiles.Reserve(Recipe.Assets.Num());
    for (UObject* Asset : Recipe.Assets)
    {
        AssetPins.Emplace(Asset);
        FString Filename;
        if (Asset != nullptr && FPackageName::TryConvertLongPackageNameToFilename(
                Asset->GetOutermost()->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
        {
            FixtureFiles.AddUnique(Filename);
        }
    }

    const FString MapPackage = TEXT("/Game/MH/Generated/") + Recipe.Name(TEXT("saved_dependency_roundtrip_map"));
    ON_SCOPE_EXIT
    {
        FString Filename;
        if (FPackageName::TryConvertLongPackageNameToFilename(MapPackage, Filename, FPackageName::GetMapPackageExtension()))
            IFileManager::Get().Delete(*Filename, false, true, true);
        for (const FString& FixtureFile : FixtureFiles)
            IFileManager::Get().Delete(*FixtureFile, false, true, true);
    };

    UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (!TestNotNull(TEXT("roundtrip blank map"), World)) return false;
    AMHCompositeActor* Authored = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("roundtrip authored actor"), Authored)) return false;
    Authored->SetAutoSeed(false);
    Authored->SetAutoAppearanceSeed(false);
    Authored->SetSeed(5);
    Authored->SetAppearanceSeed(9);
    Authored->SetCompositeAsset(Root);
    if (!TestNotNull(TEXT("roundtrip authored plan"), Authored->GetResolvedPlan())) return false;
    TWeakObjectPtr<AMHCompositeActor> AuthoredWeak = Authored;
    if (bPropertyAvailable)
    {
        const TArray<UStaticMesh*> Before = Property.Values(*Authored);
        if (!TestTrue(TEXT("roundtrip authored map has selected hard reference"), Before.Contains(Selected))) return false;
    }

    TArray<UPackage*> Packages;
    for (UObject* Asset : Recipe.Assets) Packages.AddUnique(Asset->GetOutermost());
    bool bPassed = TestTrue(TEXT("roundtrip fixture assets save"), UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) &&
        TestTrue(TEXT("roundtrip map saves"), UEditorLoadingAndSavingUtils::SaveMap(World, MapPackage));
    if (!bPassed)
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }
    UEditorLoadingAndSavingUtils::NewBlankMap(false);
    CollectGarbage(RF_NoFlags);
    bPassed &= TestFalse(TEXT("authored actor is unloaded before direct package load"), AuthoredWeak.IsValid());
    UPackage* LoadedPackage = LoadPackage(nullptr, *MapPackage, LOAD_None);
    bPassed &= TestNotNull(TEXT("roundtrip map package loads directly"), LoadedPackage);
    if (LoadedPackage != nullptr && LoadedPackage->GetLinker() != nullptr)
    {
        const FName SelectedPackageName = FName(*Selected->GetOutermost()->GetName());
        const FName UnselectedPackageName = FName(*Unselected->GetOutermost()->GetName());
        bool bSelectedImport = false;
        bool bUnselectedImport = false;
        for (const FObjectImport& Import : LoadedPackage->GetLinker()->ImportMap)
        {
            if (Import.ClassName == NAME_Package)
            {
                bSelectedImport |= Import.ObjectName == SelectedPackageName;
                bUnselectedImport |= Import.ObjectName == UnselectedPackageName;
            }
#if WITH_EDITORONLY_DATA
            bSelectedImport |= Import.PackageName == SelectedPackageName;
            bUnselectedImport |= Import.PackageName == UnselectedPackageName;
#endif
        }
        bPassed &= TestTrue(TEXT("map linker imports selected mesh package"), bSelectedImport);
        bPassed &= TestFalse(TEXT("map linker omits unselected mesh package"), bUnselectedImport);
    }
    else
    {
        bPassed &= TestNotNull(TEXT("roundtrip map linker is available"), LoadedPackage != nullptr ? LoadedPackage->GetLinker() : nullptr);
    }
    UWorld* Loaded = LoadedPackage != nullptr ? UWorld::FindWorldInPackage(LoadedPackage) : nullptr;
    bPassed &= TestNotNull(TEXT("roundtrip world is present"), Loaded);
    AMHCompositeActor* Reloaded = nullptr;
    if (Loaded != nullptr && Loaded->PersistentLevel != nullptr)
    {
        for (AActor* Actor : Loaded->PersistentLevel->Actors)
        {
            if (AMHCompositeActor* Composite = Cast<AMHCompositeActor>(Actor))
            {
                Reloaded = Composite;
                break;
            }
        }
    }
    bPassed &= TestNotNull(TEXT("roundtrip actor reopens"), Reloaded);
    if (Reloaded != nullptr)
    {
        bPassed &= TestEqual(TEXT("reopen performs no preview build before registration"),
            Reloaded->GetPlacementUnregisteredBuildCount(), 0u);
    }
    if (Reloaded != nullptr && bPropertyAvailable)
    {
        const TArray<UStaticMesh*> After = Property.Values(*Reloaded);
        bPassed &= TestEqual(TEXT("roundtrip preserves one selected dependency"), After.Num(), 1);
        bPassed &= TestTrue(TEXT("roundtrip restores selected hard reference"), After.Contains(Selected));
        bPassed &= TestFalse(TEXT("roundtrip does not restore unselected dependency"), After.Contains(Unselected));
    }
    // Direct LoadPackage created a second world outside the editor's current
    // map context. Retire it before the fixture retires its mesh assets.
    if (Reloaded != nullptr) Reloaded->Destroy();
    if (Loaded != nullptr)
    {
        Loaded->DestroyWorld(true);
        Loaded->ClearFlags(RF_Public | RF_Standalone);
        Loaded->MarkAsGarbage();
    }
    if (LoadedPackage != nullptr)
    {
        LoadedPackage->ClearFlags(RF_Public | RF_Standalone);
        LoadedPackage->MarkAsGarbage();
    }
    CollectGarbage(RF_NoFlags);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
