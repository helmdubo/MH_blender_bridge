#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Random/MHRandomStream.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{
namespace
{

FString BreakFreshnessSignature(const AMHCompositeActor& Actor)
{
    const FStrProperty* Property = FindFProperty<FStrProperty>(AMHCompositeActor::StaticClass(), TEXT("ResolvedSignature"));
    return Property != nullptr ? Property->GetPropertyValue_InContainer(&Actor) : FString();
}

TSet<AActor*> BreakFreshnessLiveActors(const UWorld& World)
{
    TSet<AActor*> Actors;
    for (AActor* Actor : World.PersistentLevel->Actors)
        if (IsValid(Actor) && !Actor->IsActorBeingDestroyed()) Actors.Add(Actor);
    return Actors;
}

struct FBreakFreshnessSnapshot
{
    TArray<TObjectPtr<UActorComponent>> Components;
    TArray<FTransform> ComponentTransforms;
    FString Signature;
    FString Error;
    FTransform Transform;
    int32 Seed = 0;

    explicit FBreakFreshnessSnapshot(const AMHCompositeActor& Actor)
        : Components(Actor.GetDerivedComponents()), Signature(BreakFreshnessSignature(Actor)),
          Error(Actor.GetLastPlacementError()), Transform(Actor.GetActorTransform()), Seed(Actor.GetSeed())
    {
        for (const UActorComponent* Component : Components)
        {
            const USceneComponent* Scene = Cast<USceneComponent>(Component);
            ComponentTransforms.Add(Scene != nullptr ? Scene->GetComponentTransform() : FTransform::Identity);
        }
    }

    bool ExpectUnchanged(FAutomationTestBase& Test, const AMHCompositeActor& Actor) const
    {
        bool bPassed = Test.TestFalse(TEXT("failed Break preserves the placement actor"), Actor.IsActorBeingDestroyed());
        bPassed &= Test.TestTrue(TEXT("failed Break preserves component identities"), Components == Actor.GetDerivedComponents());
        for (int32 Index = 0; Index < Components.Num(); ++Index)
        {
            const USceneComponent* Scene = Cast<USceneComponent>(Components[Index]);
            bPassed &= Test.TestTrue(TEXT("failed Break preserves component transforms"),
                IsValid(Components[Index]) && (Scene == nullptr || Scene->GetComponentTransform().Equals(ComponentTransforms[Index], 0.0)));
        }
        bPassed &= Test.TestEqual(TEXT("failed Break preserves the stored signature"), BreakFreshnessSignature(Actor), Signature);
        bPassed &= Test.TestEqual(TEXT("failed Break preserves the placement error"), Actor.GetLastPlacementError(), Error);
        bPassed &= Test.TestEqual(TEXT("failed Break preserves Seed"), Actor.GetSeed(), Seed);
        bPassed &= Test.TestTrue(TEXT("failed Break preserves the actor transform"), Actor.GetActorTransform().Equals(Transform, 0.0));
        return bPassed;
    }
};

struct FBreakFreshnessFixture
{
    FAutomationTestBase& Test;
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    FString SourceRoot;
    FDirectoryPath PreviousRoot;
    UWorld* World = nullptr;
    UMHSourceImporter* Importer = nullptr;
    TArray<UObject*> Assets;
    TArray<FString> PublishedPackageFiles;
    UStaticMesh* MeshA = nullptr;
    UStaticMesh* MeshB = nullptr;
    UMHCompositeAsset* Child = nullptr;
    UMHCompositeAsset* Parent = nullptr;

    explicit FBreakFreshnessFixture(FAutomationTestBase& InTest) : Test(InTest)
    {
        Importer = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHSourceImporter>() : nullptr;
        // Advancing the core ticker below must exercise only the deferred
        // preview refresh, not a host startup/source-watcher import that can
        // accidentally heal the stale placement through a global rebuild.
        // This existing test seam pauses lifecycle work, not explicit Build.
        if (Importer != nullptr) Importer->SetPIEActiveForTests(true);
        SourceRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests"), TEXT("break_freshness_") + Suffix);
        IFileManager::Get().MakeDirectory(*SourceRoot, true);
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        PreviousRoot = Settings->SourceRoot;
        Settings->SourceRoot.Path = SourceRoot;
        MHShutdownProjectIndex();
        World = UWorld::CreateWorld(EWorldType::Editor, false);
    }

