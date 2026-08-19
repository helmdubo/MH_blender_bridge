#pragma once

#include "Composite/MHCompositeTypes.h"
#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "MHImportLedger.generated.h"

/**
 * One Ledger row of docs/07 section 2. The Ledger is derived reader state: it
 * records what UE last applied, never source authority. Losing it only forces a
 * full startup scan.
 */
USTRUCT()
struct MIMIRCOMPOSITEEDITOR_API FMHLedgerRow
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    EMHResourceKind Kind = EMHResourceKind::Composite;

    /** Imported asset; still empty at gate C1, where nothing is imported yet. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FSoftObjectPath Asset;

    /** Payload path relative to source_root, forward slashes. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString SourcePath;

    /** mh.meshser:2 passport hash; static meshes only. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString AppliedGeometryHash;

    /** Passport descriptor hash, or the single semantic hash of a material/composite. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString AppliedDescriptorHash;

    /** Byte fingerprint of the payload the row was written from. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString PayloadFingerprint;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FDateTime ImportedAt;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString ImportStatus;
};

/**
 * Editor-owned import Ledger stored at `<content_root>/_MH/Ledger`. It lives
 * outside the source tree by construction and is never configurable into it.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHImportLedger final : public UObject
{
    GENERATED_BODY()

public:
    /** ResourceUID -> last applied import state. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TMap<FString, FMHLedgerRow> Rows;

    /** Loads `<ContentRoot>/_MH/Ledger`, creating an empty one when missing. */
    static UMHImportLedger* LoadOrCreate(const FString& ContentRoot);

    /** Writes the owning package to disk; false when the save was rejected. */
    bool Save();
};

/** Stable snapshot spelling of one resource kind. */
MIMIRCOMPOSITEEDITOR_API const TCHAR* MHResourceKindLabel(EMHResourceKind Kind);
MIMIRCOMPOSITEEDITOR_API bool MHResourceKindFromLabel(const FString& Label, EMHResourceKind& OutKind);

/**
 * Plain JSON round-trip of a Ledger row map, independent of UObject packages so
 * commandlets and automation tests can carry reader state without an editor
 * package. This is reader state, not source schema: snapshots belong under
 * Saved/ and must never be written into source_root.
 */
MIMIRCOMPOSITEEDITOR_API bool MHLedgerSnapshotToJson(
    const TMap<FString, FMHLedgerRow>& Rows,
    FString& OutJson);

MIMIRCOMPOSITEEDITOR_API bool MHLedgerSnapshotFromJson(
    const FString& Json,
    TMap<FString, FMHLedgerRow>& OutRows,
    FString& OutError);
