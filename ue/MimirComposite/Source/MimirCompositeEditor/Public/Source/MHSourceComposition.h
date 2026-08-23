#pragma once

#include "CoreMinimal.h"
#include "Ledger/MHImportLedger.h"
#include "Source/MHChangeDetector.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

/** Default C1 service composition, isolated from the import coordinator. */
struct MIMIRCOMPOSITEEDITOR_API FMHSourceAnalysisServices
{
    TUniquePtr<IMHSourceResolver> Resolver;
    TUniquePtr<IMHChangeDetector> ChangeDetector;
};

/** Creates the current payload scanner behind IMHSourceResolver. */
MIMIRCOMPOSITEEDITOR_API bool MHCreateDefaultSourceResolver(
    const FString& SourceRoot,
    TUniquePtr<IMHSourceResolver>& OutResolver,
    FString& OutError);

/** Wraps an explicit reader-state snapshot behind IMHChangeDetector. */
MIMIRCOMPOSITEEDITOR_API TUniquePtr<IMHChangeDetector> MHCreateSnapshotChangeDetector(
    TMap<FString, FMHLedgerRow> ReaderState);

/**
 * Creates the current reader implementations behind their public seams.
 * Future protocol/applied-state pivots replace only this composition root.
 */
MIMIRCOMPOSITEEDITOR_API bool MHCreateDefaultSourceAnalysisServices(
    const FString& SourceRoot,
    const FString& ContentRoot,
    FMHSourceAnalysisServices& OutServices,
    FString& OutError);

} // namespace UE::MimirComposite
