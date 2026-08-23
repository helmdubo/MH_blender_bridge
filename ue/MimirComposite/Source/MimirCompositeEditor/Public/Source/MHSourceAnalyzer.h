#pragma once

#include "Composite/MHCompositeTypes.h"
#include "CoreMinimal.h"
#include "Ledger/MHImportLedger.h"
#include "Source/MHChangeDetector.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

/**
 * Per-resource classification of docs/07 section 4 and docs/05 section 9.
 * Batch and dependency order stay out of this enum: the analyzer answers only
 * "what happened to this ResourceUID", never "in which wave is it imported".
 */
enum class EMHSourceChange : uint8
{
    /** Scanned UID the Ledger has never seen. */
    Create,
    /** Static mesh whose passport geometry_hash moved. */
    UpdateGeometry,
    /** Static mesh whose descriptor (metadata/slots) moved. */
    UpdateDescriptor,
    /** Material or composite whose canonical payload moved. */
    UpdateProperties,
    /** Same UID, same semantics, different path under source_root. */
    Move,
    /** Nothing to do. */
    NoChange,
    /** Bytes changed while both semantic hashes stayed equal: confirmation gate. */
    NoChangeExternal,
    /** Ledger row whose UID no longer has a valid payload. */
    Remove,
    /** MH_E_* blocks this resource; the rest of the analysis continues. */
    Blocked
};

/** Stable report/log spelling of one classification. */
MIMIRCOMPOSITEEDITOR_API const TCHAR* MHSourceChangeLabel(EMHSourceChange Change);

/** True when the Ledger may be advanced from an entry with this classification. */
MIMIRCOMPOSITEEDITOR_API bool MHSourceChangeAdvancesLedger(EMHSourceChange Change);

/**
 * Ordered mh.diff_report:1 flag space. Numeric order is contractual and must
 * stay identical to addon/mh4blend/core/diff.py FLAG_ORDER.
 */
enum class EMHDiffFlag : uint8
{
    Create,
    Remove,
    Rename,
    UpdateGeometry,
    UpdateTransform,
    UpdateProperties,
    Reparent,
    UpdateResource,
    UpdateKind,
    Move,
    LocalEdit,
    Conflict,
    ExternalUnresolved
};

/** Stable mh.diff_report spelling of one ordered flag. */
MIMIRCOMPOSITEEDITOR_API const TCHAR* MHDiffFlagLabel(EMHDiffFlag Flag);

/**
 * One validated node value from a canonical mh.composite document. Canonical
 * value strings are opaque equality tokens produced by the strict snapshot
 * loader, not JSON reconstructed from a Ledger row.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHDiffNodeValue
{
    FString NodeUid;
    FString DisplayName;
    FString CanonicalTransformValue;
    FString CanonicalPropertiesValue;
    FString ParentUid;
    FString ResourceUid;
    FString Kind;
};

/**
 * Validated value-space image of one source resource.
 *
 * CanonicalSemanticValue means the canonical material semantic value, the
 * canonical composite top-level properties value, or for a static mesh the
 * canonical {lod_levels,lod_policy,material_slots,properties} value. The mesh
 * value deliberately excludes name, geometry_hash and exporter. A current C1
 * Ledger row cannot truthfully populate these fields.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHDiffResourceValue
{
    FString ResourceUid;
    EMHResourceKind Kind = EMHResourceKind::Composite;
    FString Name;
    /** Validated normalized source-root-relative POSIX path. */
    FString SourcePath;

    /** Static-mesh fast-path values; empty for material/composite. */
    FString GeometryHash;
    FString DescriptorHash;

    FString CanonicalSemanticValue;
    bool bHasValidatedSemanticValue = false;

    /** Required and true only for a validated composite document. */
    TArray<FMHDiffNodeValue> Nodes;
    bool bHasValidatedNodeValues = false;
};

/** Complete in-memory value input to the independent parity report builder. */
struct MIMIRCOMPOSITEEDITOR_API FMHDiffSnapshotValue
{
    /** Key must equal FMHDiffResourceValue::ResourceUid. */
    TMap<FString, FMHDiffResourceValue> Resources;
};

/** Ordered flags for one ResourceUID; empty sets are omitted from reports. */
struct MIMIRCOMPOSITEEDITOR_API FMHDiffResourceOp
{
    FString ResourceUid;
    TArray<EMHDiffFlag> Flags;
};

/** Ordered flags for one composite NodeUID. */
struct MIMIRCOMPOSITEEDITOR_API FMHDiffNodeOp
{
    FString NodeUid;
    TArray<EMHDiffFlag> Flags;
};

/** Node operations of one composite resource. */
struct MIMIRCOMPOSITEEDITOR_API FMHDiffCompositeNodeOps
{
    FString ResourceUid;
    TArray<FMHDiffNodeOp> Nodes;

    const FMHDiffNodeOp* Find(const FString& NodeUid) const;
};

/** In-memory mh.diff_report:1, sorted by ResourceUID and NodeUID. */
struct MIMIRCOMPOSITEEDITOR_API FMHDiffReport
{
    TArray<FMHDiffResourceOp> Resources;
    TArray<FMHDiffCompositeNodeOps> Nodes;

