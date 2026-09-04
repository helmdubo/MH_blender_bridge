#include "Material/MHMaterialDocumentExport.h"

#include "Algo/Unique.h"
#include "HAL/FileManager.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialDonorTransfer.h"
#include "Material/MHMaterialProtocol.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHMaterialDocumentExport, Log, All);

namespace UE::MimirComposite
{
namespace
{

struct FNormalizedExportRequest
{
    UMaterialInstanceConstant* Material = nullptr;
    FString DestinationPath;
    FString ValidationError;
    bool bDuplicateDestination = false;
};

const UMHMaterialSourceData* GetMaterialReceipt(const UMaterialInstanceConstant& Material)
{
    return Cast<UMHMaterialSourceData>(
        const_cast<UMaterialInstanceConstant&>(Material).GetAssetUserDataOfClass(
            UMHMaterialSourceData::StaticClass()));
}

FString NormalizeFilePath(const FString& Path)
{
    FString Result = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Result);
    return Result;
}

bool IsInsideSourceRoot(const FString& DestinationPath, const FString& SourceRoot)
{
    if (SourceRoot.IsEmpty())
    {
        return false;
    }
    FString FullRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(FullRoot);
    return FPaths::IsSamePath(DestinationPath, FullRoot) ||
        FPaths::IsUnderDirectory(DestinationPath, FullRoot);
}

FMHMaterialDocumentExportFailure MakeFailure(
    const UMaterialInstanceConstant* Material,
    const FString& DestinationPath,
    const FString& Error)
{
    FMHMaterialDocumentExportFailure Result;
    Result.MaterialPath = Material != nullptr ? Material->GetPathName() : TEXT("<null>");
    Result.DestinationPath = DestinationPath;
    Result.Error = Error;
    return Result;
}

bool AtomicWriteCanonicalMaterial(
    const FString& TargetPath,
    const TArray<uint8>& Bytes,
    FString& OutError)
{
    const FString Folder = FPaths::GetPath(TargetPath);
    if (Folder.IsEmpty() || !IFileManager::Get().MakeDirectory(*Folder, true))
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: cannot create material document export folder: %s"),
            *Folder);
        return false;
    }

    const FString TempPath = TargetPath + FString::Printf(
        TEXT(".tmp.%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: cannot write temporary material document: %s"),
            *TempPath);
        return false;
    }

    TArray<uint8> ReadBack;
    FMHMaterialDocument Parsed;
    TArray<uint8> CanonicalReadBack;
    FString ValidationError;
    if (!FFileHelper::LoadFileToArray(ReadBack, *TempPath) ||
        ReadBack != Bytes ||
        !MHParseMaterialV4(ReadBack, Parsed, ValidationError) ||
        !MHWriteCanonicalMaterialV4(Parsed, CanonicalReadBack, ValidationError) ||
        CanonicalReadBack != Bytes)
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: material document export read-back failed for %s: %s"),
            *TargetPath,
            *ValidationError);
        return false;
    }

    if (!IFileManager::Get().Move(*TargetPath, *TempPath, true, true, false, true))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: atomic material document export failed: %s"),
            *TargetPath);
        return false;
    }
    return true;
}

} // namespace

FString MHGetMaterialDocumentExportLogicalName(const UMaterialInstanceConstant& Material)
{
    if (const UMHMaterialSourceData* Receipt = GetMaterialReceipt(Material);
        Receipt != nullptr && !Receipt->LogicalName.IsEmpty())
    {
        return Receipt->LogicalName;
    }
    return Material.GetName();
}

