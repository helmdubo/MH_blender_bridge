#pragma once

#include "CoreMinimal.h"
#include "Source/MHChangeDetector.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

/** Total GeneratedAssets.status dictionary from Source Protocol v4 section 3. */
enum class EMHGeneratedAssetStatus : uint8
{
    Applied,
    Stale,
    Orphan,
    SourceBlocked,
    InvalidReceipt,
    DuplicateClaim
};

MIMIRCOMPOSITEEDITOR_API const TCHAR* MHGeneratedAssetStatusLabel(
    EMHGeneratedAssetStatus Status);

/**
 * Asset Registry projection injected by the editor lifecycle layer.
 *
 * The six tag values are kept explicit so the index can validate incomplete
 * records without loading the UObject. CarrierKind is supplied from the
 * AssetData class/carrier inspection performed by the caller.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHGeneratedAssetTagClaim
{
    FString UEObjectPath;
    FString Kind;
    FString LogicalName;
    FString SourcePath;
    FString SourceHash;
    FString AppliedHash;
    FString Managed;
    int32 MHTagCount = 0;
    bool bHasCarrierKind = false;
    EMHResourceKind CarrierKind = EMHResourceKind::StaticMesh;
};

struct MIMIRCOMPOSITEEDITOR_API FMHProjectIndexGeneratedAssetState
{
    FMHResourceKey Key;
    /** Raw persisted fields remain available even when bKeyValid is false. */
    FString KindLabel;
    FString LogicalName;
    FString UEObjectPath;
    FString SourcePath;
    FString SourceHash;
    FString AppliedHash;
    EMHGeneratedAssetStatus Status = EMHGeneratedAssetStatus::InvalidReceipt;
    bool bKeyValid = false;
    bool bReceiptValid = false;
};

enum class EMHProjectDiagnosticSeverity : uint8
{
    Warning,
    Error
};

/** Typed reader view of one derived Diagnostics row. */
struct MIMIRCOMPOSITEEDITOR_API FMHProjectIndexDiagnostic
{
    EMHProjectDiagnosticSeverity Severity = EMHProjectDiagnosticSeverity::Error;
    FString Code;
    FString OwnerKind;
    FString OwnerName;
    FString Path;
    FString TargetKind;
    FString TargetName;
    FString Message;
};

/** Session-only events are deliberately excluded from the normalized dump. */
struct MIMIRCOMPOSITEEDITOR_API FMHProjectIndexUpdateResult
{
    int64 Generation = 0;
    TArray<FString> SessionEvents;
    /** Ephemeral watcher scope: changed keys, reverse dependents and their full
     * forward closures over the union of pre/post-upsert edges. Never stored. */
    TArray<FMHResourceKey> AffectedResourceKeys;
};

/**
 * UE-only, rebuildable projection of source files and managed Asset Registry
 * claims. The database is never source authority and is never migrated.
 */
class MIMIRCOMPOSITEEDITOR_API FMHProjectResourceIndex
{
public:
    static FString DefaultDatabasePath();

    explicit FMHProjectResourceIndex(
        FString InSourceRoot,
        FString InDatabasePath = FString());
    ~FMHProjectResourceIndex();

    FMHProjectResourceIndex(const FMHProjectResourceIndex&) = delete;
    FMHProjectResourceIndex& operator=(const FMHProjectResourceIndex&) = delete;

    /** Opens a valid mh.project_index:4 cache or recreates an empty cache. */
    bool Open(bool& bOutRecreated, FString& OutError);
    void Close();
    bool IsOpen() const;

    /** One complete source snapshot plus the complete six-tag asset projection. */
    bool FullScan(
        const TArray<FMHGeneratedAssetTagClaim>& GeneratedAssets,
        FMHProjectIndexUpdateResult& OutResult,
        FString& OutError);

    /** Incremental file batch used by Publish in S4 and DirectoryWatcher in S6. */
    bool UpsertPaths(
        const TArray<FString>& Paths,
        FMHProjectIndexUpdateResult& OutResult,
        FString& OutError);

    /** Replaces only the complete Asset Registry projection transactionally. */
    bool ReplaceGeneratedAssets(
        const TArray<FMHGeneratedAssetTagClaim>& GeneratedAssets,
        FMHProjectIndexUpdateResult& OutResult,
        FString& OutError);

    /** Registers the single-shot token after atomic replace and before UpsertPaths. */
    bool RegisterSelfPublishAfterReplace(
        const FString& Path,
        const FString& RawHash,
        FString& OutError);

    /** Consumes one session-only divergent orphan-rebind event for Key. */
    bool ConsumeOrphanRebindEvent(
        const FMHResourceKey& Key,
        FString& OutEvent);

    FMHResolveOutcome Resolve(const FMHResourceKey& Key) const;
    FMHSourceSnapshot GetSnapshot() const;
    bool IsImportBlocked(const FMHResourceKey& Key, FString& OutDiagnostic) const;

    bool GetGeneratedAssets(
        const FMHResourceKey& Key,
        TArray<FMHProjectIndexGeneratedAssetState>& OutAssets,
        FString& OutError) const;

    /** Stable profile edges for importer-owned inline freshness checks. */
    bool GetPlacementProfileDependencies(
        const FMHResourceKey& CompositeKey,
        TArray<FMHResourceKey>& OutProfiles,
        FString& OutError) const;

    /** Enumerates every generated claim, including malformed key rows. */
    bool GetAllGeneratedAssets(
        TArray<FMHProjectIndexGeneratedAssetState>& OutAssets,
        FString& OutError) const;

    /** Enumerates the stable derived diagnostics projection without string parsing. */
    bool GetDiagnostics(
        TArray<FMHProjectIndexDiagnostic>& OutDiagnostics,
        FString& OutError) const;

    /** Stable logical dump of the five projection tables; volatile fields omitted. */
    bool BuildNormalizedDump(FString& OutDump, FString& OutError) const;

    int64 GetGeneration() const;
    int32 GetFullScanCountForTests() const;
    const FString& GetSourceRoot() const;
    const FString& GetDatabasePath() const;

#if WITH_DEV_AUTOMATION_TESTS
    /** Narrow corruption seam used to verify semantic cache rejection on reopen. */
    bool InjectResolutionStatusForTests(
        const FMHResourceKey& Key,
        const FString& Status,
        FString& OutError);
    bool InjectExtraSchemaColumnForTests(FString& OutError);
#endif

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};

/** IMHSourceResolver adapter; every Resolve is a keyed SQLite lookup. */
class MIMIRCOMPOSITEEDITOR_API FMHProjectIndexResolver final : public IMHSourceResolver
{
public:
    explicit FMHProjectIndexResolver(FMHProjectResourceIndex& InIndex)
        : Index(InIndex)
    {
    }

    virtual FMHSourceSnapshot GetSnapshot() const override;
    virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) override;

private:
    FMHProjectResourceIndex& Index;
};

/** Index-backed source change detector. */
class MIMIRCOMPOSITEEDITOR_API FMHProjectIndexChangeDetector final : public IMHChangeDetector
{
public:
    explicit FMHProjectIndexChangeDetector(FMHProjectResourceIndex& InIndex)
        : Index(InIndex)
    {
    }

    virtual void DetectChanges(
        IMHSourceResolver& Resolver,
        const FString& SourceRoot,
        FMHSourceAnalysis& OutAnalysis) override;

private:
    FMHProjectResourceIndex& Index;
};

} // namespace UE::MimirComposite
