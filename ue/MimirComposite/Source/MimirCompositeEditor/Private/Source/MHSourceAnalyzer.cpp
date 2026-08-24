#include "Source/MHSourceAnalyzer.h"

#include "Misc/Paths.h"

namespace UE::MimirComposite
{
namespace
{

bool KeyLess(const FMHResourceKey& A, const FMHResourceKey& B)
{
    if (A.Kind != B.Kind)
    {
        return static_cast<uint8>(A.Kind) < static_cast<uint8>(B.Kind);
    }
    return A.LogicalName < B.LogicalName;
}

bool RelativeSourcePath(
    const FString& SourceRoot,
    const FString& PayloadPath,
    FString& OutPath)
{
    OutPath = PayloadPath;
    FString RootDirectory = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(RootDirectory);
    RootDirectory += TEXT("/");
    if (!FPaths::MakePathRelativeTo(OutPath, *RootDirectory))
    {
        return false;
    }
    FPaths::NormalizeFilename(OutPath);
    return !OutPath.IsEmpty() && !OutPath.StartsWith(TEXT("../")) &&
        OutPath != TEXT("..") && FPaths::IsRelative(OutPath);
}

bool RowToKey(const FString& Serialized, const FMHLedgerRow& Row, FMHResourceKey& OutKey)
{
    OutKey.Kind = Row.Kind;
    OutKey.LogicalName = Row.LogicalName;
    return OutKey.IsCanonical() && OutKey.ToString() == Serialized;
}

} // namespace

const TCHAR* MHSourceChangeLabel(const EMHSourceChange Change)
{
    switch (Change)
    {
    case EMHSourceChange::Create: return TEXT("CREATE");
    case EMHSourceChange::Reimport: return TEXT("REIMPORT");
    case EMHSourceChange::Move: return TEXT("MOVE");
    case EMHSourceChange::NoChange: return TEXT("NO_CHANGE");
    case EMHSourceChange::Remove: return TEXT("REMOVE");
    case EMHSourceChange::Blocked: return TEXT("BLOCKED");
    }
    return TEXT("BLOCKED");
}

bool MHSourceChangeAdvancesLedger(const EMHSourceChange Change)
{
    return Change == EMHSourceChange::Create || Change == EMHSourceChange::Reimport ||
        Change == EMHSourceChange::Move || Change == EMHSourceChange::NoChange;
}

const FMHSourceAnalysisEntry* FMHSourceAnalysis::Find(const FMHResourceKey& Key) const
{
    return Entries.FindByPredicate([&Key](const FMHSourceAnalysisEntry& Entry)
    {
        return Entry.Key == Key;
    });
}

int32 FMHSourceAnalysis::CountOf(const EMHSourceChange Change) const
{
    int32 Count = 0;
    for (const FMHSourceAnalysisEntry& Entry : Entries)
    {
        Count += Entry.Change == Change ? 1 : 0;
    }
    return Count;
}

bool FMHSourceAnalysis::HasErrors() const
{
    return !Errors.IsEmpty() || Entries.ContainsByPredicate([](const FMHSourceAnalysisEntry& Entry)
    {
        return !Entry.Errors.IsEmpty();
    });
}

void FMHLedgerChangeDetector::DetectChanges(
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis)
{
    OutAnalysis = FMHSourceAnalysis();
    const FMHSourceSnapshot Snapshot = Resolver.GetSnapshot();
    OutAnalysis.Warnings = Snapshot.Warnings;
    OutAnalysis.Errors = Snapshot.Errors;

    TArray<FMHResourceKey> Keys = Snapshot.ResourceKeys;
    for (const TPair<FString, FMHLedgerRow>& Pair : Ledger)
    {
        FMHResourceKey Key;
        if (!RowToKey(Pair.Key, Pair.Value, Key))
        {
            OutAnalysis.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: deprecated Ledger row has invalid resource key %s"),
                *Pair.Key));
            continue;
        }
        if (!Keys.Contains(Key))
        {
            Keys.Add(Key);
        }
    }
    Keys.Sort(KeyLess);

    for (const FMHResourceKey& Key : Keys)
    {
        FMHSourceAnalysisEntry& Entry = OutAnalysis.Entries.AddDefaulted_GetRef();
        Entry.Key = Key;
        const FMHLedgerRow* Row = Ledger.Find(Key.ToString());

        if (Row != nullptr)
        {
            const FString NormalizedRowPath = FPaths::ConvertRelativePathToFull(
                SourceRoot,
                Row->SourcePath);
            const FMHSourceQuarantine* Quarantine = Snapshot.Quarantined.FindByPredicate(
                [&NormalizedRowPath](const FMHSourceQuarantine& Candidate)
                {
                    return Candidate.PayloadPath.Equals(
                        NormalizedRowPath,
                        ESearchCase::IgnoreCase);
                });
            if (Quarantine != nullptr)
            {
                Entry.Change = EMHSourceChange::Blocked;
                Entry.Errors.Add(Quarantine->Diagnostic);
                continue;
            }
        }

        const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            if (Outcome.Status == EMHResolveStatus::Unresolved && Row != nullptr)
            {
                Entry.Change = EMHSourceChange::Remove;
            }
            else
            {
                Entry.Change = EMHSourceChange::Blocked;
                Entry.Errors.Add(Outcome.Diagnostic);
            }
            continue;
        }

        Entry.PayloadPath = Outcome.PayloadPath;
        Entry.RawHash = Outcome.RawHash;
        if (!RelativeSourcePath(SourceRoot, Outcome.PayloadPath, Entry.SourcePath))
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: resolved path escapes source_root: %s"),
                *Outcome.PayloadPath));
            continue;
        }

        if (Row == nullptr)
        {
            Entry.Change = EMHSourceChange::Create;
        }
        else if (Row->AppliedRawHash != Entry.RawHash)
        {
            Entry.Change = EMHSourceChange::Reimport;
        }
        else if (Row->SourcePath != Entry.SourcePath)
        {
            Entry.Change = EMHSourceChange::Move;
        }
        else
        {
            Entry.Change = EMHSourceChange::NoChange;
        }
        Entry.bLedgerAdvanceAllowed = MHSourceChangeAdvancesLedger(Entry.Change);
    }
}

void MHAnalyzeSources(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis)
{
    ChangeDetector.DetectChanges(Resolver, SourceRoot, OutAnalysis);
}

bool MHLedgerRowFromAnalysis(
    const FMHSourceAnalysisEntry& Entry,
    FMHLedgerRow& OutRow)
{
    if (!Entry.bLedgerAdvanceAllowed || !Entry.Key.IsCanonical() ||
        Entry.SourcePath.IsEmpty() || Entry.RawHash.IsEmpty())
    {
        return false;
    }
    OutRow = FMHLedgerRow();
    OutRow.Kind = Entry.Key.Kind;
    OutRow.LogicalName = Entry.Key.LogicalName;
    OutRow.SourcePath = Entry.SourcePath;
    OutRow.AppliedRawHash = Entry.RawHash;
    OutRow.ImportedAt = FDateTime::UtcNow();
    OutRow.ImportStatus = MHSourceChangeLabel(Entry.Change);
    return true;
}

} // namespace UE::MimirComposite
