#include "Source/MHSourceComposition.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Modules/ModuleManager.h"

namespace UE::MimirComposite
{
namespace
{

TSharedPtr<FMHProjectResourceIndex> GProjectIndex;

FString NormalizedRoot(const FString& SourceRoot)
{
    FString Result = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(Result);
    return Result;
}

void ReadTag(const FAssetData& Asset, const TCHAR* Name, FString& OutValue)
{
    OutValue.Reset();
    Asset.GetTagValue(FName(Name), OutValue);
}

void SetCarrierKind(
    const FAssetData& Asset,
    const TSet<FTopLevelAssetPath>& TextureClassPaths,
    FMHGeneratedAssetTagClaim& OutClaim)
{
    if (Asset.AssetClassPath == UMaterialInstanceConstant::StaticClass()->GetClassPathName())
    {
        OutClaim.bHasCarrierKind = true;
        OutClaim.CarrierKind = EMHResourceKind::Material;
    }
    else if (Asset.AssetClassPath == UMHCompositeAsset::StaticClass()->GetClassPathName())
    {
        OutClaim.bHasCarrierKind = true;
        OutClaim.CarrierKind = EMHResourceKind::Composite;
    }
    else if (Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName())
    {
        OutClaim.bHasCarrierKind = true;
        OutClaim.CarrierKind = EMHResourceKind::StaticMesh;
    }
    else if (TextureClassPaths.Contains(Asset.AssetClassPath))
    {
        OutClaim.bHasCarrierKind = true;
        OutClaim.CarrierKind = EMHResourceKind::Texture;
    }
}

void GatherGeneratedAssetClaims(
    TArray<FMHGeneratedAssetTagClaim>& OutClaims)
{
    OutClaims.Reset();
    IAssetRegistry& Registry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    const TArray<FName> ManagedTagNames = {
        FName(TEXT("MH.Kind")),
        FName(TEXT("MH.LogicalName")),
        FName(TEXT("MH.SourcePath")),
        FName(TEXT("MH.SourceHash")),
        FName(TEXT("MH.AppliedHash")),
        FName(TEXT("MH.Managed"))};
    TMap<FString, FAssetData> AssetsByObjectPath;
    for (const FName Tag : ManagedTagNames)
    {
        TArray<FAssetData> TaggedAssets;
        Registry.GetAssetsByTags({Tag}, TaggedAssets);
        for (const FAssetData& Asset : TaggedAssets)
        {
            AssetsByObjectPath.Add(Asset.GetSoftObjectPath().ToString(), Asset);
        }
    }

    const TArray<FTopLevelAssetPath> TextureRootClasses = {
        UTexture::StaticClass()->GetClassPathName()};
    const TSet<FTopLevelAssetPath> ExcludedTextureClasses;
    TSet<FTopLevelAssetPath> TextureClassPaths;
    Registry.GetDerivedClassNames(
        TextureRootClasses,
        ExcludedTextureClasses,
        TextureClassPaths);
    TextureClassPaths.Add(UTexture::StaticClass()->GetClassPathName());

    TArray<FString> ObjectPaths;
    AssetsByObjectPath.GetKeys(ObjectPaths);
    ObjectPaths.Sort();

    OutClaims.Reserve(ObjectPaths.Num());
    for (const FString& ObjectPath : ObjectPaths)
    {
        const FAssetData& Asset = AssetsByObjectPath.FindChecked(ObjectPath);
        FMHGeneratedAssetTagClaim& Claim = OutClaims.AddDefaulted_GetRef();
        Claim.UEObjectPath = ObjectPath;
        ReadTag(Asset, TEXT("MH.Kind"), Claim.Kind);
        ReadTag(Asset, TEXT("MH.LogicalName"), Claim.LogicalName);
        ReadTag(Asset, TEXT("MH.SourcePath"), Claim.SourcePath);
        ReadTag(Asset, TEXT("MH.SourceHash"), Claim.SourceHash);
        ReadTag(Asset, TEXT("MH.AppliedHash"), Claim.AppliedHash);
        ReadTag(Asset, TEXT("MH.Managed"), Claim.Managed);
        for (const TPair<FName, FAssetTagValueRef>& Tag : Asset.TagsAndValues)
        {
            Claim.MHTagCount += Tag.Key.ToString().StartsWith(TEXT("MH.")) ? 1 : 0;
        }
        SetCarrierKind(Asset, TextureClassPaths, Claim);
    }
}

bool AcquireProjectIndex(
    const FString& SourceRoot,
    TSharedPtr<FMHProjectResourceIndex>& OutIndex,
    bool& bOutRecreated,
    FString& OutError)
{
    OutIndex.Reset();
    OutError.Reset();
    bOutRecreated = false;
    const FString Root = NormalizedRoot(SourceRoot);
    if (Root.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is empty");
        return false;
    }

    if (!GProjectIndex.IsValid() ||
        !FPaths::IsSamePath(GProjectIndex->GetSourceRoot(), Root))
    {
        if (GProjectIndex.IsValid())
        {
            GProjectIndex->Close();
        }
        GProjectIndex = MakeShared<FMHProjectResourceIndex>(Root);
    }
    if (!GProjectIndex->IsOpen() && !GProjectIndex->Open(bOutRecreated, OutError))
    {
        GProjectIndex.Reset();
        return false;
    }
    OutIndex = GProjectIndex;
    return true;
}

void BuildAnalysisServices(
    const TSharedPtr<FMHProjectResourceIndex>& Index,
    FMHSourceAnalysisServices& OutServices)
{
    OutServices.Index = Index;
    OutServices.Resolver = MakeUnique<FMHProjectIndexResolver>(*Index);
    OutServices.ChangeDetector = MakeUnique<FMHProjectIndexChangeDetector>(*Index);
}

} // namespace

bool MHCreateDefaultSourceAnalysisServices(
    const FString& SourceRoot,
    FMHSourceAnalysisServices& OutServices,
    FString& OutError)
{
    OutServices = FMHSourceAnalysisServices();
    OutError.Reset();

    bool bRecreated = false;
    TSharedPtr<FMHProjectResourceIndex> Index;
    if (!AcquireProjectIndex(SourceRoot, Index, bRecreated, OutError))
    {
        return false;
    }

    TArray<FMHGeneratedAssetTagClaim> Claims;
    GatherGeneratedAssetClaims(Claims);
    FMHProjectIndexUpdateResult Update;
    if (!Index->FullScan(Claims, Update, OutError))
    {
        return false;
    }

    BuildAnalysisServices(Index, OutServices);
    return true;
}

bool MHCreateIncrementalSourceAnalysisServices(
    const FString& SourceRoot,
    const TArray<FString>& Paths,
    FMHSourceAnalysisServices& OutServices,
    FMHProjectIndexUpdateResult& OutUpdate,
    bool& bOutUsedFullScan,
    FString& OutError)
{
    OutServices = FMHSourceAnalysisServices();
    OutUpdate = FMHProjectIndexUpdateResult();
    OutError.Reset();
    bOutUsedFullScan = false;

    bool bRecreated = false;
    TSharedPtr<FMHProjectResourceIndex> Index;
    if (!AcquireProjectIndex(SourceRoot, Index, bRecreated, OutError))
    {
        return false;
    }

    if (bRecreated)
    {
        TArray<FMHGeneratedAssetTagClaim> Claims;
        GatherGeneratedAssetClaims(Claims);
        if (!Index->FullScan(Claims, OutUpdate, OutError))
        {
            return false;
        }
        bOutUsedFullScan = true;
    }
    else if (!Index->UpsertPaths(Paths, OutUpdate, OutError))
    {
        return false;
    }

    BuildAnalysisServices(Index, OutServices);
    return true;
}

bool MHUpsertPublishedSource(
    const FString& SourceRoot,
    const FString& PublishedPath,
    const FString& RawHash,
    TArray<FString>& OutSessionEvents,
    FString& OutError)
{
    OutSessionEvents.Reset();
    OutError.Reset();

    bool bRecreated = false;
    TSharedPtr<FMHProjectResourceIndex> Index;
    if (!AcquireProjectIndex(SourceRoot, Index, bRecreated, OutError))
    {
        return false;
    }
    if (bRecreated)
    {
        TArray<FMHGeneratedAssetTagClaim> Claims;
        GatherGeneratedAssetClaims(Claims);
        FMHProjectIndexUpdateResult Rebuild;
        if (!Index->FullScan(Claims, Rebuild, OutError))
        {
            return false;
        }
    }

    if (!Index->RegisterSelfPublishAfterReplace(PublishedPath, RawHash, OutError))
    {
        return false;
    }
    FMHProjectIndexUpdateResult Update;
    if (!Index->UpsertPaths({PublishedPath}, Update, OutError))
    {
        return false;
    }
    OutSessionEvents = MoveTemp(Update.SessionEvents);
    return true;
}

bool MHRefreshGeneratedAssetProjection(
    const FString& SourceRoot,
    FString& OutError)
{
    OutError.Reset();
    bool bRecreated = false;
    TSharedPtr<FMHProjectResourceIndex> Index;
    if (!AcquireProjectIndex(SourceRoot, Index, bRecreated, OutError))
    {
        return false;
    }
    if (bRecreated)
    {
        TArray<FMHGeneratedAssetTagClaim> Claims;
        GatherGeneratedAssetClaims(Claims);
        FMHProjectIndexUpdateResult Rebuild;
        return Index->FullScan(Claims, Rebuild, OutError);
    }

    TArray<FMHGeneratedAssetTagClaim> Claims;
    GatherGeneratedAssetClaims(Claims);
    FMHProjectIndexUpdateResult Update;
    return Index->ReplaceGeneratedAssets(Claims, Update, OutError);
}

bool MHConsumeOrphanRebindEvent(
    const FString& SourceRoot,
    const FMHResourceKey& Key,
    FString& OutEvent)
{
    OutEvent.Reset();
    const FString Root = NormalizedRoot(SourceRoot);
    if (!GProjectIndex.IsValid() || !GProjectIndex->IsOpen() ||
        !FPaths::IsSamePath(GProjectIndex->GetSourceRoot(), Root))
    {
        return false;
    }
    return GProjectIndex->ConsumeOrphanRebindEvent(Key, OutEvent);
}

#if WITH_DEV_AUTOMATION_TESTS
void MHGatherGeneratedAssetClaimsForTests(
    TArray<FMHGeneratedAssetTagClaim>& OutClaims)
{
    GatherGeneratedAssetClaims(OutClaims);
}
#endif

void MHShutdownProjectIndex()
{
    if (GProjectIndex.IsValid())
    {
        GProjectIndex->Close();
        GProjectIndex.Reset();
    }
}

} // namespace UE::MimirComposite
