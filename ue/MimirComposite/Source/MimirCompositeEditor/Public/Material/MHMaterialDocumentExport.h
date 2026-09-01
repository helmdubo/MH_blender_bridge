#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UMaterialInstanceConstant;
class UMHCompositeSettings;

namespace UE::MimirComposite
{

struct MIMIRCOMPOSITEEDITOR_API FMHMaterialDocumentExportRequest
{
    UMaterialInstanceConstant* Material = nullptr;
    FString DestinationPath;
};

struct MIMIRCOMPOSITEEDITOR_API FMHMaterialDocumentExportFailure
{
    FString MaterialPath;
    FString DestinationPath;
    FString Error;
};

struct MIMIRCOMPOSITEEDITOR_API FMHPreparedMaterialDocumentExport
{
    TWeakObjectPtr<UMaterialInstanceConstant> Material;
    FString LogicalName;
    FString DestinationPath;
    FString CanonicalHash;
    TArray<uint8> CanonicalBytes;
    bool bOverwritesExistingFile = false;
    bool bMatchesAppliedHash = false;
};

/** Read-only preflight. No file or asset is mutated. */
struct MIMIRCOMPOSITEEDITOR_API FMHMaterialDocumentExportPlan
{
    TArray<FMHPreparedMaterialDocumentExport> Ready;
    TArray<FMHMaterialDocumentExportFailure> Skipped;
    TArray<FString> OverwritePaths;
};

struct MIMIRCOMPOSITEEDITOR_API FMHMaterialDocumentExportResult
{
    int32 ExportedCount = 0;
    bool bCancelled = false;
    TArray<FMHMaterialDocumentExportFailure> FailedWrites;
};

/** Receipt logical name, falling back to the asset object name. */
MIMIRCOMPOSITEEDITOR_API FString MHGetMaterialDocumentExportLogicalName(
    const UMaterialInstanceConstant& Material);

/**
 * Extract and canonicalize every request. A bad material is added to Skipped
 * without blocking valid peers. Any target inside SourceRoot rejects the whole
 * plan before a write can occur.
 */
MIMIRCOMPOSITEEDITOR_API bool MHPrepareMaterialDocumentExport(
    TConstArrayView<FMHMaterialDocumentExportRequest> Requests,
    const UMHCompositeSettings& Settings,
    const FString& SourceRoot,
    FMHMaterialDocumentExportPlan& OutPlan,
    FString& OutError);

/**
 * Commit a prepared plan. When overwrites exist and bAllowOverwrite is false,
 * the whole batch is cancelled and no file is written.
 */
MIMIRCOMPOSITEEDITOR_API bool MHCommitMaterialDocumentExport(
    const FMHMaterialDocumentExportPlan& Plan,
    bool bAllowOverwrite,
    FMHMaterialDocumentExportResult& OutResult,
    FString& OutError);

} // namespace UE::MimirComposite
