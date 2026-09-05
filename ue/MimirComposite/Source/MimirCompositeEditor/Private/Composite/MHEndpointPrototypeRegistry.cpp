#include "Composite/MHEndpointPrototypeRegistry.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Containers/StringConv.h"
#include "Hash/CityHash.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMeshResources.h"
#include "Texture/MHTextureSourceData.h"
#include "UObject/UObjectGlobals.h"

using namespace UE::MimirComposite;

namespace UE::MimirComposite
{
FString MHEndpointObjectPath(const FMHResourceKey& Key)
{
    const TCHAR* Folder = nullptr;
    switch (Key.Kind)
    {
    case EMHResourceKind::Composite: Folder = TEXT("Composites"); break;
    case EMHResourceKind::StaticMesh: Folder = TEXT("Meshes"); break;
    case EMHResourceKind::Material: Folder = TEXT("Materials"); break;
    case EMHResourceKind::Texture: Folder = TEXT("Textures"); break;
    default: return FString();
    }
    return FString::Printf(TEXT("/Game/MH/Generated/%s/%s.%s"), Folder, *Key.LogicalName, *Key.LogicalName);
}

namespace
{
/** Explicit framing and endian order; no UObject addresses, FName ids or padding. */
struct FMHEndpointInterfaceBytes
{
    TArray<uint8> Data;

    explicit FMHEndpointInterfaceBytes(const TCHAR* Domain)
    {
        Data.Reserve(128);
        Text(Domain);
    }

