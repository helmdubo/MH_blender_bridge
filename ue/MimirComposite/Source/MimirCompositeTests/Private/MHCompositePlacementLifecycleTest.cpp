#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{
namespace
{
/**
 * Minimal applied fixture for the S6.2 lifecycle slice: one mesh receipt plus a
 * composite whose top-level nodes each carry exactly one mesh leaf.
 */
struct FMHLifecycleFixture
{
    FAutomationTestBase& Test;
    TArray<UObject*> Assets;
    FString Name = TEXT("lifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UStaticMesh* Mesh = nullptr;
    UMHCompositeAsset* Asset = nullptr;

    explicit FMHLifecycleFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FMHLifecycleFixture()
    {
        for (UObject* Object : Assets)
        {
            if (!IsValid(Object)) continue;
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->MarkAsGarbage();
        }
    }

    bool Build(const int32 TopLevelNodes)
    {
        const FString MeshName = Name + TEXT("_mesh");
        Mesh = NewObject<UStaticMesh>(CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + MeshName)),
            FName(*MeshName), RF_Public | RF_Standalone);
        Assets.Add(Mesh);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = MeshName;
        Receipt->SourceRelativePath = MeshName + TEXT(".mesh.fbx");
        Receipt->SourceHash = TEXT("blake3-160:0123456789012345678901234567890123456789");
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);

        Asset = NewObject<UMHCompositeAsset>(CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + Name)),
            FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        FMHCompositeDocument Document;
        for (int32 Index = 0; Index < TopLevelNodes; ++Index)
        {
            FMHCompositeNode& Group = Document.Nodes.AddDefaulted_GetRef();
            Group.Kind = EMHCompositeNodeKind::Group;
            Group.Name = FString::Printf(TEXT("group_%d"), Index);
            Group.Transform.TranslationCm = FVector(200.0 * Index, 0.0, 0.0);
            FMHCompositeNode& Leaf = Group.Children.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshName;
        }
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(*Asset, Document, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(Error);
            return false;
        }
        Asset->LogicalName = Name;
        Asset->SourceRelativePath = Name + TEXT(".composite");
        Asset->SourceHash = MHRawPayloadHash(Bytes);
        Asset->AppliedHash = Asset->SourceHash;
        return true;
    }
};

/** Only the explicit isolated host may write maps and generated packages. */
bool LifecycleIsolatedHost(FAutomationTestBase& Test)
{
    if (GEditor != nullptr && FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) == TEXT("MimirCompositeV5S6")) return true;
    Test.AddInfo(TEXT("map lane NOT RUN: requires the isolated MimirCompositeV5S6 editor host"));
    return false;
}

int32 LifecycleLeafCount(const AMHCompositeActor& Actor)
{
    int32 Count = 0;
    for (UActorComponent* Component : Actor.GetDerivedComponents())
        if (Cast<UStaticMeshComponent>(Component) != nullptr) ++Count;
    return Count;
}
} // namespace

