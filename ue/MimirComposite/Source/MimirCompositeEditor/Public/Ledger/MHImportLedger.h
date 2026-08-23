#pragma once

#include "Composite/MHCompositeTypes.h"
#include "CoreMinimal.h"
#include "Misc/DateTime.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "MHImportLedger.generated.h"

/**
 * Deprecated pre-S4 reader state. It is not source authority and is replaced
 * by ProjectIndex.sqlite in Source Protocol v4 slice S4.
 */
USTRUCT()
struct MIMIRCOMPOSITEEDITOR_API FMHLedgerRow
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    EMHResourceKind Kind = EMHResourceKind::StaticMesh;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString LogicalName;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FSoftObjectPath Asset;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString SourcePath;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString AppliedRawHash;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FDateTime ImportedAt;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString ImportStatus;
};

/** Deprecated UObject carrier kept compilable until S4 replaces it. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHImportLedger final : public UObject
{
    GENERATED_BODY()

public:
    /** Explicit transition marker; S4 removes this carrier. */
    static constexpr bool bIsDeprecated = true;

    /** Serialized FMHResourceKey::ToString() -> last observed reader state. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TMap<FString, FMHLedgerRow> Rows;

    static UMHImportLedger* LoadOrCreate(const FString& ContentRoot);
    static UMHImportLedger* LoadExisting(const FString& ContentRoot);
    bool Save();
};

MIMIRCOMPOSITEEDITOR_API bool MHLedgerSnapshotToJson(
    const TMap<FString, FMHLedgerRow>& Rows,
    FString& OutJson);

MIMIRCOMPOSITEEDITOR_API bool MHLedgerSnapshotFromJson(
    const FString& Json,
    TMap<FString, FMHLedgerRow>& OutRows,
    FString& OutError);