    void U8(uint8 Value) { Data.Add(Value); }
    void U32(uint32 Value)
    {
        for (int32 Shift = 0; Shift < 32; Shift += 8) U8(static_cast<uint8>(Value >> Shift));
    }
    void Double(double Value)
    {
        uint64 Bits;
        static_assert(sizeof(Bits) == sizeof(Value));
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        for (int32 Shift = 0; Shift < 64; Shift += 8) U8(static_cast<uint8>(Bits >> Shift));
    }
    void Vector(const FVector& Value) { Double(Value.X); Double(Value.Y); Double(Value.Z); }
    void Text(const FString& Value)
    {
        const FTCHARToUTF8 Utf8(*Value);
        U32(static_cast<uint32>(Utf8.Length()));
        Data.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    }
    uint64 Hash() const
    {
        const uint64 Value = CityHash64(reinterpret_cast<const char*>(Data.GetData()), static_cast<uint32>(Data.Num()));
        return Value == 0 ? 1 : Value;
    }
};

struct FMHEndpointMeshInterface
{
    FBox Bounds = FBox(ForceInit);
    TArray<uint8> BoundsInput;
    uint64 BucketDescriptorHash = 0;
    uint64 CollisionInterfaceHash = 0;
    uint64 MaterialBindingHash = 0;
};

FMHEndpointMeshInterface MHReadEndpointMeshInterface(const UStaticMesh& Mesh)
{
    FMHEndpointMeshInterface Result;
    const FBoxSphereBounds Extended = Mesh.GetExtendedBounds();
    const FVector Positive = Mesh.GetPositiveBoundsExtension();
    const FVector Negative = Mesh.GetNegativeBoundsExtension();
    FMHEndpointInterfaceBytes Bounds(TEXT("mh.endpoint.bounds:1"));
    Bounds.Vector(Extended.Origin);
    Bounds.Vector(Extended.BoxExtent);
    Bounds.Double(Extended.SphereRadius);
    Bounds.Vector(Positive);
    Bounds.Vector(Negative);
    Result.BoundsInput = MoveTemp(Bounds.Data);
    Result.Bounds = Extended.GetBox();
    // Synthetic meshes have no render bounds. Real extended bounds already
    // include extensions and must not receive them a second time.
    if (Extended.Origin == FVector::ZeroVector && Extended.BoxExtent == FVector::ZeroVector && Extended.SphereRadius == 0.0)
    {
        Result.Bounds.Min -= Negative;
        Result.Bounds.Max += Positive;
    }

    FMHEndpointInterfaceBytes Bucket(TEXT("mh.endpoint.bucket:1"));
    FMHEndpointInterfaceBytes Binding(TEXT("mh.endpoint.binding:1"));
    const TArray<FStaticMaterial>& Slots = Mesh.GetStaticMaterials();
    Bucket.U32(static_cast<uint32>(Slots.Num()));
    Binding.U32(static_cast<uint32>(Slots.Num()));
    for (const FStaticMaterial& Slot : Slots)
    {
        const FString SlotName = Slot.MaterialSlotName.ToString();
        Bucket.Text(SlotName);
        Binding.Text(SlotName);
        // TObjectPtr obtains paths without resolving unloaded object handles.
        // OPEN-R3A-1: material identities never enter the bucket descriptor.
        Binding.Text(Slot.MaterialInterface.GetPathName());
        Binding.Text(Slot.OverlayMaterialInterface.GetPathName());
    }
    Bucket.U32(static_cast<uint32>(Mesh.GetNumSourceModels()));
    if (const FStaticMeshRenderData* RenderData = Mesh.GetRenderData())
    {
        Bucket.U32(static_cast<uint32>(RenderData->LODResources.Num()));
        for (const FStaticMeshLODResources& Lod : RenderData->LODResources)
        {
            Bucket.U32(static_cast<uint32>(Lod.Sections.Num()));
            for (const FStaticMeshSection& Section : Lod.Sections)
            {
                Bucket.U32(Section.MaterialIndex);
                Bucket.U8(Section.bEnableCollision ? 1 : 0);
                Bucket.U8(Section.bCastShadow ? 1 : 0);
            }
        }
    }
    Result.BucketDescriptorHash = Bucket.Hash();
    Result.MaterialBindingHash = Binding.Hash();

    FMHEndpointInterfaceBytes Collision(TEXT("mh.endpoint.collision:1"));
    const UBodySetup* Body = Mesh.GetBodySetup();
    Collision.U8(Body != nullptr ? 1 : 0);
    if (Body != nullptr)
    {
        Collision.U8(static_cast<uint8>(Body->CollisionTraceFlag));
        Collision.U8(Body->bDoubleSidedGeometry ? 1 : 0);
        Collision.U32(static_cast<uint32>(Body->AggGeom.SphereElems.Num()));
        Collision.U32(static_cast<uint32>(Body->AggGeom.BoxElems.Num()));
        Collision.U32(static_cast<uint32>(Body->AggGeom.SphylElems.Num()));
        Collision.U32(static_cast<uint32>(Body->AggGeom.ConvexElems.Num()));
        Collision.U32(static_cast<uint32>(Body->AggGeom.TaperedCapsuleElems.Num()));
        Collision.U32(static_cast<uint32>(Body->AggGeom.LevelSetElems.Num()));
        Collision.U32(static_cast<uint32>(Body->AggGeom.SkinnedLevelSetElems.Num()));
        Collision.U32(static_cast<uint32>(Body->AggGeom.MLLevelSetElems.Num()));
        Collision.U32(static_cast<uint32>(Body->AggGeom.SkinnedTriangleMeshElems.Num()));
        Collision.Text(Body->DefaultInstance.GetCollisionProfileName().ToString());
    }
    Result.CollisionInterfaceHash = Collision.Hash();
    return Result;
}

bool ReceiptSourcePathMatchesKey(const FString& SourcePath, const FMHResourceKey& Key)
{
    TArray<FString> Segments;
    SourcePath.ParseIntoArray(Segments, TEXT("/"), false);
    FMHResourceKey PathKey;
    FString PathError;
    return !SourcePath.IsEmpty() && FPaths::IsRelative(SourcePath) &&
        !SourcePath.Contains(TEXT("\\")) && !SourcePath.StartsWith(TEXT("/")) &&
        !SourcePath.EndsWith(TEXT("/")) && !SourcePath.Contains(TEXT("//")) &&
        !Segments.ContainsByPredicate([](const FString& Part) { return Part.IsEmpty() || Part == TEXT(".") || Part == TEXT(".."); }) &&
        MHResourceKeyFromSourceFile(SourcePath, PathKey, PathError) && PathKey == Key;
}

bool AdmitReceiptFields(
    const FMHResourceKey& Key, const UObject& Object, const FString& SourcePath,
    const FString& SourceHash, const FString& AppliedHash, FString& OutError)
{
    const bool bPathValid = ReceiptSourcePathMatchesKey(SourcePath, Key);
    const bool bCanonicalObject = Object.GetPathName() == MHEndpointObjectPath(Key);
    const bool bSourceHashValid = MHIsCanonicalRawPayloadHash(SourceHash);
    const bool bAppliedHashValid = MHIsCanonicalRawPayloadHash(AppliedHash);
    if (bPathValid && bCanonicalObject && bSourceHashValid && bAppliedHashValid)
    {
        return true;
    }
    OutError = FString::Printf(
        TEXT("MH_E_SOURCE_INDEX_INVALID: invalid managed receipt for %s at %s (source_path='%s', path_valid=%d, canonical_object=%d, source_hash_valid=%d, applied_hash_valid=%d)"),
        *Key.ToString(), *Object.GetPathName(), *SourcePath, bPathValid, bCanonicalObject,
        bSourceHashValid, bAppliedHashValid);
    return false;
}

bool MissingReceipt(const FMHResourceKey& Key, const TCHAR* KindLabel, FString& OutError)
{
    OutError = FString::Printf(TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: %s has no matching managed %s receipt"),
        *Key.ToString(), KindLabel);
    return false;
}

bool ResourceKindFromLabel(const FString& Label, EMHResourceKind& OutKind)
{
    for (const EMHResourceKind Kind : {EMHResourceKind::Composite, EMHResourceKind::StaticMesh,
        EMHResourceKind::Material, EMHResourceKind::Texture})
    {
        if (Label == MHResourceKindLabel(Kind))
        {
            OutKind = Kind;
            return true;
        }
    }
    return false;
}
} // namespace

bool MHAdmitEndpointIdentity(const FMHResourceKey& Key, const UObject& Object, FString& OutError)
{
    // Read-only queries; the asset-user-data accessors are non-const by API only.
    UObject& Mutable = const_cast<UObject&>(Object);
    switch (Key.Kind)
    {
    case EMHResourceKind::Composite:
    {
        const UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(&Object);
        if (Asset == nullptr || Asset->LogicalName != Key.LogicalName) return MissingReceipt(Key, TEXT("composite"), OutError);
        return AdmitReceiptFields(Key, Object, Asset->SourceRelativePath, Asset->SourceHash, Asset->AppliedHash, OutError);
    }
    case EMHResourceKind::StaticMesh:
    {
        const UStaticMesh* Mesh = Cast<UStaticMesh>(&Object);
        const UMHStaticMeshImportData* Receipt = Mesh != nullptr ? Cast<UMHStaticMeshImportData>(Mesh->GetAssetImportData()) : nullptr;
        if (Receipt == nullptr || Receipt->LogicalName != Key.LogicalName) return MissingReceipt(Key, TEXT("mesh"), OutError);
        // Binary kind: AppliedHash == SourceHash by definition (10 §7).
        return AdmitReceiptFields(Key, Object, Receipt->SourceRelativePath, Receipt->SourceHash, Receipt->SourceHash, OutError);
    }
    case EMHResourceKind::Material:
    {
        UMaterialInstanceConstant* Material = Cast<UMaterialInstanceConstant>(&Mutable);
        const UMHMaterialSourceData* Receipt = Material != nullptr
            ? Cast<UMHMaterialSourceData>(Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass())) : nullptr;
        if (Receipt == nullptr || Receipt->LogicalName != Key.LogicalName) return MissingReceipt(Key, TEXT("material"), OutError);
        return AdmitReceiptFields(Key, Object, Receipt->SourceRelativePath, Receipt->SourceHash, Receipt->AppliedHash, OutError);
    }
    case EMHResourceKind::Texture:
    {
        UTexture* Texture = Cast<UTexture>(&Mutable);
        const UMHTextureSourceData* Receipt = Texture != nullptr
            ? Cast<UMHTextureSourceData>(Texture->GetAssetUserDataOfClass(UMHTextureSourceData::StaticClass())) : nullptr;
        if (Receipt == nullptr || Receipt->LogicalName != Key.LogicalName) return MissingReceipt(Key, TEXT("texture"), OutError);
        return AdmitReceiptFields(Key, Object, Receipt->SourceRelativePath, Receipt->SourceHash, Receipt->SourceHash, OutError);
    }
    default:
        OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: ") + Key.ToString() + TEXT(" has no generated asset kind");
        return false;
    }
}
} // namespace UE::MimirComposite

