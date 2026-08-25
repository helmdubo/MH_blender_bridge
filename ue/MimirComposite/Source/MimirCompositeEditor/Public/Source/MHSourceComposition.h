#pragma once

#include "CoreMinimal.h"
#include "Index/MHProjectResourceIndex.h"
#include "Source/MHChangeDetector.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

/** Default Source Protocol v4 services sharing one project-index lifetime. */
struct MIMIRCOMPOSITEEDITOR_API FMHSourceAnalysisServices
{
    TSharedPtr<FMHProjectResourceIndex> Index;
    TUniquePtr<IMHSourceResolver> Resolver;
    TUniquePtr<IMHChangeDetector> ChangeDetector;
};

/**
 * Opens/rebuilds the project index, performs one full source/Asset Registry
 * projection, and returns indexed reader services.
 */
MIMIRCOMPOSITEEDITOR_API bool MHCreateDefaultSourceAnalysisServices(
    const FString& SourceRoot,
    FMHSourceAnalysisServices& OutServices,
    FString& OutError);

/**
 * Applies one watcher batch to the live index and returns reader services for
 * that exact generation. A valid existing cache is updated with one
 * UpsertPaths transaction; a newly recreated cache falls back to one complete
 * projection because an incremental batch cannot reconstruct unseen files.
 */
MIMIRCOMPOSITEEDITOR_API bool MHCreateIncrementalSourceAnalysisServices(
    const FString& SourceRoot,
    const TArray<FString>& Paths,
    FMHSourceAnalysisServices& OutServices,
    FMHProjectIndexUpdateResult& OutUpdate,
    bool& bOutUsedFullScan,
    FString& OutError);

/**
 * Publish integration called strictly after atomic source replacement. The
 * single-shot token is registered before the same path is incrementally
 * upserted into the shared project index.
 */
MIMIRCOMPOSITEEDITOR_API bool MHUpsertPublishedSource(
    const FString& SourceRoot,
    const FString& PublishedPath,
    const FString& RawHash,
    TArray<FString>& OutSessionEvents,
    FString& OutError);

/** Reprojects all managed Asset Registry claims after a successful receipt save. */
MIMIRCOMPOSITEEDITOR_API bool MHRefreshGeneratedAssetProjection(
    const FString& SourceRoot,
    FString& OutError);

/** Consumes one live-session orphan-rebind warning after the actual import. */
MIMIRCOMPOSITEEDITOR_API bool MHConsumeOrphanRebindEvent(
    const FString& SourceRoot,
    const FMHResourceKey& Key,
    FString& OutEvent);

/** Closes the process-shared cache before the editor module unloads. */
MIMIRCOMPOSITEEDITOR_API void MHShutdownProjectIndex();

#if WITH_DEV_AUTOMATION_TESTS
/** Focused test seam for the Asset Registry union projection. */
MIMIRCOMPOSITEEDITOR_API void MHGatherGeneratedAssetClaimsForTests(
    TArray<FMHGeneratedAssetTagClaim>& OutClaims);
#endif

} // namespace UE::MimirComposite
