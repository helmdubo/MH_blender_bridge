#include "Composite/MHEndpointPrototypeRegistry.h"

#include "Composite/MHCompositePlacementMetrics.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "PhysicsEngine/BodySetup.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/**
 * R3a fixture: managed static meshes whose interface can be mutated one axis
 * at a time (payload hash, bounds, material slots, LOD count, collision) with
 * a reimport notification simulated by Registry->Invalidate.
 */
struct FInterfaceFixture
{
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower().Left(10);
    TArray<UObject*> Assets;

    ~FInterfaceFixture()
    {
        if (UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get()) Registry->InvalidateAll();
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

    static FMHResourceKey MeshKey(const FString& LogicalName)
    {
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::StaticMesh;
        Key.LogicalName = LogicalName;
        return Key;
    }

    UMaterial* Material(const TCHAR* Stem)
    {
        const FString MaterialName = Name(Stem);
        UMaterial* Result = NewObject<UMaterial>(
            CreatePackage(*(TEXT("/Game/MimirCompositeTests/InterfaceHash/") + MaterialName)),
            FName(*MaterialName), RF_Public | RF_Standalone);
        Assets.Add(Result);
        return Result;
    }

    UStaticMesh* Mesh(const FString& LogicalName, UMaterial* Slot0)
    {
        UStaticMesh* Result = NewObject<UStaticMesh>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + LogicalName)),
            FName(*LogicalName), RF_Public | RF_Standalone);
        Assets.Add(Result);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Result);
        Receipt->LogicalName = LogicalName;
        Receipt->SourceRelativePath = LogicalName + TEXT(".mesh.fbx");
        Receipt->SourceHash = MHRawPayloadHash({0x69, 0x66, 0x61, 0x63, 0x65, 0x31});
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Result->SetAssetImportData(Receipt);
        Result->GetStaticMaterials().Add(FStaticMaterial(Slot0, TEXT("slot0"), TEXT("slot0")));
        Result->AddSourceModel();
        Result->CreateBodySetup();
        Result->GetBodySetup()->CollisionTraceFlag = CTF_UseDefault;
        return Result;
    }

    static UMHStaticMeshImportData* Receipt(UStaticMesh& Mesh)
    {
        return Cast<UMHStaticMeshImportData>(Mesh.GetAssetImportData());
    }
};

struct FInterfaceSnapshot
{
    uint32 PayloadRevision = 0;
    uint32 BoundsRevision = 0;
    uint64 BucketDescriptorHash = 0;
    uint64 CollisionInterfaceHash = 0;
    uint64 MaterialBindingHash = 0;

    explicit FInterfaceSnapshot(const FMHEndpointPrototype& Prototype)
        : PayloadRevision(Prototype.PayloadRevision)
        , BoundsRevision(Prototype.BoundsRevision)
        , BucketDescriptorHash(Prototype.BucketDescriptorHash)
        , CollisionInterfaceHash(Prototype.CollisionInterfaceHash)
        , MaterialBindingHash(Prototype.MaterialBindingHash)
    {
    }
};

/** Re-admits Key after a simulated reimport and returns the new prototype. */
FMHEndpointPrototype Reimport(UMHEndpointPrototypeRegistry& Registry, const FMHResourceKey& Key)
{
    Registry.Invalidate(Key);
    return Registry.Resolve(Key);
}

} // namespace