void UMHEndpointPrototypeRegistry::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    AssetsAddedHandle = Registry.OnAssetsAdded().AddUObject(this, &UMHEndpointPrototypeRegistry::OnAssetsChanged);
    AssetsRemovedHandle = Registry.OnAssetsRemoved().AddUObject(this, &UMHEndpointPrototypeRegistry::OnAssetsChanged);
}

void UMHEndpointPrototypeRegistry::Deinitialize()
{
    if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
    {
        if (IAssetRegistry* Registry = IAssetRegistry::Get())
        {
            Registry->OnAssetsAdded().Remove(AssetsAddedHandle);
            Registry->OnAssetsRemoved().Remove(AssetsRemovedHandle);
        }
    }
    AssetsAddedHandle.Reset();
    AssetsRemovedHandle.Reset();
    Prototypes.Reset();
    ReadyMeshInterfaces.Reset();
    Super::Deinitialize();
}

UMHEndpointPrototypeRegistry* UMHEndpointPrototypeRegistry::Get()
{
    return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHEndpointPrototypeRegistry>() : nullptr;
}

UObject* UMHEndpointPrototypeRegistry::ResolveEndpoint(const FMHResourceKey& Key, FString& OutError)
{
    if (UMHEndpointPrototypeRegistry* Registry = Get())
    {
        return Registry->ResolveObject(Key, OutError);
    }
    OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: endpoint prototype registry unavailable for ") + Key.ToString();
    return nullptr;
}

