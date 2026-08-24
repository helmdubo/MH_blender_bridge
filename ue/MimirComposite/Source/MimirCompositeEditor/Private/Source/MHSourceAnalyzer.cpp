#include "Source/MHSourceAnalyzer.h"

namespace UE::MimirComposite
{

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

void MHAnalyzeSources(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis)
{
    ChangeDetector.DetectChanges(Resolver, SourceRoot, OutAnalysis);
}

} // namespace UE::MimirComposite
