#pragma once

#include "CoreMinimal.h"
#include "Ledger/MHImportLedger.h"
#include "Source/MHChangeDetector.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

enum class EMHSourceChange : uint8
{
    Create,
    Reimport,
    Move,
    NoChange,
    Remove,
    Blocked
};

MIMIRCOMPOSITEEDITOR_API const TCHAR* MHSourceChangeLabel(EMHSourceChange Change);
MIMIRCOMPOSITEEDITOR_API bool MHSourceChangeAdvancesLedger(EMHSourceChange Change);

struct MIMIRCOMPOSITEEDITOR_API FMHSourceAnalysisEntry
{
    FMHResourceKey Key;
    FString PayloadPath;
    FString SourcePath;
    FString RawHash;
    EMHSourceChange Change = EMHSourceChange::NoChange;
    TArray<FString> Warnings;
    TArray<FString> Errors;
    bool bLedgerAdvanceAllowed = false;
};

struct MIMIRCOMPOSITEEDITOR_API FMHSourceAnalysis
{
    TArray<FMHSourceAnalysisEntry> Entries;
    TArray<FString> Warnings;
    TArray<FString> Errors;

    const FMHSourceAnalysisEntry* Find(const FMHResourceKey& Key) const;
    int32 CountOf(EMHSourceChange Change) const;
    bool HasErrors() const;
};

/** Deprecated Ledger-backed detector retained only until S4. */
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

MIMIRCOMPOSITEEDITOR_API void MHAnalyzeSources(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis);

MIMIRCOMPOSITEEDITOR_API bool MHLedgerRowFromAnalysis(
    const FMHSourceAnalysisEntry& Entry,
    FMHLedgerRow& OutRow);

} // namespace UE::MimirComposite