    ~FBreakFreshnessFixture()
    {
        if (GEditor != nullptr) GEditor->SelectNone(false, true, false);
        if (World != nullptr) World->DestroyWorld(false);
        for (UObject* Asset : Assets)
        {
            if (!IsValid(Asset)) continue;
            FAssetRegistryModule::AssetDeleted(Asset);
            Asset->GetOutermost()->SetDirtyFlag(false);
            Asset->ClearFlags(RF_Public | RF_Standalone);
            Asset->MarkAsGarbage();
        }
        // Only the explicitly tracked, uniquely named Build output is saved
        // under Content; all input source files live in this fixture's Saved dir.
        for (const FString& File : PublishedPackageFiles) IFileManager::Get().Delete(*File, false, true);
        MHShutdownProjectIndex();
        GetMutableDefault<UMHCompositeSettings>()->SourceRoot = PreviousRoot;
        IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
        if (Importer != nullptr) Importer->SetPIEActiveForTests(false);
    }

    FString Name(const TCHAR* Stem) const { return FString(Stem) + TEXT("_") + Suffix; }

    bool Apply(UMHCompositeAsset& Asset, const FMHCompositeDocument& Document)
    {
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(Asset, Document, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(TEXT("freshness fixture: ") + Error);
            return false;
        }
        Asset.SourceRelativePath = Asset.LogicalName + TEXT(".composite");
        Asset.SourceHash = MHRawPayloadHash(Bytes);
        Asset.AppliedHash = Asset.SourceHash;
        return true;
    }

