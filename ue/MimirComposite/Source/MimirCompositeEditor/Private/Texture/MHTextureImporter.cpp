#include "Texture/MHTextureImporter.h"

#include "AssetCompilingManager.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImportBatch.h"
#include "Source/MHSourceImportMetrics.h"
#include "Texture/MHTextureSourceData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHTextureImport, Display, All);

namespace UE::MimirComposite
{

bool MHTextureIsManagedNormalMapLogicalName(const FString& LogicalName)
{
    return LogicalName == TEXT("tex_n") ||
        LogicalName.EndsWith(TEXT("_tex_n"), ESearchCase::CaseSensitive);
}

namespace
{

constexpr const TCHAR* GeneratedTextureRoot = TEXT("/Game/MH/Generated/Textures");

bool MHTextureHasManagedSettings(const UTexture& Texture, const FString& LogicalName)
{
    return !MHTextureIsManagedNormalMapLogicalName(LogicalName) ||
        (!Texture.SRGB && Texture.CompressionSettings == TC_BC7);
}

void MHTextureApplyManagedSettings(UTexture& Texture, const FString& LogicalName)
{
    if (!MHTextureIsManagedNormalMapLogicalName(LogicalName))
    {
        return;
    }
    Texture.SRGB = false;
    Texture.CompressionSettings = TC_BC7;
    Texture.PostEditChange();
}

bool TextureRelativeToRoot(
    const FString& Root,
    const FString& Path,
    FString& OutRelative)
{
    FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(FullRoot);
    FString FullPath = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(FullPath);
    if (!FPaths::IsUnderDirectory(FullPath, FullRoot))
    {
        return false;
    }
    FullRoot += TEXT("/");
    OutRelative = FullPath;
    return FPaths::MakePathRelativeTo(OutRelative, *FullRoot) &&
        FPaths::IsRelative(OutRelative) &&
        !OutRelative.StartsWith(TEXT("../"));
}

UMHTextureSourceData* TextureSourceData(UTexture& Texture)
{
    return Cast<UMHTextureSourceData>(
        Texture.GetAssetUserDataOfClass(UMHTextureSourceData::StaticClass()));
}

bool HasExactTextureReceipt(
    UTexture& Texture,
    const FMHSourceAnalysisEntry& Entry,
    const FString& RelativePath)
{
    const UMHTextureSourceData* Data = TextureSourceData(Texture);
    return Data != nullptr &&
        Data->LogicalName == Entry.Key.LogicalName &&
        Data->SourceRelativePath == RelativePath &&
        Data->SourceHash == Entry.RawHash &&
        MHTextureHasManagedSettings(Texture, Entry.Key.LogicalName);
}

bool SaveTexturePackage(
    UTexture& Texture,
    const FString& PackageName,
    FString& OutError)
{
    if (MHDeferSourceImportPersistence(Texture))
    {
        return true;
    }
    FMHSourceImportMetricScope MetricScope(
        EMHSourceImportMetricResource::Texture,
        EMHSourceImportMetricStage::SavePackage);
    UPackage* Package = Texture.GetOutermost();
    Package->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName,
        FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Package, &Texture, *Filename, Args))
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: imported texture:%s could not be persisted"),
            *Texture.GetName());
        return false;
    }
    if (Package->HasAnyPackageFlags(PKG_InMemoryOnly))
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: persisted texture:%s remained in-memory-only"),
            *Texture.GetName());
        return false;
    }
    return true;
}

struct FPreparedTextureImport
{
    const FMHSourceAnalysisEntry* Entry = nullptr;
    FMHTextureOperationResult Result;
    FString SourceRelativePath;
    FString PackageName;
    FString ObjectPath;
    TArray<uint8> PreImportSourceBytes;
    UAssetImportTask* Task = nullptr;
};

