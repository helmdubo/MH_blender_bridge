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

} // namespace

FMHTextureOperationResult MHEnsureTextureV4(
    const FMHSourceAnalysisEntry& Entry,
    const FString& SourceRoot,
    const bool bForceReimport)
{
    FMHTextureOperationResult Result;
    if (Entry.Key.Kind != EMHResourceKind::Texture ||
        !Entry.Key.IsCanonical() ||
        Entry.PayloadPath.IsEmpty() ||
        Entry.RawHash.IsEmpty())
    {
        Result.Error = TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: import entry is not a resolved texture");
        return Result;
    }

    FString SourceRelativePath;
    if (!TextureRelativeToRoot(SourceRoot, Entry.PayloadPath, SourceRelativePath))
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture:%s resolved outside source_root"),
            *Entry.Key.LogicalName);
        return Result;
    }

    const FString PackageName = FString(GeneratedTextureRoot) + TEXT("/") + Entry.Key.LogicalName;
    const FString ObjectPath = PackageName + TEXT(".") + Entry.Key.LogicalName;
    UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
    UTexture* ExistingTexture = Cast<UTexture>(ExistingObject);
    if (ExistingObject != nullptr && ExistingTexture == nullptr)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: generated texture path is occupied by %s"),
            *ExistingObject->GetClass()->GetName());
        return Result;
    }
    if (!bForceReimport &&
        ExistingTexture != nullptr &&
        HasExactTextureReceipt(*ExistingTexture, Entry, SourceRelativePath))
    {
        Result.Texture = ExistingTexture;
        return Result;
    }

    TArray<uint8> PreImportSourceBytes;
    if (!FFileHelper::LoadFileToArray(PreImportSourceBytes, *Entry.PayloadPath) ||
        MHRawPayloadHash(PreImportSourceBytes) != Entry.RawHash)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: texture:%s changed before import task"),
            *Entry.Key.LogicalName);
        return Result;
    }

    UAssetImportTask* Task = NewObject<UAssetImportTask>();
    Task->Filename = Entry.PayloadPath;
    Task->DestinationPath = GeneratedTextureRoot;
    Task->DestinationName = Entry.Key.LogicalName;
    Task->bAutomated = true;
    Task->bReplaceExisting = true;
    Task->bReplaceExistingSettings = false;
    Task->bSave = false;
    Task->bAsync = false;

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    TArray<UAssetImportTask*> Tasks = {Task};
    AssetToolsModule.Get().ImportAssetTasks(Tasks);

    TArray<uint8> PostImportSourceBytes;
    if (!FFileHelper::LoadFileToArray(PostImportSourceBytes, *Entry.PayloadPath) ||
        PostImportSourceBytes != PreImportSourceBytes ||
        MHRawPayloadHash(PostImportSourceBytes) != Entry.RawHash)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: texture:%s changed during import task"),
            *Entry.Key.LogicalName);
        return Result;
    }

    UTexture* Texture = nullptr;
    for (UObject* Imported : Task->GetObjects())
    {
        UTexture* Candidate = Cast<UTexture>(Imported);
        if (Candidate == nullptr ||
            !Candidate->GetPathName().Equals(ObjectPath, ESearchCase::CaseSensitive))
        {
            continue;
        }
        if (Texture != nullptr && Texture != Candidate)
        {
            Result.Error = FString::Printf(
                TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: current import returned duplicate exact objects for texture:%s"),
                *Entry.Key.LogicalName);
            return Result;
        }
        Texture = Candidate;
    }
    if (Texture == nullptr)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture:%s resolved to source but UE import failed: %s"),
            *Entry.Key.LogicalName,
            *Entry.PayloadPath);
        return Result;
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
        Result.Error = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: current import did not record exact source bytes for texture:%s"),
            *Entry.Key.LogicalName);
        return Result;
    }
#else
    Result.Error = TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture import receipts require editor-only data");
    return Result;
#endif

    MHTextureApplyManagedSettings(*Texture, Entry.Key.LogicalName);
    FAssetCompilingManager::Get().FinishAllCompilation();
    if (!SaveTexturePackage(*Texture, PackageName, Result.Error))
    {
        return Result;
    }

    UMHTextureSourceData* SourceData = TextureSourceData(*Texture);
    if (SourceData == nullptr)
    {
        SourceData = NewObject<UMHTextureSourceData>(Texture, NAME_None, RF_Transactional);
        Texture->AddAssetUserData(SourceData);
    }
    SourceData->LogicalName = Entry.Key.LogicalName;
    SourceData->SourceRelativePath = SourceRelativePath;
    SourceData->SourceHash = Entry.RawHash;
    Texture->PostEditChange();
    if (!SaveTexturePackage(*Texture, PackageName, Result.Error))
    {
        return Result;
    }

    FString RebindEvent;
    if (MHConsumeOrphanRebindEvent(SourceRoot, Entry.Key, RebindEvent))
    {
        Result.Warnings.Add(RebindEvent);
        UE_LOG(LogMHTextureImport, Warning, TEXT("%s"), *RebindEvent);
    }
    if (!MHRefreshGeneratedAssetProjection(SourceRoot, Result.Error))
    {
        return Result;
    }
    Result.Texture = Texture;
    Result.bImported = true;
    return Result;
}

} // namespace UE::MimirComposite