const FMHEndpointPrototype& UMHEndpointPrototypeRegistry::Resolve(const FMHResourceKey& Key)
{
    // The definition endpoint metrics keep their S6.5 meaning: leaf (mesh)
    // endpoints only. Composite roots are prototypes too, but the M0
    // registry counters, not these, measure them.
    const bool bLeafEndpoint = Key.Kind == EMHResourceKind::StaticMesh;
    FMHEndpointPrototype& Prototype = Prototypes.FindOrAdd(Key);
    if (Prototype.State == EMHEndpointState::Ready)
    {
        if (Prototype.Object.IsValid())
        {
            if (bLeafEndpoint) MHRecordDefinitionEndpointHit();
            return Prototype;
        }
        if (bLeafEndpoint) MHRecordDefinitionDeadEndpointReload();
    }
    // Unresolved, Invalid and dead prototypes re-admit: an invalid key never
    // becomes sticky, so an in-memory repair heals on the next resolve.
    Admit(Key, Prototype);
    return Prototype;
}

UObject* UMHEndpointPrototypeRegistry::ResolveObject(const FMHResourceKey& Key, FString& OutError)
{
    const FMHEndpointPrototype& Prototype = Resolve(Key);
    if (Prototype.State == EMHEndpointState::Ready)
    {
        return Prototype.Object.Get();
    }
    if (!Prototype.AdmissionError.IsEmpty())
    {
        OutError = Prototype.AdmissionError;
    }
    return nullptr;
}

UStaticMesh* UMHEndpointPrototypeRegistry::ResolveMeshForPreview(
    const FMHResourceKey& Key, const UMHCompositeSettings& Settings, bool& bOutPlaceholder, FString& OutError)
{
    // R4 red stub: today's synchronous path.
    static_cast<void>(Settings);
    bOutPlaceholder = false;
    return Cast<UStaticMesh>(ResolveObject(Key, OutError));
}

bool UMHEndpointPrototypeRegistry::FlushAsyncLoadsForTests()
{
    // R4 red stub: nothing is ever in flight.
    return true;
}

void UMHEndpointPrototypeRegistry::Invalidate(const FMHResourceKey& Key)
{
    if (FMHEndpointPrototype* Prototype = Prototypes.Find(Key))
    {
        const uint32 NextRevision = Prototype->Revision + 1u;
        *Prototype = FMHEndpointPrototype();
        Prototype->Revision = NextRevision;
    }
}

void UMHEndpointPrototypeRegistry::InvalidateAll()
{
    for (TPair<FMHResourceKey, FMHEndpointPrototype>& Pair : Prototypes)
    {
        const uint32 NextRevision = Pair.Value.Revision + 1u;
        Pair.Value = FMHEndpointPrototype();
        Pair.Value.Revision = NextRevision;
    }
}

FMHEndpointInterfaceDelta UMHEndpointPrototypeRegistry::GetLastInterfaceDelta(const FMHResourceKey& Key) const
{
    const FMHEndpointPrototype* Prototype = Prototypes.Find(Key);
    if (Key.Kind != EMHResourceKind::StaticMesh || Prototype == nullptr ||
        Prototype->State != EMHEndpointState::Ready || !Prototype->Object.IsValid()) return FMHEndpointInterfaceDelta();
    const FReadyMeshInterface* Interface = ReadyMeshInterfaces.Find(Key);
    return Interface != nullptr ? Interface->Delta : FMHEndpointInterfaceDelta();
}

uint32 UMHEndpointPrototypeRegistry::GetRevision(const FMHResourceKey& Key) const
{
    const FMHEndpointPrototype* Prototype = Prototypes.Find(Key);
    return Prototype != nullptr ? Prototype->Revision : 0u;
}