FPreparedTextureImport PrepareTextureImport(
    const FMHSourceAnalysisEntry& Entry,
    const FString& SourceRoot,
    const bool bForceReimport,
    const bool bAsync)
{
    FMHSourceImportMetricScope CreateScope(
        EMHSourceImportMetricResource::Texture,
        EMHSourceImportMetricStage::Create);
    FPreparedTextureImport Prepared;
    Prepared.Entry = &Entry;
    if (Entry.Key.Kind != EMHResourceKind::Texture ||
        !Entry.Key.IsCanonical() ||
        Entry.PayloadPath.IsEmpty() ||
        Entry.RawHash.IsEmpty())
    {
        Prepared.Result.Error =
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: import entry is not a resolved texture");
        return Prepared;
    }

    if (!TextureRelativeToRoot(SourceRoot, Entry.PayloadPath, Prepared.SourceRelativePath))
    {
        Prepared.Result.Error = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture:%s resolved outside source_root"),
            *Entry.Key.LogicalName);
        return Prepared;
    }

    Prepared.PackageName = FString(GeneratedTextureRoot) + TEXT("/") + Entry.Key.LogicalName;
    Prepared.ObjectPath = Prepared.PackageName + TEXT(".") + Entry.Key.LogicalName;
    UObject* ExistingObject = StaticLoadObject(
        UObject::StaticClass(),
        nullptr,
        *Prepared.ObjectPath);
    UTexture* ExistingTexture = Cast<UTexture>(ExistingObject);
    if (ExistingObject != nullptr && ExistingTexture == nullptr)
    {
        Prepared.Result.Error = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: generated texture path is occupied by %s"),
            *ExistingObject->GetClass()->GetName());
        return Prepared;
    }
    if (!bForceReimport &&
        ExistingTexture != nullptr &&
        HasExactTextureReceipt(*ExistingTexture, Entry, Prepared.SourceRelativePath))
    {
        Prepared.Result.Texture = ExistingTexture;
        return Prepared;
    }

    if (!FFileHelper::LoadFileToArray(Prepared.PreImportSourceBytes, *Entry.PayloadPath) ||
        MHRawPayloadHash(Prepared.PreImportSourceBytes) != Entry.RawHash)
    {
        Prepared.Result.Error = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: texture:%s changed before import task"),
            *Entry.Key.LogicalName);
        return Prepared;
    }

    Prepared.Task = NewObject<UAssetImportTask>();
    Prepared.Task->Filename = Entry.PayloadPath;
    Prepared.Task->DestinationPath = GeneratedTextureRoot;
    Prepared.Task->DestinationName = Entry.Key.LogicalName;
    Prepared.Task->bAutomated = true;
    Prepared.Task->bReplaceExisting = true;
    Prepared.Task->bReplaceExistingSettings = false;
    Prepared.Task->bSave = false;
    Prepared.Task->bAsync = bAsync;
    return Prepared;
}

FMHTextureOperationResult FinalizeTextureImport(
    FPreparedTextureImport& Prepared,
    const FString& SourceRoot,
    const bool bBatch)
{
    const bool bDeferred = bBatch || MHIsSourceImportBatchActive();
    if (Prepared.Task == nullptr)
    {
        return MoveTemp(Prepared.Result);
    }
    check(Prepared.Entry != nullptr);
    const FMHSourceAnalysisEntry& Entry = *Prepared.Entry;

    // GetObjects is the explicit completion boundary for an async import task.
    // Revalidate source bytes only after that boundary so a mid-task edit can
    // never be admitted under the scan-time receipt.
    const TArray<UObject*>& ImportedObjects = Prepared.Task->GetObjects();
    TArray<uint8> PostImportSourceBytes;
    if (!FFileHelper::LoadFileToArray(PostImportSourceBytes, *Entry.PayloadPath) ||
        PostImportSourceBytes != Prepared.PreImportSourceBytes ||
        MHRawPayloadHash(PostImportSourceBytes) != Entry.RawHash)
    {
        Prepared.Result.Error = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: texture:%s changed during import task"),
            *Entry.Key.LogicalName);
        return MoveTemp(Prepared.Result);
    }

    UTexture* Texture = nullptr;
    for (UObject* Imported : ImportedObjects)
    {
        UTexture* Candidate = Cast<UTexture>(Imported);
        if (Candidate == nullptr ||
            !Candidate->GetPathName().Equals(Prepared.ObjectPath, ESearchCase::CaseSensitive))
        {
            continue;
        }
        if (Texture != nullptr && Texture != Candidate)
        {
            Prepared.Result.Error = FString::Printf(
                TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: current import returned duplicate exact objects for texture:%s"),
                *Entry.Key.LogicalName);
            return MoveTemp(Prepared.Result);
        }
        Texture = Candidate;
    }
    if (Texture == nullptr)
    {
        Prepared.Result.Error = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture:%s resolved to source but UE import failed: %s"),
            *Entry.Key.LogicalName,
            *Entry.PayloadPath);
        return MoveTemp(Prepared.Result);
    }

#if WITH_EDITORONLY_DATA
    const UAssetImportData* ImportData = Texture->AssetImportData;
    const FMD5Hash CurrentSourceHash = FMD5Hash::HashFile(*Entry.PayloadPath);
    if (ImportData == nullptr ||
        ImportData->GetSourceFileCount() != 1 ||
        !FPaths::IsSamePath(ImportData->GetFirstFilename(), Entry.PayloadPath) ||
        !CurrentSourceHash.IsValid() ||
        ImportData->GetSourceData().SourceFiles[0].FileHash != CurrentSourceHash)
    {
        Prepared.Result.Error = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: current import did not record exact source bytes for texture:%s"),
            *Entry.Key.LogicalName);
        return MoveTemp(Prepared.Result);
    }
