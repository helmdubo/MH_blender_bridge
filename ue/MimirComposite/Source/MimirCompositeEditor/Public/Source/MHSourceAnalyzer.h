#pragma once

#include "Composite/MHCompositeTypes.h"
#include "CoreMinimal.h"
#include "Ledger/MHImportLedger.h"
#include "Source/MHPayloadScanResolver.h"

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
 * Compares an initialized scan against a Ledger row map. Nothing is imported
 * and nothing is written: the analyzer only classifies, so both the silent
 * startup path and the prompt path can share it.
 */
MIMIRCOMPOSITEEDITOR_API void MHAnalyzeSources(
    FMHPayloadScanResolver& Resolver,
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