// 16 §2.2 П4: a Ready static mesh carries five hashes/revisions and its bounds;
// every re-admission after Revision++ recomputes them and classifies what
// changed, so the §4 reconcile protocol never compares meshes itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEndpointInterfaceHashTest,
    "Mimir.V5.Composite.Reconcile.PrototypeInterfaceHashes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEndpointInterfaceHashTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get();
    if (!TestNotNull(TEXT("endpoint prototype registry"), Registry)) return false;
    FInterfaceFixture Fixture;
    UMaterial* MaterialA = Fixture.Material(TEXT("iface_mat_a"));
    UMaterial* MaterialB = Fixture.Material(TEXT("iface_mat_b"));
    const FString MeshName = Fixture.Name(TEXT("iface_mesh"));
    UStaticMesh* Mesh = Fixture.Mesh(MeshName, MaterialA);
    const FMHResourceKey Key = FInterfaceFixture::MeshKey(MeshName);

    // A. First admission: interface populated, delta = first admission only.
    Registry->InvalidateAll();
    const FMHEndpointPrototype First = Registry->Resolve(Key);
    bool bPassed = TestTrue(TEXT("mesh admits Ready"), First.State == EMHEndpointState::Ready);
    bPassed &= TestNotEqual(TEXT("Ready mesh carries a bucket descriptor hash"), First.BucketDescriptorHash, 0ull);
    bPassed &= TestNotEqual(TEXT("Ready mesh carries a collision interface hash"), First.CollisionInterfaceHash, 0ull);
    bPassed &= TestNotEqual(TEXT("Ready mesh carries a material binding hash"), First.MaterialBindingHash, 0ull);
    const FMHEndpointInterfaceDelta FirstDelta = Registry->GetLastInterfaceDelta(Key);
    bPassed &= TestTrue(TEXT("first admission is reported as such"), FirstDelta.bFirstAdmission);
    bPassed &= TestFalse(TEXT("first admission changes nothing else"),
        FirstDelta.bPayload || FirstDelta.bBounds || FirstDelta.bBucketDescriptor ||
        FirstDelta.bCollisionInterface || FirstDelta.bMaterialBinding);

    // B. Determinism: the same interface hashes the same across re-admissions,
    // and an unchanged mesh reports no delta at all.
    const FInterfaceSnapshot Baseline(First);
    const FMHEndpointPrototype Same = Reimport(*Registry, Key);
    bPassed &= TestTrue(TEXT("unchanged mesh re-admits Ready"), Same.State == EMHEndpointState::Ready);
    bPassed &= TestEqual(TEXT("unchanged payload keeps PayloadRevision"), Same.PayloadRevision, Baseline.PayloadRevision);
    bPassed &= TestEqual(TEXT("unchanged bounds keep BoundsRevision"), Same.BoundsRevision, Baseline.BoundsRevision);
    bPassed &= TestEqual(TEXT("unchanged interface keeps BucketDescriptorHash"), Same.BucketDescriptorHash, Baseline.BucketDescriptorHash);
    bPassed &= TestEqual(TEXT("unchanged interface keeps CollisionInterfaceHash"), Same.CollisionInterfaceHash, Baseline.CollisionInterfaceHash);
    bPassed &= TestEqual(TEXT("unchanged interface keeps MaterialBindingHash"), Same.MaterialBindingHash, Baseline.MaterialBindingHash);
    bPassed &= TestFalse(TEXT("unchanged mesh reports an empty delta"), Registry->GetLastInterfaceDelta(Key).Any());

    // C. Payload only (receipt SourceHash moved, interface identical): the
    // §4 second row, render refresh without migration.
    FInterfaceFixture::Receipt(*Mesh)->SourceHash = MHRawPayloadHash({0x69, 0x66, 0x61, 0x63, 0x65, 0x32});
    const FMHEndpointPrototype Payload = Reimport(*Registry, Key);
    FMHEndpointInterfaceDelta Delta = Registry->GetLastInterfaceDelta(Key);
    bPassed &= TestEqual(TEXT("payload reimport bumps PayloadRevision once"), Payload.PayloadRevision, Baseline.PayloadRevision + 1u);
    bPassed &= TestTrue(TEXT("payload reimport is classified as payload"), Delta.bPayload);
    bPassed &= TestFalse(TEXT("payload reimport touches no interface hash"),
        Delta.bBounds || Delta.bBucketDescriptor || Delta.bCollisionInterface || Delta.bMaterialBinding || Delta.bFirstAdmission);
    bPassed &= TestEqual(TEXT("payload reimport keeps BucketDescriptorHash"), Payload.BucketDescriptorHash, Baseline.BucketDescriptorHash);

    // D. Bounds only.
    Mesh->SetPositiveBoundsExtension(FVector(25.0, 0.0, 0.0));
    const FMHEndpointPrototype Bounds = Reimport(*Registry, Key);
    Delta = Registry->GetLastInterfaceDelta(Key);
    bPassed &= TestEqual(TEXT("bounds change bumps BoundsRevision once"), Bounds.BoundsRevision, Baseline.BoundsRevision + 1u);
    bPassed &= TestTrue(TEXT("bounds change is classified as bounds"), Delta.bBounds);
    bPassed &= TestFalse(TEXT("bounds change is neither payload nor interface"),
        Delta.bPayload || Delta.bBucketDescriptor || Delta.bCollisionInterface || Delta.bMaterialBinding);
    bPassed &= TestTrue(TEXT("prototype bounds follow the extension"), Bounds.Bounds.IsValid && Bounds.Bounds.Max.X >= 25.0);

    // E. Material slot added: bucket descriptor and material binding move,
    // collision does not.
    Mesh->GetStaticMaterials().Add(FStaticMaterial(MaterialB, TEXT("slot1"), TEXT("slot1")));
    const FMHEndpointPrototype Slots = Reimport(*Registry, Key);
    Delta = Registry->GetLastInterfaceDelta(Key);
    bPassed &= TestNotEqual(TEXT("added slot changes BucketDescriptorHash"), Slots.BucketDescriptorHash, Baseline.BucketDescriptorHash);
    bPassed &= TestNotEqual(TEXT("added slot changes MaterialBindingHash"), Slots.MaterialBindingHash, Baseline.MaterialBindingHash);
    bPassed &= TestEqual(TEXT("added slot keeps CollisionInterfaceHash"), Slots.CollisionInterfaceHash, Baseline.CollisionInterfaceHash);
    bPassed &= TestTrue(TEXT("added slot is classified as descriptor + binding"), Delta.bBucketDescriptor && Delta.bMaterialBinding);
    bPassed &= TestFalse(TEXT("added slot is not a collision change"), Delta.bCollisionInterface);

    // F. Default material swapped in an existing slot: binding moves, the
    // descriptor's slot layout stays.
    const FInterfaceSnapshot AfterSlots(Slots);
    Mesh->GetStaticMaterials()[0].MaterialInterface = MaterialB;
    const FMHEndpointPrototype Binding = Reimport(*Registry, Key);
    Delta = Registry->GetLastInterfaceDelta(Key);
    bPassed &= TestNotEqual(TEXT("swapped default material changes MaterialBindingHash"), Binding.MaterialBindingHash, AfterSlots.MaterialBindingHash);
    bPassed &= TestEqual(TEXT("swapped default material keeps BucketDescriptorHash (slot layout unchanged)"), Binding.BucketDescriptorHash, AfterSlots.BucketDescriptorHash);
    bPassed &= TestTrue(TEXT("swapped default material is classified as binding"), Delta.bMaterialBinding);
    bPassed &= TestFalse(TEXT("swapped default material is neither a descriptor nor a collision change"), Delta.bBucketDescriptor || Delta.bCollisionInterface);

    // G. Collision policy: only the collision interface moves.
    const FInterfaceSnapshot AfterBinding(Binding);
    Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
    const FMHEndpointPrototype Collision = Reimport(*Registry, Key);
    Delta = Registry->GetLastInterfaceDelta(Key);
    bPassed &= TestNotEqual(TEXT("trace flag changes CollisionInterfaceHash"), Collision.CollisionInterfaceHash, AfterBinding.CollisionInterfaceHash);
    bPassed &= TestEqual(TEXT("trace flag keeps BucketDescriptorHash"), Collision.BucketDescriptorHash, AfterBinding.BucketDescriptorHash);
    bPassed &= TestEqual(TEXT("trace flag keeps MaterialBindingHash"), Collision.MaterialBindingHash, AfterBinding.MaterialBindingHash);
    bPassed &= TestTrue(TEXT("trace flag is classified as collision"), Delta.bCollisionInterface);
    bPassed &= TestFalse(TEXT("trace flag is nothing else"), Delta.bPayload || Delta.bBounds || Delta.bBucketDescriptor || Delta.bMaterialBinding);

    // H. LOD count: descriptor moves, binding and collision stay.
    const FInterfaceSnapshot AfterCollision(Collision);
    Mesh->AddSourceModel();
    const FMHEndpointPrototype Lods = Reimport(*Registry, Key);
    Delta = Registry->GetLastInterfaceDelta(Key);
    bPassed &= TestNotEqual(TEXT("LOD count changes BucketDescriptorHash"), Lods.BucketDescriptorHash, AfterCollision.BucketDescriptorHash);
    bPassed &= TestEqual(TEXT("LOD count keeps MaterialBindingHash"), Lods.MaterialBindingHash, AfterCollision.MaterialBindingHash);
    bPassed &= TestEqual(TEXT("LOD count keeps CollisionInterfaceHash"), Lods.CollisionInterfaceHash, AfterCollision.CollisionInterfaceHash);
    bPassed &= TestTrue(TEXT("LOD count is classified as descriptor only"),
        Delta.bBucketDescriptor && !Delta.bMaterialBinding && !Delta.bCollisionInterface);

    // I. Non-mesh kinds and Invalid prototypes carry nothing.
    const FString MissingName = Fixture.Name(TEXT("iface_missing"));
    const FMHEndpointPrototype Missing = Registry->Resolve(FInterfaceFixture::MeshKey(MissingName));
    bPassed &= TestTrue(TEXT("absent mesh is Invalid"), Missing.State == EMHEndpointState::Invalid);
    bPassed &= TestEqual(TEXT("Invalid prototype has no descriptor hash"), Missing.BucketDescriptorHash, 0ull);
    bPassed &= TestFalse(TEXT("Invalid prototype has no delta"), Registry->GetLastInterfaceDelta(FInterfaceFixture::MeshKey(MissingName)).Any());

    // J. Preview-plane discipline holds for the hash computation.
    MHResetEndpointResolveMetrics();
    Reimport(*Registry, Key);
    bPassed &= TestEqual(TEXT("hash computation makes no Asset Registry tag queries"), MHGetEndpointResolveMetrics().AssetRegistryTagQueries, 0ull);
    bPassed &= TestEqual(TEXT("hash computation reads no live receipt tags"), MHGetEndpointResolveMetrics().LiveReceiptTagReads, 0ull);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
