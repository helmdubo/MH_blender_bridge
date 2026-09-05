#include "MHRecipeTestFixture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMesh/MHStaticMeshImporter.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** A managed mesh that exists only on disk: saved to its canonical package, then released and collected. */
struct FColdMesh
{
    FString LogicalName;
    FString PackageName;
    FString Filename;

    bool Create(FAutomationTestBase& Test, const FString& InLogicalName)
    {
        LogicalName = InLogicalName;
        PackageName = TEXT("/Game/MH/Generated/Meshes/") + LogicalName;
        Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
        UPackage* Package = CreatePackage(*PackageName);
        // Real geometry (a stock cube copy), so the saved asset passes the
        // engine's asset check on reload; only the fixture may wait for it.
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!Test.TestNotNull(TEXT("stock cube"), Cube)) return false;
        FStaticMeshCompilingManager::Get().FinishCompilation({Cube});
        UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, Package, FName(*LogicalName));
        Mesh->SetFlags(RF_Public | RF_Standalone);
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = LogicalName;
        Receipt->SourceRelativePath = LogicalName + TEXT(".mesh.fbx");
        Receipt->SourceHash = MHRawPayloadHash({0x63, 0x6f, 0x6c, 0x64});
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);
        Package->MarkPackageDirty();
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        if (!Test.TestTrue(TEXT("cold mesh package saves"), UPackage::SavePackage(Package, Mesh, *Filename, Args))) return false;
        // Release: no strong references remain, so a forced GC unloads both the
        // mesh and its package; the next resolve finds nothing in memory.
        Mesh->ClearFlags(RF_Public | RF_Standalone);
        Mesh->MarkAsGarbage();
        Package->ClearFlags(RF_Public | RF_Standalone);
        Package->MarkAsGarbage();
        CollectGarbage(RF_NoFlags);
        return Test.TestNull(TEXT("cold mesh is not resident after GC"), FindObject<UStaticMesh>(nullptr, *(PackageName + TEXT(".") + LogicalName)));
    }

    ~FColdMesh()
    {
        if (!Filename.IsEmpty()) IFileManager::Get().Delete(*Filename, false, true, true);
    }

    static FMHResourceKey Key(const FString& Name)
    {
        FMHResourceKey Result;
        Result.Kind = EMHResourceKind::StaticMesh;
        Result.LogicalName = Name;
        return Result;
    }
};

/** The mesh component rendering the single leaf (ISM bucket or plain static mesh component). */
UStaticMeshComponent* FirstBucket(const AMHCompositeActor& Actor)
{
    for (UActorComponent* Component : Actor.GetDerivedComponents())
    {
        if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component)) return Mesh;
    }
    return nullptr;
}

} // namespace