/**
 * Acceptance 2: no placement component is created or registered while the actor
 * is still unregistered, which is exactly the state PostLoad runs in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPlacementLifecycleRegistrationTest,
    "Mimir.V5.Composite.Lifecycle.NoBuildBeforeRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPlacementLifecycleRegistrationTest::RunTest(const FString& Parameters)
{
    if (!LifecycleIsolatedHost(*this)) return true;
    FMHLifecycleFixture Fixture(*this);
    if (!Fixture.Build(3)) return false;
    UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (!TestNotNull(TEXT("blank editor map"), World)) return false;
    AMHCompositeActor* Authored = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("authored placement"), Authored)) return false;
    Authored->SetAutoSeed(false);
    Authored->SetCompositeAsset(Fixture.Asset);
    if (!TestNotNull(*Authored->GetLastPlacementError(), Authored->GetResolvedPlan())) return false;

    TArray<UPackage*> Packages;
    for (UObject* Object : Fixture.Assets) Packages.AddUnique(Object->GetOutermost());
    const FString MapPackage = TEXT("/Game/MimirS6/PlacementLifecycle");
    if (!TestTrue(TEXT("fixture asset packages saved"), UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) ||
        !TestTrue(TEXT("authored placement map saved"), UEditorLoadingAndSavingUtils::SaveMap(World, MapPackage)))
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }
    UEditorLoadingAndSavingUtils::NewBlankMap(false);
    FString MapFile;
    if (!TestTrue(TEXT("map filename resolves"),
        FPackageName::TryConvertLongPackageNameToFilename(MapPackage, MapFile, FPackageName::GetMapPackageExtension())))
    {
        return false;
    }
    UWorld* Loaded = UEditorLoadingAndSavingUtils::LoadMap(MapFile);
    if (!TestNotNull(TEXT("reopened level"), Loaded))
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }
    AMHCompositeActor* Reloaded = nullptr;
    for (TActorIterator<AMHCompositeActor> It(Loaded); It; ++It) { Reloaded = *It; break; }
    bool bPassed = TestNotNull(TEXT("reopened level still holds the placement"), Reloaded);
    if (Reloaded != nullptr)
    {
        AddInfo(FString::Printf(TEXT("reloaded placement rebuilds=%u unregistered-builds=%u leaves=%d registered=%d"),
            Reloaded->GetPlacementRebuildCount(), Reloaded->GetPlacementUnregisteredBuildCount(),
            LifecycleLeafCount(*Reloaded), Reloaded->HasActorRegisteredAllComponents() ? 1 : 0));
        bPassed &= TestEqual(TEXT("no placement build ran before the actor was registered"),
            Reloaded->GetPlacementUnregisteredBuildCount(), 0u);
        bPassed &= TestNotNull(*Reloaded->GetLastPlacementError(), Reloaded->GetResolvedPlan());
        bPassed &= TestEqual(TEXT("reopened level materializes every leaf"), LifecycleLeafCount(*Reloaded), 3);
        for (UActorComponent* Component : Reloaded->GetDerivedComponents())
        {
            if (!IsValid(Component)) continue;
            bPassed &= TestTrue(TEXT("every derived placement component is registered"), Component->IsRegistered());
        }
    }
    UEditorLoadingAndSavingUtils::NewBlankMap(false);
    return bPassed;
}

/** Acceptance 3: moving the actor updates the basis and never rebuilds. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPlacementLifecycleMoveTest,
    "Mimir.V5.Composite.Lifecycle.MoveDoesNotRebuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPlacementLifecycleMoveTest::RunTest(const FString& Parameters)
{
    FMHLifecycleFixture Fixture(*this);
    if (!Fixture.Build(4)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetAutoSeed(false);
    Actor->SetCompositeAsset(Fixture.Asset);
    bool bPassed = TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan());
    const uint32 Before = Actor->GetPlacementRebuildCount();
    for (int32 Step = 1; Step <= 8; ++Step)
    {
        Actor->SetActorLocation(FVector(100.0 * Step, 25.0 * Step, 0.0));
        Actor->OnConstruction(Actor->GetActorTransform());
    }
    const uint32 After = Actor->GetPlacementRebuildCount();
    AddInfo(FString::Printf(TEXT("rebuilds before=%u after eight moves=%u"), Before, After));
    bPassed &= TestEqual(TEXT("moving the actor performs no full rebuild"), After, Before);
    bPassed &= TestNotNull(TEXT("plan survives the moves"), Actor->GetResolvedPlan());
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

/**
 * Acceptance 4: a desynchronized component view is refused with
 * MH_E_PLACEMENT_STATE_DESYNC and recovered with a full rebuild, never with a
 * partial intersection-length update.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPlacementLifecycleDesyncTest,
    "Mimir.V5.Composite.Lifecycle.BasisDesyncFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPlacementLifecycleDesyncTest::RunTest(const FString& Parameters)
{
    FMHLifecycleFixture Fixture(*this);
    if (!Fixture.Build(4)) return false;
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetAutoSeed(false);
    Actor->SetCompositeAsset(Fixture.Asset);
    bool bPassed = TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan());
    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    if (!bPassed || Plan == nullptr)
    {
        Actor->Destroy();
        World->DestroyWorld(false);
        return false;
    }

    FMHRandomSourceGraph Graph;
    TSet<FMHResourceKey> Dependencies;
    FString Error;
    if (!MHBuildAppliedCompositeGraph(*Fixture.Asset, *Settings, Graph, Dependencies, Error))
    {
        AddError(Error);
        Actor->Destroy();
        World->DestroyWorld(false);
        return false;
    }
    const FMHRandomComposite* Root = Graph.Composites.Find(Graph.RootComposite);
    if (!TestNotNull(TEXT("applied root definition"), Root))
    {
        Actor->Destroy();
        World->DestroyWorld(false);
        return false;
    }

    // A foreign basis proves the difference between a refusal and a partial
    // update: on refusal not one handle may have been moved into it.
    AActor* ForeignBasis = World->SpawnActor<AActor>();
    ForeignBasis->SetActorLocation(FVector(7777.0, -3333.0, 111.0));
    TArray<FTransform> BeforeHandles;
    for (const USceneComponent* Handle : Actor->GetTopLevelPlacementComponents())
        BeforeHandles.Add(Handle != nullptr ? Handle->GetComponentTransform() : FTransform::Identity);

    const TArray<TObjectPtr<USceneComponent>> NoLeaves;
    FString DesyncError;
    const bool bUpdated = MHUpdateCompositePlacementBasis(*ForeignBasis, *Plan, *Root,
        Actor->GetTopLevelPlacementComponents(), NoLeaves, DesyncError);
    AddInfo(TEXT("desynchronized basis update returned: ") +
        FString(bUpdated ? TEXT("true") : TEXT("false")) + TEXT(" / ") +
        (DesyncError.IsEmpty() ? TEXT("<no diagnostic>") : DesyncError));
    bPassed &= TestFalse(TEXT("a desynchronized component view is refused"), bUpdated);
    bPassed &= TestTrue(TEXT("refusal carries MH_E_PLACEMENT_STATE_DESYNC"),
        DesyncError.StartsWith(TEXT("MH_E_PLACEMENT_STATE_DESYNC")));
    for (int32 Index = 0; Index < Actor->GetTopLevelPlacementComponents().Num(); ++Index)
    {
        const USceneComponent* Handle = Actor->GetTopLevelPlacementComponents()[Index];
        bPassed &= TestTrue(TEXT("a refused update leaves every handle untouched"),
            Handle != nullptr && BeforeHandles.IsValidIndex(Index) &&
            Handle->GetComponentTransform().Equals(BeforeHandles[Index], 0.0));
    }

    // Same desync through the live actor: it must recover with a full rebuild.
    FArrayProperty* Leaves = CastField<FArrayProperty>(
        AMHCompositeActor::StaticClass()->FindPropertyByName(TEXT("LeafPlacementComponents")));
    if (TestNotNull(TEXT("reflected leaf component array"), Leaves))
    {
        FScriptArrayHelper Helper(Leaves, Leaves->ContainerPtrToValuePtr<void>(Actor));
        if (TestTrue(TEXT("leaf component array is populated"), Helper.Num() > 0))
        {
            Helper.RemoveValues(Helper.Num() - 1, 1);
            const uint32 RebuildsBefore = Actor->GetPlacementRebuildCount();
            Actor->SetActorLocation(FVector(500.0, 0.0, 0.0));
            AddInfo(FString::Printf(TEXT("after forced desync: rebuilds %u->%u desyncs=%u leaves=%d"),
                RebuildsBefore, Actor->GetPlacementRebuildCount(), Actor->GetPlacementDesyncCount(),
                LifecycleLeafCount(*Actor)));
            bPassed &= TestEqual(TEXT("the actor records exactly one fail-closed desync"),
                Actor->GetPlacementDesyncCount(), 1u);
            bPassed &= TestTrue(TEXT("the desync forces a full placement rebuild"),
                Actor->GetPlacementRebuildCount() > RebuildsBefore);
            bPassed &= TestEqual(TEXT("the rebuild restores every leaf"), LifecycleLeafCount(*Actor), 4);
            bPassed &= TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan());
        }
    }
    ForeignBasis->Destroy();
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

/** Acceptance 5: previous-component lookups are linear in the leaf count. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPlacementLifecycleLookupTest,
    "Mimir.V5.Composite.Lifecycle.PreviousLookupIsLinear",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPlacementLifecycleLookupTest::RunTest(const FString& Parameters)
{
    constexpr int32 TopLevelNodes = 200;
    FMHLifecycleFixture Fixture(*this);
    if (!Fixture.Build(TopLevelNodes)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetAutoSeed(false);
    Actor->SetCompositeAsset(Fixture.Asset);
    bool bPassed = TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan());
    bPassed &= TestEqual(TEXT("fixture materializes every leaf"), LifecycleLeafCount(*Actor), TopLevelNodes);

    // Second compile: the previous view now holds every handle and every leaf.
    const int32 Previous = Actor->GetDerivedComponents().Num();
    MHResetPlacementPreviousComponentProbes();
    Actor->RebuildComposite();
    const uint64 Probes = MHGetPlacementPreviousComponentProbes();
    const int32 Queries = TopLevelNodes * 2;
    const uint64 Linear = 16ull * static_cast<uint64>(Previous + Queries);
    AddInfo(FString::Printf(TEXT("previous=%d queries=%d probes=%llu linear bound=%llu"),
        Previous, Queries, Probes, Linear));
    bPassed &= TestTrue(TEXT("previous-component lookup work is linear in the leaf count"), Probes <= Linear);
    bPassed &= TestEqual(TEXT("the reused view still materializes every leaf"),
        LifecycleLeafCount(*Actor), TopLevelNodes);
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}
} // namespace UE::MimirComposite::Tests
