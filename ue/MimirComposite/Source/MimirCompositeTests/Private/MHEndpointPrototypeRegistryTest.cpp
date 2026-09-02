#include "Composite/MHEndpointPrototypeRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeDefinitionSubsystem.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeRuntimeBridge.h"
#include "Composite/MHRuntimeCompositeInput.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeProtocol.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{
struct FRegistryFixture
{
    FAutomationTestBase& Test;
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    TArray<UObject*> Assets;
    TSet<UObject*> RegisteredAssets;

    explicit FRegistryFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FRegistryFixture()
    {
        if (UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get())
        {
            Registry->InvalidateAll();
        }
        for (UObject* Asset : RegisteredAssets) FAssetRegistryModule::AssetDeleted(Asset);
        for (UObject* Asset : Assets)
        {
            if (IsValid(Asset))
            {
                Asset->ClearFlags(RF_Public | RF_Standalone);
                Asset->MarkAsGarbage();
            }
        }
    }

    FString Name(const TCHAR* Stem) const { return FString(Stem) + TEXT("_") + Suffix; }

    static FMHResourceKey Key(const EMHResourceKind Kind, const FString& LogicalName)
    {
        FMHResourceKey Result;
        Result.Kind = Kind;
        Result.LogicalName = LogicalName;
        return Result;
    }