// KICKOFF §5 R4 / 16 §2.2: a selected endpoint that is not resident loads
// asynchronously; the first frame renders the placeholder mesh, the
// interactive path performs no synchronous package load and never waits for
// static mesh compilation; once the load completes the placement is reconciled
// to the real mesh. Unselected variants are never loaded at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAsyncColdEndpointTest,
    "Mimir.V5.Composite.Async.ColdEndpointLoadsWithoutSyncLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAsyncColdEndpointTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (!TestNotNull(TEXT("endpoint registry"), Registry)) return false;
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    // Engine content is not rooted: compare the placeholder by path, the object
    // may be collected and reloaded by the fixture's forced GC.
    const FString PlaceholderPath = Settings->PlaceholderMesh.ToSoftObjectPath().ToString();
    if (!TestTrue(TEXT("placeholder mesh setting is set (default /Engine/BasicShapes/Cube)"), !PlaceholderPath.IsEmpty())) return false;

    FRecipeFixture Recipe(*this);
    FColdMesh Selected, Unselected;
    if (!Selected.Create(*this, Recipe.Name(TEXT("async_cold_selected"))) ||
        !Unselected.Create(*this, Recipe.Name(TEXT("async_cold_unselected")))) return false;
    Registry->InvalidateAll();

    // Root = [random{selected w1, unselected w0}]: the seed can only pick the first.
    FMHCompositeDocument Document;
    {
        FMHCompositeNode& Random = Document.Nodes.AddDefaulted_GetRef();
        Random.Kind = EMHCompositeNodeKind::Random;
        FMHCompositeOption& A = Random.Options.AddDefaulted_GetRef();
        A.Kind = EMHCompositeOptionKind::Mesh;
        A.Resource = Selected.LogicalName;
        A.Weight = 1.0f;
        FMHCompositeOption& B = Random.Options.AddDefaulted_GetRef();
        B.Kind = EMHCompositeOptionKind::Mesh;
        B.Resource = Unselected.LogicalName;
        B.Weight = 0.0f;
    }
    UMHCompositeAsset* Root = Recipe.Composite(Recipe.Name(TEXT("async_cold_root")), Document, {});
    if (Root == nullptr) return false;

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("async world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(true); };
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("async actor"), Actor)) return false;
    Actor->SetAutoSeed(false);
    Actor->SetAutoAppearanceSeed(false);
    Actor->SetSeed(5);
    Actor->SetAppearanceSeed(9);
    MHResetEndpointResolveMetrics();
    MHResetPlacementStageMetrics();
    Actor->SetCompositeAsset(Root);

    // First frame: placeholder, no sync load, no compilation wait, no error.
    bool bPassed = TestTrue(TEXT("first frame builds without error: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty());
    bPassed &= TestNotNull(TEXT("first frame has a resident plan"), Actor->GetResolvedPlan());
    bPassed &= TestEqual(TEXT("cold endpoint performs no synchronous package load"), MHGetEndpointResolveMetrics().PackageLoadsSync, 0ull);
    bPassed &= TestEqual(TEXT("interactive path never waits for mesh compilation"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::WaitStaticMeshCompilation).Calls, 0ull);
    const FMHEndpointPrototype& Loading = Registry->Resolve(FColdMesh::Key(Selected.LogicalName));
    bPassed &= TestTrue(TEXT("selected cold endpoint is Loading"), Loading.State == EMHEndpointState::Loading);
    UStaticMeshComponent* Bucket = FirstBucket(*Actor);
    bPassed &= TestTrue(TEXT("first frame renders the placeholder mesh"),
        Bucket != nullptr && Bucket->GetStaticMesh() != nullptr && Bucket->GetStaticMesh()->GetPathName() == PlaceholderPath);
    bPassed &= TestFalse(TEXT("unselected variant is never resolved"), Registry->HasPrototype(FColdMesh::Key(Unselected.LogicalName)));

    // Completion: the registry admits the loaded object and the placement is
    // reconciled to the real mesh, still without a synchronous package load.
    bPassed &= TestTrue(TEXT("async loads flush"), Registry->FlushAsyncLoadsForTests());
    const FMHEndpointPrototype& Ready = Registry->Resolve(FColdMesh::Key(Selected.LogicalName));
    bPassed &= TestTrue(TEXT("selected endpoint is Ready after the flush"), Ready.State == EMHEndpointState::Ready);
    bPassed &= TestEqual(TEXT("still no synchronous package load"), MHGetEndpointResolveMetrics().PackageLoadsSync, 0ull);
    UStaticMesh* Real = Cast<UStaticMesh>(Ready.Object.Get());
    Bucket = FirstBucket(*Actor);
    bPassed &= TestTrue(TEXT("placement renders the real mesh after the load"), Real != nullptr && Bucket != nullptr && Bucket->GetStaticMesh() == Real);
    bPassed &= TestTrue(TEXT("no error after reconcile: ") + Actor->GetLastPlacementError(), Actor->GetLastPlacementError().IsEmpty());
    bPassed &= TestFalse(TEXT("unselected variant stays unloaded"), FindObject<UStaticMesh>(nullptr, *(Unselected.PackageName + TEXT(".") + Unselected.LogicalName)) != nullptr);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