    const FMHDiffResourceOp* FindResource(const FString& ResourceUid) const;
    const FMHDiffCompositeNodeOps* FindCompositeNodes(const FString& ResourceUid) const;
};

/**
 * Builds Python-parity resource and composite-node operations from two
 * validated value snapshots. Returns false and leaves OutReport empty when a
 * same-resource comparison lacks the full canonical values required by its
 * kind; it never infers missing document state from hashes or the Ledger.
 */
MIMIRCOMPOSITEEDITOR_API bool MHBuildDiffReport(
    const FMHDiffSnapshotValue& OldSnapshot,
    const FMHDiffSnapshotValue& NewSnapshot,
    FMHDiffReport& OutReport,
    FString& OutError);

/** Deterministic structural JSON serialization of mh.diff_report:1. */
MIMIRCOMPOSITEEDITOR_API bool MHDiffReportToJson(
    const FMHDiffReport& Report,
    FString& OutJson,
    FString& OutError);

/** One analyzed ResourceUID. */
struct MIMIRCOMPOSITEEDITOR_API FMHSourceAnalysisEntry
{
    FString ResourceUid;
    EMHResourceKind Kind = EMHResourceKind::Composite;

    /** Embedded resource name of the chosen payload; empty for REMOVE/BLOCKED. */
    FString Name;

    /** Absolute path of the chosen candidate; empty for REMOVE/BLOCKED. */
    FString PayloadPath;

    /** Chosen payload path relative to source_root, forward slashes. */
    FString SourcePath;

    /** Passport geometry_hash; static meshes only. */
    FString GeometryHash;

    /** Passport descriptor hash, or the semantic hash of a material/composite. */
    FString DescriptorHash;

    FString Fingerprint;

    EMHSourceChange Change = EMHSourceChange::NoChange;

    /** MH_W_* facts about this resource. */
    TArray<FString> Warnings;

    /** MH_E_* diagnostics blocking this resource only. */
    TArray<FString> Errors;

    /** False whenever the Ledger must not be advanced from this entry. */
    bool bLedgerAdvanceAllowed = false;
};

/** Result of one reader-side analysis pass over source_root. */
struct MIMIRCOMPOSITEEDITOR_API FMHSourceAnalysis
{
    /** One entry per ResourceUID of the scan and the Ledger, sorted by UID. */
    TArray<FMHSourceAnalysisEntry> Entries;

    /**
     * Rich mh.diff_report:1 result produced by the active IMHChangeDetector.
     * False means the detector cannot prove parity from complete old/new value
     * snapshots; consumers must ignore DiffReport and must not fabricate it
     * from Ledger hashes or the scalar execution-priority Change values.
     */
    bool bHasDiffReport = false;
    FMHDiffReport DiffReport;

    /** Scan-level MH_W_*: quarantined payloads, legacy v1 files. */
    TArray<FString> Warnings;

    /** Scan-level MH_E_* that belong to no single resource. */
    TArray<FString> Errors;

    const FMHSourceAnalysisEntry* Find(const FString& ResourceUid) const;
    int32 CountOf(EMHSourceChange Change) const;

    /** True when any MH_E_* was raised, scan-level or per resource. */
    bool HasErrors() const;
};

/**
 * Current C1 detector backed by an immutable Ledger view. Ledger storage is an
 * implementation detail of this class and is intentionally absent from
 * IMHChangeDetector so an asset-applied-state detector can replace it later.
 */
class MIMIRCOMPOSITEEDITOR_API FMHLedgerChangeDetector final : public IMHChangeDetector
{
public:
    explicit FMHLedgerChangeDetector(TMap<FString, FMHLedgerRow> InLedger)
        : Ledger(MoveTemp(InLedger))
    {
    }

    virtual void DetectChanges(
        IMHSourceResolver& Resolver,
        const FString& SourceRoot,
        FMHSourceAnalysis& OutAnalysis) override;

private:
    TMap<FString, FMHLedgerRow> Ledger;
};

/** Dispatches analysis exclusively through the replaceable detector seam. */
MIMIRCOMPOSITEEDITOR_API void MHAnalyzeSources(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis);

/**
 * C1 composition adapter for existing Ledger callers. The classification still
 * crosses IMHChangeDetector; consumers that inject a detector use the overload
 * above and do not depend on Ledger.
 */
MIMIRCOMPOSITEEDITOR_API void MHAnalyzeSources(
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const TMap<FString, FMHLedgerRow>& Ledger,
    FMHSourceAnalysis& OutAnalysis);

/**
 * Builds the Ledger row an entry would commit. Returns false for entries that
 * must not advance the Ledger (REMOVE, BLOCKED and the external-modification
 * confirmation gate).
 */
MIMIRCOMPOSITEEDITOR_API bool MHLedgerRowFromAnalysis(
    const FMHSourceAnalysisEntry& Entry,
    FMHLedgerRow& OutRow);

} // namespace UE::MimirComposite