void UMHEndpointPrototypeRegistry::OnAssetsChanged(TConstArrayView<FAssetData> Assets)
{
    // Tags come from the registry event payload, never from a live object.
    for (const FAssetData& Asset : Assets)
    {
        FString KindLabel;
        FString LogicalName;
        if (!Asset.GetTagValue(TEXT("MH.Kind"), KindLabel) ||
            !Asset.GetTagValue(TEXT("MH.LogicalName"), LogicalName))
        {
            continue;
        }
        FMHResourceKey Key;
        if (!ResourceKindFromLabel(KindLabel, Key.Kind)) continue;
        Key.LogicalName = LogicalName;
        Invalidate(Key);
    }
}

void UMHEndpointPrototypeRegistry::Admit(const FMHResourceKey& Key, FMHEndpointPrototype& Prototype)
{
    MHRecordEndpointRegistryLookup();
    if (Key.Kind == EMHResourceKind::StaticMesh) MHRecordDefinitionEndpointResolve();
    const uint32 Revision = Prototype.Revision;
    Prototype = FMHEndpointPrototype();
    Prototype.Revision = Revision;
    Prototype.State = EMHEndpointState::Invalid;
    if (!Key.IsCanonical())
    {
        Prototype.AdmissionError = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: ") + Key.ToString();
        return;
    }
    const FString Path = MHEndpointObjectPath(Key);
    if (Path.IsEmpty())
    {
        Prototype.AdmissionError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: ") + Key.ToString() + TEXT(" has no generated asset kind");
        return;
    }

    UObject* Loaded = FindObject<UObject>(nullptr, *Path);
    if (Loaded == nullptr)
    {
        Loaded = LoadObject<UObject>(nullptr, *Path);
        if (Loaded != nullptr)
        {
            MHRecordEndpointPackageLoadSync();
        }
    }
    if (Loaded == nullptr)
    {
        // Absent object: no diagnostic here; the caller names the reference.
        return;
    }
    MHRecordEndpointIdentityAdmission();
    FString Error;
    Prototype.Object = Loaded;
    if (!MHAdmitEndpointIdentity(Key, *Loaded, Error))
    {
        Prototype.AdmissionError = MoveTemp(Error);
        return;
    }
    if (Key.Kind == EMHResourceKind::StaticMesh)
    {
        const UStaticMesh& Mesh = *CastChecked<UStaticMesh>(Loaded);
        const UMHStaticMeshImportData& Receipt = *CastChecked<UMHStaticMeshImportData>(Mesh.GetAssetImportData());
        FMHEndpointMeshInterface Values = MHReadEndpointMeshInterface(Mesh);
        FReadyMeshInterface Current;
        Current.SourceHash = Receipt.SourceHash;
        Current.ImporterVersion = Receipt.ImporterVersion;
        Current.BoundsInput = MoveTemp(Values.BoundsInput);
        Current.BucketDescriptorHash = Values.BucketDescriptorHash;
        Current.CollisionInterfaceHash = Values.CollisionInterfaceHash;
        Current.MaterialBindingHash = Values.MaterialBindingHash;
        if (const FReadyMeshInterface* Previous = ReadyMeshInterfaces.Find(Key))
        {
            Current.Delta.bPayload = Current.SourceHash != Previous->SourceHash || Current.ImporterVersion != Previous->ImporterVersion;
            Current.Delta.bBounds = Current.BoundsInput != Previous->BoundsInput;
            Current.Delta.bBucketDescriptor = Current.BucketDescriptorHash != Previous->BucketDescriptorHash;
            Current.Delta.bCollisionInterface = Current.CollisionInterfaceHash != Previous->CollisionInterfaceHash;
            Current.Delta.bMaterialBinding = Current.MaterialBindingHash != Previous->MaterialBindingHash;
            Current.PayloadRevision = Previous->PayloadRevision + static_cast<uint32>(Current.Delta.bPayload);
            Current.BoundsRevision = Previous->BoundsRevision + static_cast<uint32>(Current.Delta.bBounds);
        }
        else
        {
            Current.Delta.bFirstAdmission = true;
        }
        Prototype.Bounds = Values.Bounds;
        Prototype.PayloadRevision = Current.PayloadRevision;
        Prototype.BoundsRevision = Current.BoundsRevision;
        Prototype.BucketDescriptorHash = Current.BucketDescriptorHash;
        Prototype.CollisionInterfaceHash = Current.CollisionInterfaceHash;
        Prototype.MaterialBindingHash = Current.MaterialBindingHash;
        ReadyMeshInterfaces.Add(Key, MoveTemp(Current));
        MHRecordDefinitionEndpointStore();
    }
    Prototype.State = EMHEndpointState::Ready;
}
