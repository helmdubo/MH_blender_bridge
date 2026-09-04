#include "Composite/MHEndpointPrototypeRegistry.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
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

void UMHEndpointPrototypeRegistry::Invalidate(const FMHResourceKey& Key)
{
    if (FMHEndpointPrototype* Prototype = Prototypes.Find(Key))
    {
        ++Prototype->Revision;
        Prototype->State = EMHEndpointState::Unresolved;
        Prototype->Object.Reset();
        Prototype->AdmissionError.Reset();
    }
}

void UMHEndpointPrototypeRegistry::InvalidateAll()
{
    for (TPair<FMHResourceKey, FMHEndpointPrototype>& Pair : Prototypes)
    {
        ++Pair.Value.Revision;
        Pair.Value.State = EMHEndpointState::Unresolved;
        Pair.Value.Object.Reset();
        Pair.Value.AdmissionError.Reset();
    }
}

FMHEndpointInterfaceDelta UMHEndpointPrototypeRegistry::GetLastInterfaceDelta(const FMHResourceKey& Key) const
{
    // R3a red stub: the executor computes the five hashes/revisions in Admit and
    // the delta against the previous Ready admission.
    static_cast<void>(Key);
    return FMHEndpointInterfaceDelta();
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
    Prototype.Object.Reset();
    Prototype.State = EMHEndpointState::Invalid;
    Prototype.AdmissionError.Reset();
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
    Prototype.State = EMHEndpointState::Ready;
    if (Key.Kind == EMHResourceKind::StaticMesh) MHRecordDefinitionEndpointStore();
}