bool MHPrepareMaterialDocumentExport(
    TConstArrayView<FMHMaterialDocumentExportRequest> Requests,
    const UMHCompositeSettings& Settings,
    const FString& SourceRoot,
    FMHMaterialDocumentExportPlan& OutPlan,
    FString& OutError)
{
    OutPlan = FMHMaterialDocumentExportPlan();
    OutError.Reset();
    if (Requests.IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: material document export selection is empty");
        return false;
    }

    TArray<FNormalizedExportRequest> Normalized;
    Normalized.Reserve(Requests.Num());
    for (const FMHMaterialDocumentExportRequest& Request : Requests)
    {
        FNormalizedExportRequest& Item = Normalized.AddDefaulted_GetRef();
        Item.Material = Request.Material;
        if (Request.DestinationPath.IsEmpty())
        {
            Item.ValidationError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: material document export path is empty");
            continue;
        }
        Item.DestinationPath = NormalizeFilePath(Request.DestinationPath);
        if (!FPaths::GetExtension(Item.DestinationPath, true).Equals(
                TEXT(".material"),
                ESearchCase::CaseSensitive))
        {
            Item.ValidationError = FString::Printf(
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: material document export requires the exact .material suffix: %s"),
                *Item.DestinationPath);
            continue;
        }
        if (IsInsideSourceRoot(Item.DestinationPath, SourceRoot))
        {
            // Owner decision (2026-09-03): a material may be exported into any
            // material source, Source Root included, in every situation. The
            // written document is an ordinary external edit for the source
            // index; 'Publish Material to MH Source' stays the receipt-bound path.
            UE_LOG(LogMHMaterialDocumentExport, Display,
                TEXT("material document export writes inside Source Root: %s"), *Item.DestinationPath);
        }
    }

    for (int32 Left = 0; Left < Normalized.Num(); ++Left)
    {
        if (Normalized[Left].DestinationPath.IsEmpty())
        {
            continue;
        }
        for (int32 Right = Left + 1; Right < Normalized.Num(); ++Right)
        {
            if (!Normalized[Right].DestinationPath.IsEmpty() &&
                FPaths::IsSamePath(
                    Normalized[Left].DestinationPath,
                    Normalized[Right].DestinationPath))
            {
                Normalized[Left].bDuplicateDestination = true;
                Normalized[Right].bDuplicateDestination = true;
            }
        }
    }

    for (const FNormalizedExportRequest& Request : Normalized)
    {
        if (Request.Material == nullptr)
        {
            OutPlan.Skipped.Add(MakeFailure(
                nullptr,
                Request.DestinationPath,
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: material document export contains a null asset")));
            continue;
        }
        if (!Request.ValidationError.IsEmpty())
        {
            OutPlan.Skipped.Add(MakeFailure(
                Request.Material,
                Request.DestinationPath,
                Request.ValidationError));
            continue;
        }
        if (Request.bDuplicateDestination)
        {
            OutPlan.Skipped.Add(MakeFailure(
                Request.Material,
                Request.DestinationPath,
                FString::Printf(
                    TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: multiple selected materials target the same document: %s"),
                    *Request.DestinationPath)));
            continue;
        }

        FMHMaterialDocument Document;
        FString ItemError;
        TArray<FString> ItemWarnings;
        if (!MHExtractMaterialV4(*Request.Material, Settings, Document, ItemError, &ItemWarnings))
        {
            OutPlan.Skipped.Add(MakeFailure(
                Request.Material,
                Request.DestinationPath,
                ItemError));
            continue;
        }

        for (const FString& Warning : ItemWarnings)
        {
            OutPlan.Warnings.Add(FString::Printf(TEXT("%s -> %s: %s"),
                *Request.Material->GetPathName(), *Request.DestinationPath, *Warning));
        }
        FMHPreparedMaterialDocumentExport& Prepared = OutPlan.Ready.AddDefaulted_GetRef();
        Prepared.Material = Request.Material;
        Prepared.LogicalName = MHGetMaterialDocumentExportLogicalName(*Request.Material);
        Prepared.DestinationPath = Request.DestinationPath;
        if (!MHWriteCanonicalMaterialV4(Document, Prepared.CanonicalBytes, ItemError))
        {
            OutPlan.Ready.Pop();
            OutPlan.Skipped.Add(MakeFailure(
                Request.Material,
                Request.DestinationPath,
                ItemError));
            continue;
        }
        Prepared.CanonicalHash = MHRawPayloadHash(Prepared.CanonicalBytes);
        if (const UMHMaterialSourceData* Receipt = GetMaterialReceipt(*Request.Material);
            Receipt != nullptr && !Receipt->AppliedHash.IsEmpty())
        {
            Prepared.bMatchesAppliedHash = Prepared.CanonicalHash.Equals(
                Receipt->AppliedHash,
                ESearchCase::CaseSensitive);
        }
        Prepared.bOverwritesExistingFile = IFileManager::Get().FileExists(
            *Prepared.DestinationPath);
        if (Prepared.bOverwritesExistingFile)
        {
            OutPlan.OverwritePaths.Add(Prepared.DestinationPath);
        }
    }

    OutPlan.OverwritePaths.Sort();
    OutPlan.OverwritePaths.SetNum(Algo::Unique(OutPlan.OverwritePaths));
    return true;
}

bool MHCommitMaterialDocumentExport(
    const FMHMaterialDocumentExportPlan& Plan,
    const bool bAllowOverwrite,
    FMHMaterialDocumentExportResult& OutResult,
    FString& OutError)
{
    OutResult = FMHMaterialDocumentExportResult();
    OutError.Reset();
    if (!Plan.OverwritePaths.IsEmpty() && !bAllowOverwrite)
    {
        OutResult.bCancelled = true;
        return true;
    }

    if (!Plan.DonorSourceRoot.IsEmpty() && !MHValidateMaterialDonorDestinations(Plan, OutError))
        return false;

    for (const FMHPreparedMaterialDocumentExport& Prepared : Plan.Ready)
    {
        if (!Prepared.ExpectedDestinationHash.IsEmpty())
        {
            TArray<uint8> CurrentBytes;
            if (!FFileHelper::LoadFileToArray(CurrentBytes, *Prepared.DestinationPath) ||
                MHRawPayloadHash(CurrentBytes) != Prepared.ExpectedDestinationHash)
            {
                OutResult.FailedWrites.Add(MakeFailure(Prepared.Material.Get(), Prepared.DestinationPath,
                    TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: material document changed after donor preflight; prepare the transfer again")));
                continue;
            }
        }
        if (!Prepared.bOverwritesExistingFile &&
            IFileManager::Get().FileExists(*Prepared.DestinationPath))
        {
            const FString ItemError = FString::Printf(
                TEXT("MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED: export target appeared after preflight: %s"),
                *Prepared.DestinationPath);
            OutResult.FailedWrites.Add(MakeFailure(
                Prepared.Material.Get(),
                Prepared.DestinationPath,
                ItemError));
            continue;
        }

        FString ItemError;
        if (!AtomicWriteCanonicalMaterial(
                Prepared.DestinationPath,
                Prepared.CanonicalBytes,
                ItemError))
        {
            OutResult.FailedWrites.Add(MakeFailure(
                Prepared.Material.Get(),
                Prepared.DestinationPath,
                ItemError));
            continue;
        }
        ++OutResult.ExportedCount;
        OutResult.ExportedPaths.Add(Prepared.DestinationPath);
    }

    if (!OutResult.FailedWrites.IsEmpty())
    {
        OutError = FString::Printf(
            TEXT("MH_E_PARTIAL_PUBLISH: %d material document export write(s) failed"),
            OutResult.FailedWrites.Num());
        return false;
    }
    return true;
}

} // namespace UE::MimirComposite