    UStaticMesh* Mesh(const FString& LogicalName, const bool bWithReceipt = true, const FString& ReceiptName = FString())
    {
        UStaticMesh* Mesh = NewObject<UStaticMesh>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + LogicalName)),
            FName(*LogicalName), RF_Public | RF_Standalone);
        Assets.Add(Mesh);
        if (bWithReceipt)
        {
            UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
            Receipt->LogicalName = ReceiptName.IsEmpty() ? LogicalName : ReceiptName;
            Receipt->SourceRelativePath = LogicalName + TEXT(".mesh.fbx");
            const TArray<uint8> SyntheticPayload = {0x72, 0x65, 0x67, 0x69, 0x73};
            Receipt->SourceHash = MHRawPayloadHash(SyntheticPayload);
            Receipt->ImporterVersion = MHStaticMeshImporterVersion;
            Mesh->SetAssetImportData(Receipt);
        }
        return Mesh;
    }

    void Register(UObject& Asset)
    {
        FAssetRegistryModule::AssetCreated(&Asset);
        RegisteredAssets.Add(&Asset);
    }

    void RemoveClaim(UObject& Asset)
    {
        if (RegisteredAssets.Remove(&Asset) > 0) FAssetRegistryModule::AssetDeleted(&Asset);
        Assets.Remove(&Asset);
        Asset.ClearFlags(RF_Public | RF_Standalone);
        Asset.MarkAsGarbage();
    }

    UMHCompositeAsset* Composite(const FString& LogicalName, const FString& MeshName, const bool bAliasPath = false)
    {
        FMHCompositeDocument Document;
        FMHCompositeNode& Node = Document.Nodes.AddDefaulted_GetRef();
        Node.Kind = EMHCompositeNodeKind::Mesh;
        Node.Resource = MeshName;
        const FString PackageName = (bAliasPath
            ? TEXT("/Game/MimirCompositeTests/RegistryAlias/")
            : TEXT("/Game/MH/Generated/Composites/")) + LogicalName;
        UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
            CreatePackage(*PackageName),
            FName(*LogicalName), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        Asset->LogicalName = LogicalName;
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(*Asset, Document, {}, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(TEXT("registry fixture apply failed: ") + Error);
            return nullptr;
        }
        Asset->SourceRelativePath = LogicalName + TEXT(".composite");
        Asset->SourceHash = MHRawPayloadHash(Bytes);
        Asset->AppliedHash = Asset->SourceHash;
        return Asset;
    }
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPrototypeRegistryIdentityAdmissionTest,
    "Mimir.V5.Composite.Registry.IdentityAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPrototypeRegistryIdentityAdmissionTest::RunTest(const FString& Parameters)
{
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (!TestNotNull(TEXT("endpoint prototype registry subsystem"), Registry))
    {
        return false;
    }
    FRegistryFixture Fixture(*this);
    bool bPassed = true;

    // A. Registry contract: canonical path, structural receipt, no stickiness.
    const FString MeshName = Fixture.Name(TEXT("r0_mesh"));
    UStaticMesh* Mesh = Fixture.Mesh(MeshName);
    const FMHResourceKey MeshKey = FRegistryFixture::Key(EMHResourceKind::StaticMesh, MeshName);
    MHResetDefinitionCacheMetrics();
    MHResetEndpointResolveMetrics();
    const FMHEndpointPrototype Ready = Registry->Resolve(MeshKey);
    bPassed &= TestTrue(TEXT("managed mesh is admitted Ready"), Ready.State == EMHEndpointState::Ready);
    bPassed &= TestEqual(TEXT("admitted prototype holds the canonical object"), Ready.Object.Get(), static_cast<UObject*>(Mesh));
    Registry->Resolve(MeshKey);
    FMHDefinitionCacheMetrics Cache = MHGetDefinitionCacheMetrics();
    bPassed &= TestEqual(TEXT("second resolve is a hit"), Cache.EndpointHits, 1ull);
    bPassed &= TestEqual(TEXT("one physical resolve per key"), Cache.EndpointResolves, 1ull);
    FMHEndpointResolveMetrics Endpoints = MHGetEndpointResolveMetrics();
    bPassed &= TestEqual(TEXT("one identity admission per key"), Endpoints.IdentityAdmissions, 1ull);
    bPassed &= TestEqual(TEXT("registry reads no live receipt tags"), Endpoints.LiveReceiptTagReads, 0ull);
    bPassed &= TestEqual(TEXT("resident object is not a sync package load"), Endpoints.PackageLoadsSync, 0ull);

    const FMHResourceKey MissingKey = FRegistryFixture::Key(EMHResourceKind::StaticMesh, Fixture.Name(TEXT("r0_missing")));
    const FMHEndpointPrototype Missing = Registry->Resolve(MissingKey);
    bPassed &= TestTrue(TEXT("absent object is Invalid"), Missing.State == EMHEndpointState::Invalid);
    bPassed &= TestNull(TEXT("absent object has no prototype object"), Missing.Object.Get());

    const FString BareName = Fixture.Name(TEXT("r0_bare"));
    Fixture.Mesh(BareName, false);
    FString Error;
    bPassed &= TestNull(TEXT("mesh without receipt is refused"),
        Registry->ResolveObject(FRegistryFixture::Key(EMHResourceKind::StaticMesh, BareName), Error));
    bPassed &= TestTrue(TEXT("missing receipt names the diagnostic"), Error.Contains(TEXT("has no matching managed mesh receipt")));

    const FString ForeignName = Fixture.Name(TEXT("r0_foreign"));
    Fixture.Mesh(ForeignName, true, Fixture.Name(TEXT("r0_other")));
    Error.Reset();
    bPassed &= TestNull(TEXT("receipt with a foreign logical name is refused"),
        Registry->ResolveObject(FRegistryFixture::Key(EMHResourceKind::StaticMesh, ForeignName), Error));
    bPassed &= TestTrue(TEXT("foreign receipt names the diagnostic"), Error.Contains(TEXT("has no matching managed mesh receipt")));

    const uint32 RevisionBefore = Registry->GetRevision(MeshKey);
    Registry->Invalidate(MeshKey);
    bPassed &= TestEqual(TEXT("invalidation bumps the revision"), Registry->GetRevision(MeshKey), RevisionBefore + 1u);
    MHResetDefinitionCacheMetrics();
    bPassed &= TestTrue(TEXT("invalidated key re-admits Ready"), Registry->Resolve(MeshKey).State == EMHEndpointState::Ready);
    bPassed &= TestEqual(TEXT("re-admission is one physical resolve"), MHGetDefinitionCacheMetrics().EndpointResolves, 1ull);

    // B. Two placements of one asset resolve every unique endpoint exactly once
    // through the registry, without Asset Registry tag reads on live objects.
    const FString RootName = Fixture.Name(TEXT("r0_root"));
    UMHCompositeAsset* Root = Fixture.Composite(RootName, MeshName);
    if (Root == nullptr) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    if (!TestNotNull(TEXT("registry placement world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    if (GEditor != nullptr)
    {
        if (UMHCompositeDefinitionSubsystem* Definitions = GEditor->GetEditorSubsystem<UMHCompositeDefinitionSubsystem>())
        {
            Definitions->InvalidateAllDefinitions();
        }
    }
    Registry->InvalidateAll();
    MHResetDefinitionCacheMetrics();
    MHResetEndpointResolveMetrics();
    for (int32 Index = 0; Index < 2; ++Index)
    {
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
        if (!TestNotNull(TEXT("registry placement actor"), Actor)) return false;
        Actor->SetAutoSeed(false);
        Actor->SetSeed(100);
        Actor->SetCompositeAsset(Root);
        bPassed &= TestNotNull(TEXT("placement of a managed root has a plan"), Actor->GetResolvedPlan());
    }
    Endpoints = MHGetEndpointResolveMetrics();
    Cache = MHGetDefinitionCacheMetrics();
    const uint64 UniqueKeys = 2; // root composite + one mesh
    AddInfo(FString::Printf(
        TEXT("MH_PERF_ENDPOINTS registry two placements: unique_keys=%llu registry_lookups=%llu asset_registry_tag_queries=%llu package_loads_sync=%llu identity_admissions=%llu live_receipt_tag_reads=%llu endpoint_hits=%llu"),
        UniqueKeys, Endpoints.RegistryLookups, Endpoints.AssetRegistryTagQueries, Endpoints.PackageLoadsSync,
        Endpoints.IdentityAdmissions, Endpoints.LiveReceiptTagReads, Cache.EndpointHits));
    bPassed &= TestEqual(TEXT("two placements resolve each unique key once"), Endpoints.RegistryLookups, UniqueKeys);
    bPassed &= TestEqual(TEXT("two placements admit each unique key once"), Endpoints.IdentityAdmissions, UniqueKeys);
    bPassed &= TestEqual(TEXT("preview reads no live receipt tags"), Endpoints.LiveReceiptTagReads, 0ull);
    // D0b П2 (OPEN-R-7 closed): the preview plane makes no Asset Registry tag
    // queries at all; duplicate claims belong to the source and proof planes.
    bPassed &= TestEqual(TEXT("preview makes no Asset Registry tag queries"), Endpoints.AssetRegistryTagQueries, 0ull);
    bPassed &= TestTrue(TEXT("repeated endpoints are registry hits"), Cache.EndpointHits >= 1ull);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRegistryDuplicateClaimProofPlaneTest,
    "Mimir.V5.Composite.Registry.DuplicateClaimIsProofPlane",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRegistryDuplicateClaimProofPlaneTest::RunTest(const FString& Parameters)
{
    // D0b П2: two Asset Registry claims on one logical name never block the
    // preview (it resolves the canonical path); Break and the runtime snapshot
    // are proof-plane exit points and refuse with MH_E_AMBIGUOUS_GENERATED_ASSET.
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    UMHCompositeLevelSubsystem* Operations = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("endpoint prototype registry subsystem"), Registry) ||
        !TestNotNull(TEXT("level operations subsystem"), Operations))
    {
        return false;
    }
    FRegistryFixture Fixture(*this);
    const FString MeshName = Fixture.Name(TEXT("r0c_mesh"));
    Fixture.Mesh(MeshName);
    const FString RootName = Fixture.Name(TEXT("r0c_root"));
    UMHCompositeAsset* Root = Fixture.Composite(RootName, MeshName);
    if (Root == nullptr) return false;
    Fixture.Register(*Root);
    UMHCompositeAsset* Duplicate = Fixture.Composite(RootName, MeshName, true);
    if (Duplicate == nullptr) return false;
    Fixture.Register(*Duplicate);
    bool bPassed = TestNotEqual(TEXT("duplicate claims live at distinct object paths"), Root->GetPathName(), Duplicate->GetPathName());

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    if (!TestNotNull(TEXT("duplicate-claim world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    Registry->InvalidateAll();
    MHResetEndpointResolveMetrics();
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("duplicate-claim placement"), Actor)) return false;
    Actor->SetAutoSeed(false);
    Actor->SetSeed(100);
    Actor->SetCompositeAsset(Root);
    bPassed &= TestNotNull(TEXT("preview resolves the canonical path despite a duplicate claim"), Actor->GetResolvedPlan());
    bPassed &= TestEqual(TEXT("preview made no tag queries"), MHGetEndpointResolveMetrics().AssetRegistryTagQueries, 0ull);

    TArray<AActor*> Broken;
    TArray<FString> Warnings;
    FString Error;
    bPassed &= TestFalse(TEXT("Break refuses the duplicate claim"), Operations->BreakComposites({Actor}, Broken, Warnings, Error));
    bPassed &= TestTrue(TEXT("Break names MH_E_AMBIGUOUS_GENERATED_ASSET"), Error.Contains(TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET")));
    bPassed &= TestTrue(TEXT("refused Break spawns nothing"), Broken.IsEmpty());

    FMHRuntimeCompositeInput Input;
    Error.Reset();
    bPassed &= TestFalse(TEXT("runtime snapshot refuses the duplicate claim"), MHBuildRuntimeCompositeInput(*Actor, Input, Error));
    bPassed &= TestTrue(TEXT("snapshot names MH_E_AMBIGUOUS_GENERATED_ASSET"), Error.Contains(TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET")));

    Fixture.RemoveClaim(*Duplicate);
    Error.Reset();
    Broken.Reset();
    bPassed &= TestTrue(TEXT("removing the duplicate claim heals Break"), Operations->BreakComposites({Actor}, Broken, Warnings, Error));
    if (!Error.IsEmpty()) AddInfo(Error);
    return bPassed;
}
} // namespace UE::MimirComposite::Tests