#else
    Prepared.Result.Error =
        TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture import receipts require editor-only data");
    return MoveTemp(Prepared.Result);
#endif

    MHTextureApplyManagedSettings(*Texture, Entry.Key.LogicalName);
    if (!bDeferred)
    {
        FMHSourceImportMetricScope WaitScope(
            EMHSourceImportMetricResource::Texture,
            EMHSourceImportMetricStage::BuildWait);
        FAssetCompilingManager::Get().FinishAllCompilation();
    }
    if (!SaveTexturePackage(*Texture, Prepared.PackageName, Prepared.Result.Error))
    {
        return MoveTemp(Prepared.Result);
    }

    UMHTextureSourceData* SourceData = TextureSourceData(*Texture);
    if (SourceData == nullptr)
    {
        SourceData = NewObject<UMHTextureSourceData>(Texture, NAME_None, RF_Transactional);
        Texture->AddAssetUserData(SourceData);
    }
    SourceData->LogicalName = Entry.Key.LogicalName;
    SourceData->SourceRelativePath = Prepared.SourceRelativePath;
    SourceData->SourceHash = Entry.RawHash;
    Texture->PostEditChange();
    if (bDeferred)
    {
        MHQueueSourceImportCompilation();
    }
    if (!SaveTexturePackage(*Texture, Prepared.PackageName, Prepared.Result.Error))
    {
        return MoveTemp(Prepared.Result);
    }

    FString RebindEvent;
    if (MHConsumeOrphanRebindEvent(SourceRoot, Entry.Key, RebindEvent))
    {
        Prepared.Result.Warnings.Add(RebindEvent);
        UE_LOG(LogMHTextureImport, Warning, TEXT("%s"), *RebindEvent);
    }
    if (bDeferred)
    {
        MHQueueSourceImportSourceGuard(Entry.Key, Entry.PayloadPath, Entry.RawHash);
        MHQueueSourceImportPackage(*Texture, Entry.Key);
        MHQueueSourceImportCompletion(Entry.Key, false);
    }
    else if (!MHRefreshGeneratedAssetProjection(SourceRoot, Prepared.Result.Error))
    {
        return MoveTemp(Prepared.Result);
    }
    Prepared.Result.Texture = Texture;
    Prepared.Result.bImported = true;
    return MoveTemp(Prepared.Result);
}

} // namespace

FMHTextureOperationResult MHEnsureTextureV4(
    const FMHSourceAnalysisEntry& Entry,
    const FString& SourceRoot,
    const bool bForceReimport)
{
    FPreparedTextureImport Prepared =
        PrepareTextureImport(Entry, SourceRoot, bForceReimport, false);
    if (Prepared.Task == nullptr)
    {
        return MoveTemp(Prepared.Result);
    }
    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    TArray<UAssetImportTask*> Tasks = {Prepared.Task};
    AssetToolsModule.Get().ImportAssetTasks(Tasks);
    return FinalizeTextureImport(Prepared, SourceRoot, false);
}

void MHEnsureTextureBatchV4(
    const TConstArrayView<FMHTextureBulkImportRequest> Requests,
    const FString& SourceRoot,
    TArray<FMHTextureOperationResult>& OutResults)
{
    check(MHIsSourceImportBatchActive());
    TArray<FPreparedTextureImport> Prepared;
    Prepared.Reserve(Requests.Num());
    TArray<UAssetImportTask*> Tasks;
    for (const FMHTextureBulkImportRequest& Request : Requests)
    {
        if (Request.Entry == nullptr)
        {
            FPreparedTextureImport& Invalid = Prepared.AddDefaulted_GetRef();
            Invalid.Result.Error =
                TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: bulk request has no entry");
            continue;
        }
        FPreparedTextureImport& Item = Prepared.Add_GetRef(PrepareTextureImport(
            *Request.Entry,
            SourceRoot,
            Request.bForceReimport,
            true));
        if (Item.Task != nullptr)
        {
            Tasks.Add(Item.Task);
        }
    }
    {
        FMHSourceImportMetricScope BatchCreateScope(
            EMHSourceImportMetricResource::Batch,
            EMHSourceImportMetricStage::Create);
        if (!Tasks.IsEmpty())
        {
            FAssetToolsModule& AssetToolsModule =
                FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
            AssetToolsModule.Get().ImportAssetTasks(Tasks);
        }
        OutResults.Reset(Prepared.Num());
        for (FPreparedTextureImport& Item : Prepared)
        {
            OutResults.Add(FinalizeTextureImport(Item, SourceRoot, true));
        }
    }
}

} // namespace UE::MimirComposite