    UMHCompositeAsset* Composite(const FString& LogicalName, const FMHCompositeDocument& Document)
    {
        UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + LogicalName)),
            FName(*LogicalName), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        Asset->LogicalName = LogicalName;
        if (!Apply(*Asset, Document)) return nullptr;
        FAssetRegistryModule::AssetCreated(Asset);
        return Asset;
    }

    UStaticMesh* Mesh(const FString& LogicalName)
    {
        UStaticMesh* Asset = NewObject<UStaticMesh>(CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + LogicalName)),
            FName(*LogicalName), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Asset);
        Receipt->LogicalName = LogicalName;
        Receipt->SourceRelativePath = LogicalName + TEXT(".mesh.fbx");
        Receipt->SourceHash = MHRawPayloadHash(TArray<uint8>{0x6d, 0x68, 0x35});
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Asset->SetAssetImportData(Receipt);
        FAssetRegistryModule::AssetCreated(Asset);
        return Asset;
    }

    bool Prepare()
    {
        if (!Test.TestNotNull(TEXT("freshness test world exists"), World)) return false;
        if (!Test.TestNotNull(TEXT("importer lifecycle is isolated from deferred-preview ticks"), Importer)) return false;
        MeshA = Mesh(Name(TEXT("fresh_mesh_a")));
        MeshB = Mesh(Name(TEXT("fresh_mesh_b")));
        FMHCompositeDocument ChildDocument;
        FMHCompositeNode Group;
        Group.Kind = EMHCompositeNodeKind::Group;
        Group.Name = TEXT("child_group");
        Group.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
        FMHCompositeNode Random;
        Random.Kind = EMHCompositeNodeKind::Random;
        Random.Name = TEXT("nested_choice");
        Random.Transform.TranslationCm = FVector(25.0, 0.0, 0.0);
        for (const UStaticMesh* OptionMesh : {MeshA, MeshB})
        {
            FMHCompositeOption Option;
            Option.Kind = EMHCompositeOptionKind::Mesh;
            Option.Resource = OptionMesh->GetName();
            Option.Weight = 1.0f;
            Random.Options.Add(Option);
        }
        Group.Children.Add(Random);
        ChildDocument.Nodes.Add(Group);
        Child = Composite(Name(TEXT("fresh_child")), ChildDocument);
        if (Child == nullptr) return false;

        FMHCompositeDocument ParentDocument;
        FMHCompositeNode Nested;
        Nested.Kind = EMHCompositeNodeKind::Composite;
        Nested.Resource = Child->LogicalName;
        Nested.Name = TEXT("nested_placement");
        Nested.Transform.TranslationCm = FVector(250.0, 0.0, 0.0);
        ParentDocument.Nodes.Add(Nested);
        FMHCompositeNode Direct;
        Direct.Kind = EMHCompositeNodeKind::Mesh;
        Direct.Resource = MeshA->GetName();
        Direct.Name = TEXT("direct_mesh");
        Direct.Transform.TranslationCm = FVector(600.0, 0.0, 0.0);
        ParentDocument.Nodes.Add(Direct);
        Parent = Composite(Name(TEXT("fresh_parent")), ParentDocument);
        return Parent != nullptr;
    }

    AMHCompositeActor* Spawn(UMHCompositeAsset* Asset, const int32 Seed, const FVector& Location = FVector::ZeroVector)
    {
        FActorSpawnParameters Parameters;
        Parameters.ObjectFlags = RF_Transactional | RF_Transient;
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), FTransform(Location), Parameters);
        if (Test.TestNotNull(TEXT("freshness placement exists"), Actor))
        {
            Actor->SetSeed(Seed);
            if (Asset != nullptr) Actor->SetCompositeAsset(Asset);
        }
        return Actor;
    }

    bool WriteBuildSources()
    {
        FString GoldenRoot;
        if (!ResolveGoldenRoot(Test, GoldenRoot)) return false;
        for (UStaticMesh* Asset : {MeshA, MeshB})
        {
            const FString Path = FPaths::Combine(SourceRoot, Asset->GetName() + TEXT(".mesh.fbx"));
            if (!Test.TestTrue(TEXT("real FBX copied into isolated source root"),
                IFileManager::Get().Copy(*Path, *FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"))) == COPY_OK)) return false;
            TArray<uint8> Bytes;
            if (!Test.TestTrue(TEXT("source FBX read-back"), FFileHelper::LoadFileToArray(Bytes, *Path))) return false;
            CastChecked<UMHStaticMeshImportData>(Asset->GetAssetImportData())->SourceHash = MHRawPayloadHash(Bytes);
        }
        FMHCompositeDocument Document;
        TArray<uint8> Bytes;
        FString Error;
        return Test.TestTrue(TEXT("nested authoring source is published for actual Build admission"),
            MHExtractCompositeV5(*Child, Document, Error) && MHWriteCanonicalCompositeV5(Document, Bytes, Error) &&
            FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(SourceRoot, Child->SourceRelativePath)));
    }

    void AssetUpdatedOnDisk(const UObject& Asset)
    {
        // Exercise the real callback boundary. No preview-cache test helper is
        // used, so this same regression can also run against pre-cache S5.
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.OnAssetUpdatedOnDisk().Broadcast(FAssetData(&Asset));
    }
};

bool BreakFreshnessExpectNoSpawn(FAutomationTestBase& Test, const UWorld& World, const TSet<AActor*>& Before)
{
    const TSet<AActor*> After = BreakFreshnessLiveActors(World);
    bool bSame = Before.Num() == After.Num();
    for (AActor* Actor : Before) bSame &= After.Contains(Actor);
    return Test.TestTrue(TEXT("failed Break changes no level actor membership"), bSame);
}

bool BreakFreshnessExpectLeaves(FAutomationTestBase& Test, const TArray<AActor*>& Actors,
    const FMHResolvedCompositePlan& Plan, const FTransform& Placement)
{
    bool bPassed = Test.TestEqual(TEXT("Break emits exactly the resolved leaf count"), Actors.Num(), Plan.Leaves.Num());
    for (int32 Index = 0; Index < Actors.Num() && Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        const AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actors[Index]);
        bPassed &= Test.TestTrue(TEXT("Break dissolves nested wrappers to standard mesh actors"), MeshActor != nullptr);
        if (MeshActor == nullptr) continue;
        const UStaticMesh* Mesh = MeshActor->GetStaticMeshComponent()->GetStaticMesh();
        bPassed &= Test.TestTrue(TEXT("Break uses the seed-selected resource"), Mesh != nullptr && Mesh->GetName() == Leaf.Resource);
        bPassed &= Test.TestTrue(TEXT("Break preserves parent-local products and actor basis"),
            MeshActor->GetActorTransform().Equals(FTransform(Leaf.WorldMatrix * Placement.ToMatrixWithScale()), KINDA_SMALL_NUMBER));
    }
    return bPassed;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeNestedBuildDeferredFreshnessTest,
    "Mimir.V5.Composite.BreakFreshness.NestedBuildSurvivesDeferredRegistryUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeNestedBuildDeferredFreshnessTest::RunTest(const FString& Parameters)
{
    UMHCompositeLevelSubsystem* Operations = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level operations available"), Operations)) return false;
    FBreakFreshnessFixture Fixture(*this);
    if (!Fixture.Prepare() || !Fixture.WriteBuildSources()) return false;
    AMHCompositeActor* Nested = Fixture.Spawn(Fixture.Child, 100, FVector(1000.0, 0.0, 0.0));
    if (Nested == nullptr) return false;
    AStaticMeshActor* Direct = Fixture.World->SpawnActor<AStaticMeshActor>();
    if (!TestNotNull(TEXT("direct mesh input"), Direct)) return false;
    Direct->SetActorLocation(FVector(1500.0, 0.0, 0.0));
    Direct->GetStaticMeshComponent()->SetStaticMesh(Fixture.MeshA);
    const FMHCompositeAdoptTarget Target{Fixture.SourceRoot, Fixture.Name(TEXT("fresh_built"))};
    Fixture.PublishedPackageFiles.Add(FPackageName::LongPackageNameToFilename(
        TEXT("/Game/MH/Generated/Composites/") + Target.LogicalName, FPackageName::GetAssetPackageExtension()));
    TArray<FString> Warnings;
    FString Error;
    AMHCompositeActor* Built = nullptr;
    const bool bBuilt = Operations->BuildComposite({Nested, Direct}, Target, Built, Warnings, Error);
    if (!TestTrue(*FString::Printf(TEXT("actual nested + mesh Build succeeds: %s"), *Error), bBuilt) || Built == nullptr) return false;
    Fixture.Assets.Add(Built->GetCompositeAsset());
    Built->SetSeed(100);
    const FMHResolvedCompositePlan* Initial = Built->GetResolvedPlan();
    if (!TestNotNull(*FString::Printf(TEXT("built placement initially has a current plan: %s"), *Built->GetLastPlacementError()), Initial)) return false;
    const FMHResolvedCompositePlan Expected = *Initial;
    const FTransform Placement = Built->GetActorTransform();
    AddInfo(FString::Printf(TEXT("Build freshness before AR: seed=%d current=%d error=%s signature=%s"),
        Built->GetSeed(), Built->GetResolvedPlan() != nullptr, *Built->GetLastPlacementError(), *BreakFreshnessSignature(*Built)));
    bool bPassed = TestEqual(TEXT("nested random has one decision"), Expected.Decisions.Num(), 1);
    bPassed &= TestEqual(TEXT("nested plus direct mesh has two leaves"), Expected.Leaves.Num(), 2);
    if (Expected.Leaves.Num() == 2)
    {
        bPassed &= TestEqual(TEXT("nested parent100 plus local25 survives Build at world1125"),
            (Expected.Leaves[0].WorldMatrix * Placement.ToMatrixWithScale()).GetOrigin().X, 1125.0);
        bPassed &= TestEqual(TEXT("direct mesh survives Build at world1500"),
            (Expected.Leaves[1].WorldMatrix * Placement.ToMatrixWithScale()).GetOrigin().X, 1500.0);
    }

    Fixture.AssetUpdatedOnDisk(*Built->GetCompositeAsset());
    AddInfo(FString::Printf(TEXT("Build freshness after AR, before tick: seed=%d current=%d error=%s"),
        Built->GetSeed(), Built->GetResolvedPlan() != nullptr, *Built->GetLastPlacementError()));
    // S5 had no lease gap. A cache implementation may intentionally revoke its
    // plan until the deferred callback; Break must not heal or mutate that gap.
    if (Built->GetResolvedPlan() == nullptr)
    {
        const FBreakFreshnessSnapshot Before(*Built);
        const TSet<AActor*> ActorsBefore = BreakFreshnessLiveActors(*Fixture.World);
        TArray<AActor*> Premature;
        bPassed &= TestFalse(TEXT("Break remains fail-closed until deferred refresh"),
            Operations->BreakComposites({Built}, Premature, Warnings, Error));
        bPassed &= TestTrue(TEXT("pending refresh Break spawns nothing"), Premature.IsEmpty());
        bPassed &= Before.ExpectUnchanged(*this, *Built);
        bPassed &= BreakFreshnessExpectNoSpawn(*this, *Fixture.World, ActorsBefore);
    }
    FTSTicker::GetCoreTicker().Tick(0.0f);
    const FMHResolvedCompositePlan* Refreshed = Built->GetResolvedPlan();
    AddInfo(FString::Printf(TEXT("Build freshness after deferred tick: seed=%d current=%d error=%s signature=%s"),
        Built->GetSeed(), Refreshed != nullptr, *Built->GetLastPlacementError(), *BreakFreshnessSignature(*Built)));
    bPassed &= TestNotNull(TEXT("registry event refreshes the placement automatically on the next tick"), Refreshed);
    if (Refreshed != nullptr)
    {
        bPassed &= TestEqual(TEXT("deferred refresh preserves Seed"), Refreshed->Seed, 100);
        bPassed &= TestEqual(TEXT("deferred refresh preserves the resolved signature"), Refreshed->ResolvedSignature, Expected.ResolvedSignature);
    }
    TArray<AActor*> Broken;
    const bool bBroken = Operations->BreakComposites({Built}, Broken, Warnings, Error);
    bPassed &= TestTrue(*FString::Printf(TEXT("Break after automatic refresh succeeds: %s"), *Error), bBroken);
    bPassed &= BreakFreshnessExpectLeaves(*this, Broken, Expected, Placement);
    bPassed &= TestEqual(TEXT("Break does not change the placement Seed"), Built->GetSeed(), 100);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeBreakFreshnessAtomicRefusalTest,
    "Mimir.V5.Composite.BreakFreshness.StaleSelectionAndInvalidDependencyAreAtomic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeBreakFreshnessAtomicRefusalTest::RunTest(const FString& Parameters)
{
    UMHCompositeLevelSubsystem* Operations = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level operations available"), Operations)) return false;
    FBreakFreshnessFixture Fixture(*this);
    if (!Fixture.Prepare()) return false;
    AMHCompositeActor* First = Fixture.Spawn(Fixture.Parent, 100);
    AMHCompositeActor* BadSecond = Fixture.Spawn(nullptr, 200);
    if (First == nullptr || BadSecond == nullptr || !TestNotNull(TEXT("first placement starts valid"), First->GetResolvedPlan())) return false;
    FMHCompositeDocument Changed;
    FString Error;
    if (!TestTrue(TEXT("nested definition extracts"), MHExtractCompositeV5(*Fixture.Child, Changed, Error))) return false;
    Changed.Nodes[0].Transform.TranslationCm.X = 300.0;
    if (!Fixture.Apply(*Fixture.Child, Changed)) return false;
    Fixture.AssetUpdatedOnDisk(*Fixture.Child);
    const FBreakFreshnessSnapshot FirstBefore(*First);
    const FBreakFreshnessSnapshot SecondBefore(*BadSecond);
    const TSet<AActor*> ActorsBefore = BreakFreshnessLiveActors(*Fixture.World);
    TArray<AActor*> Broken;
    TArray<FString> Warnings;
    bool bPassed = TestFalse(TEXT("stale first plus bad second is rejected as one selection"),
        Operations->BreakComposites({First, BadSecond}, Broken, Warnings, Error));
    bPassed &= TestTrue(TEXT("selection refusal spawns nothing"), Broken.IsEmpty());
    bPassed &= FirstBefore.ExpectUnchanged(*this, *First);
    bPassed &= SecondBefore.ExpectUnchanged(*this, *BadSecond);
    bPassed &= BreakFreshnessExpectNoSpawn(*this, *Fixture.World, ActorsBefore);

    // A malformed newly applied dependency must not resurrect the old valid
    // plan when the registry event is drained. No source authority is bypassed.
    Changed.Nodes[0].Children[0].Options[0].Resource = Fixture.Name(TEXT("missing_mesh"));
    if (!Fixture.Apply(*Fixture.Child, Changed)) return false;
    Fixture.AssetUpdatedOnDisk(*Fixture.Child);
    FTSTicker::GetCoreTicker().Tick(0.0f);
    bPassed &= TestNull(TEXT("invalid changed dependency cannot produce a current plan"), First->GetResolvedPlan());
    const FBreakFreshnessSnapshot InvalidBefore(*First);
    const TSet<AActor*> InvalidActorsBefore = BreakFreshnessLiveActors(*Fixture.World);
    // Failed admission is not a reason to poll/rebuild every frame. With no
    // newer notification, the diagnostic and the already displayed view stay put.
    FTSTicker::GetCoreTicker().Tick(0.0f);
    FTSTicker::GetCoreTicker().Tick(0.0f);
    bPassed &= TestTrue(TEXT("quiet ticks do not rebuild an already rejected placement"),
        First->GetDerivedComponents() == InvalidBefore.Components);
    bPassed &= TestEqual(TEXT("quiet ticks preserve the admission diagnostic"), First->GetLastPlacementError(), InvalidBefore.Error);
    bPassed &= TestEqual(TEXT("quiet ticks do not resurrect the old signature"), BreakFreshnessSignature(*First), InvalidBefore.Signature);
    bPassed &= TestFalse(TEXT("Break stays fail-closed after invalid dependency refresh"),
        Operations->BreakComposites({First}, Broken, Warnings, Error));
    bPassed &= TestTrue(TEXT("invalid dependency refusal retains the underlying diagnostic"),
        Error.Contains(TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE")));
    bPassed &= TestTrue(TEXT("invalid dependency Break spawns nothing"), Broken.IsEmpty());
    bPassed &= InvalidBefore.ExpectUnchanged(*this, *First);
    bPassed &= BreakFreshnessExpectNoSpawn(*this, *Fixture.World, InvalidActorsBefore);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeRebuildBatchFreshnessTest,
    "Mimir.V5.Composite.BreakFreshness.RebuildBatchKeepsEveryPlacementCurrent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeRebuildBatchFreshnessTest::RunTest(const FString& Parameters)
{
    UMHCompositeLevelSubsystem* Operations = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level operations available"), Operations)) return false;
    bool bPassed = true;
    for (const bool bParentBeforeChild : {false, true})
    {
        FBreakFreshnessFixture Fixture(*this);
        if (!Fixture.Prepare()) return false;
        AMHCompositeActor* First = Fixture.Spawn(Fixture.Parent, 100, FVector(1000.0, 0.0, 0.0));
        AMHCompositeActor* Second = Fixture.Spawn(bParentBeforeChild ? Fixture.Child : Fixture.Parent, 200, FVector(2000.0, 0.0, 0.0));
        if (First == nullptr || Second == nullptr) return false;
        TArray<FString> Warnings;
        FString Error;
        const FString Case = bParentBeforeChild ? TEXT("parent-before-child") : TEXT("same-root instances");
        bPassed &= TestTrue(*FString::Printf(TEXT("%s batch Rebuild succeeds"), *Case),
            Operations->RebuildComposites({First, Second}, Warnings, Error));
        const FMHResolvedCompositePlan* FirstPlan = First->GetResolvedPlan();
        const FMHResolvedCompositePlan* SecondPlan = Second->GetResolvedPlan();
        AddInfo(FString::Printf(TEXT("Batch freshness %s: first seed=%d current=%d error=%s; second seed=%d current=%d error=%s"),
            *Case, First->GetSeed(), FirstPlan != nullptr, *First->GetLastPlacementError(),
            Second->GetSeed(), SecondPlan != nullptr, *Second->GetLastPlacementError()));
        bPassed &= TestNotNull(*FString::Printf(TEXT("%s first plan is current after the whole batch"), *Case), FirstPlan);
        bPassed &= TestNotNull(*FString::Printf(TEXT("%s second plan is current after the whole batch"), *Case), SecondPlan);
        TArray<AActor*> Broken;
        bPassed &= TestTrue(*FString::Printf(TEXT("%s subsequent Break succeeds"), *Case),
            Operations->BreakComposites({First, Second}, Broken, Warnings, Error));
        bPassed &= TestEqual(*FString::Printf(TEXT("%s Break emits all leaves"), *Case), Broken.Num(), bParentBeforeChild ? 3 : 4);
        bPassed &= TestEqual(TEXT("batch Rebuild and Break preserve first Seed"), First->GetSeed(), 100);
        bPassed &= TestEqual(TEXT("batch Rebuild and Break preserve second Seed"), Second->GetSeed(), 200);
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
