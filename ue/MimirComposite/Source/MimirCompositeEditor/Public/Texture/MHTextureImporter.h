#pragma once

#include "CoreMinimal.h"

class UTexture;

namespace UE::MimirComposite
{

struct FMHSourceAnalysisEntry;

/** Exact logical-name policy for managed normal maps. */
MIMIRCOMPOSITEEDITOR_API bool MHTextureIsManagedNormalMapLogicalName(
    const FString& LogicalName);

struct MIMIRCOMPOSITEEDITOR_API FMHTextureOperationResult
{
    UTexture* Texture = nullptr;
    TArray<FString> Warnings;
    FString Error;
    bool bImported = false;

    bool Succeeded() const { return Texture != nullptr && Error.IsEmpty(); }
};

struct MIMIRCOMPOSITEEDITOR_API FMHTextureBulkImportRequest
{
    const FMHSourceAnalysisEntry* Entry = nullptr;
    bool bForceReimport = false;
};

/**
 * Ensures the canonical managed texture matches Entry. Coordinator imports use
 * bForceReimport=true; material binding uses false and reuses an exact receipt.
 */
MIMIRCOMPOSITEEDITOR_API FMHTextureOperationResult MHEnsureTextureV4(
    const FMHSourceAnalysisEntry& Entry,
    const FString& SourceRoot,
    bool bForceReimport);

/** Schedule every texture task before awaiting any result. Results align with Requests. */
MIMIRCOMPOSITEEDITOR_API void MHEnsureTextureBatchV4(
    TConstArrayView<FMHTextureBulkImportRequest> Requests,
    const FString& SourceRoot,
    TArray<FMHTextureOperationResult>& OutResults);

} // namespace UE::MimirComposite
