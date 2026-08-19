#include "Source/MHSourceAnalyzer.h"

#include "Containers/Set.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/MHFbxPassport.h"
#include "Source/MHPayloadHashes.h"

namespace UE::MimirComposite
{
namespace
{

/** Payload path as stored in the Ledger: relative to source_root, forward slashes. */
FString AnalyzerRelativeSourcePath(const FString& SourceRootFull, const FString& PayloadPath)
{
    FString Full = FPaths::ConvertRelativePathToFull(PayloadPath);
    FPaths::NormalizeFilename(Full);

    FString Root = SourceRootFull;
    FPaths::NormalizeDirectoryName(Root);
    if (!Root.IsEmpty())
    {
        const FString Prefix = Root + TEXT("/");
        if (Full.StartsWith(Prefix, ESearchCase::IgnoreCase))
        {
            return Full.RightChop(Prefix.Len());
        }
    }
    return Full;
}

/**
 * The scan knows the kind of every candidate, the resolver seam does not expose
 * it, so the analyzer asks for each kind in turn. KindMismatch means "some other
 * kind carries this UID", which is exactly the probe answer we discard.
 */
bool AnalyzerProbeKind(
    FMHPayloadScanResolver& Resolver,
    const FString& ResourceUid,
    EMHResourceKind& OutKind,
    FMHResolveOutcome& OutOutcome)
{
    static const EMHResourceKind ProbeOrder[] = {
        EMHResourceKind::StaticMesh,
        EMHResourceKind::Material,
        EMHResourceKind::Composite};

    for (const EMHResourceKind Probe : ProbeOrder)
    {
        FMHResolveOutcome Outcome = Resolver.Resolve(ResourceUid, Probe);
        if (Outcome.Status != EMHResolveStatus::KindMismatch &&
            Outcome.Status != EMHResolveStatus::Unresolved)
        {
            OutKind = Probe;
            OutOutcome = MoveTemp(Outcome);
            return true;
        }
    }
    return false;
}

/** Reads the two semantic hashes of one payload; the reader never recomputes geometry. */
bool AnalyzerReadSemanticHashes(
    const EMHResourceKind Kind,
    const FString& PayloadPath,
    FString& OutGeometryHash,
    FString& OutDescriptorHash,
    FString& OutError)
{
    OutGeometryHash.Reset();
    OutDescriptorHash.Reset();
    OutError.Reset();

    if (Kind == EMHResourceKind::StaticMesh)
    {
        FMHFbxPassport Passport;
        if (!MHReadFbxPassport(PayloadPath, Passport, OutError))
        {
            return false;
        }
        OutGeometryHash = Passport.GeometryHash;
        OutDescriptorHash = MHPassportDescriptorHash(Passport);
        if (OutDescriptorHash.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("MH_E_PASSPORT_INVALID: passport of %s cannot be canonicalized"),
                *PayloadPath);
            return false;
        }
        return true;
    }

    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *PayloadPath))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: cannot read payload bytes of %s"),
            *PayloadPath);
        return false;
    }
    if (Kind == EMHResourceKind::Material)
    {
        return MHMaterialSemanticHash(Bytes, OutDescriptorHash, OutError);
    }
    return MHCompositeSemanticHash(Bytes, OutDescriptorHash, OutError);
}

} // namespace

const TCHAR* MHSourceChangeLabel(const EMHSourceChange Change)
{
    switch (Change)
    {
    case EMHSourceChange::Create: return TEXT("CREATE");
    case EMHSourceChange::UpdateGeometry: return TEXT("UPDATE_GEOMETRY");
    case EMHSourceChange::UpdateDescriptor: return TEXT("UPDATE_DESCRIPTOR");
    case EMHSourceChange::UpdateProperties: return TEXT("UPDATE_PROPERTIES");
    case EMHSourceChange::Move: return TEXT("MOVE");
    case EMHSourceChange::NoChange: return TEXT("NO_CHANGE");
    case EMHSourceChange::NoChangeExternal: return TEXT("NO_CHANGE_EXTERNAL");
    case EMHSourceChange::Remove: return TEXT("REMOVE");
    default: return TEXT("BLOCKED");
    }
}

bool MHSourceChangeAdvancesLedger(const EMHSourceChange Change)
{
    switch (Change)
    {
    case EMHSourceChange::Create:
    case EMHSourceChange::UpdateGeometry:
    case EMHSourceChange::UpdateDescriptor:
    case EMHSourceChange::UpdateProperties:
    case EMHSourceChange::Move:
    case EMHSourceChange::NoChange:
        return true;
    default:
        return false;
    }
}

const FMHSourceAnalysisEntry* FMHSourceAnalysis::Find(const FString& ResourceUid) const
{
    for (const FMHSourceAnalysisEntry& Entry : Entries)
    {
        if (Entry.ResourceUid == ResourceUid)
        {
            return &Entry;
        }
    }
    return nullptr;
}

int32 FMHSourceAnalysis::CountOf(const EMHSourceChange Change) const
{
    int32 Count = 0;
    for (const FMHSourceAnalysisEntry& Entry : Entries)
    {
        if (Entry.Change == Change)
        {
            ++Count;
        }
    }
    return Count;
}

bool FMHSourceAnalysis::HasErrors() const
{
    if (Errors.Num() > 0)
    {
        return true;
    }
    for (const FMHSourceAnalysisEntry& Entry : Entries)
    {
        if (Entry.Errors.Num() > 0)
        {
            return true;
        }
    }
    return false;
}

void MHAnalyzeSources(
    FMHPayloadScanResolver& Resolver,
    const FString& SourceRoot,
    const TMap<FString, FMHLedgerRow>& Ledger,
    FMHSourceAnalysis& OutAnalysis)
{
    OutAnalysis = FMHSourceAnalysis();

    const FString SourceRootFull = FPaths::ConvertRelativePathToFull(SourceRoot);

    // Quarantined payloads never enter the candidate set; docs/05 section 9 keeps
    // them observable without blocking the resources that did resolve.
    for (const FString& Entry : Resolver.GetQuarantined())
    {
        OutAnalysis.Warnings.Add(FString::Printf(TEXT("quarantined: %s"), *Entry));
    }
    for (const FString& Entry : Resolver.GetLegacySkipped())
    {
        OutAnalysis.Warnings.Add(FString::Printf(TEXT("legacy v1 (migration only): %s"), *Entry));
    }

    TArray<FString> Uids = Resolver.GetAllUids();
    const TSet<FString> Scanned(Uids);
    for (const TPair<FString, FMHLedgerRow>& Row : Ledger)
    {
        if (!Scanned.Contains(Row.Key))
        {
            Uids.Add(Row.Key);
        }
    }
    Uids.Sort();

    for (const FString& Uid : Uids)
    {
        const FMHLedgerRow* Row = Ledger.Find(Uid);

        FMHSourceAnalysisEntry Analyzed;
        Analyzed.ResourceUid = Uid;

        // A Ledger UID without any valid candidate is an orphan: classify it and
        // leave the actual asset removal to a later gate.
        if (!Scanned.Contains(Uid))
        {
            Analyzed.Kind = Row != nullptr ? Row->Kind : EMHResourceKind::Composite;
            Analyzed.SourcePath = Row != nullptr ? Row->SourcePath : FString();
            Analyzed.Change = EMHSourceChange::Remove;
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }

        EMHResourceKind Kind = EMHResourceKind::Composite;
        FMHResolveOutcome Outcome;
        if (!AnalyzerProbeKind(Resolver, Uid, Kind, Outcome))
        {
            Analyzed.Kind = Row != nullptr ? Row->Kind : EMHResourceKind::Composite;
            Analyzed.Change = EMHSourceChange::Blocked;
            Analyzed.Errors.Add(FString::Printf(
                TEXT("MH_E_RESOURCE_NOT_FOUND: scanned UID %s no longer resolves"),
                *Uid));
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }
        Analyzed.Kind = Kind;

        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            Analyzed.Change = EMHSourceChange::Blocked;
            Analyzed.SourcePath = Row != nullptr ? Row->SourcePath : FString();
            Analyzed.Errors.Add(Outcome.Diagnostic);
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }

        Analyzed.Name = Outcome.Name;
        Analyzed.PayloadPath = Outcome.PayloadPath;
        Analyzed.SourcePath = AnalyzerRelativeSourcePath(SourceRootFull, Outcome.PayloadPath);
        Analyzed.Fingerprint = Outcome.Fingerprint;
        if (!Outcome.Diagnostic.IsEmpty())
        {
            Analyzed.Warnings.Add(Outcome.Diagnostic);
        }

        FString HashError;
        if (!AnalyzerReadSemanticHashes(
                Kind,
                Outcome.PayloadPath,
                Analyzed.GeometryHash,
                Analyzed.DescriptorHash,
                HashError))
        {
            Analyzed.Change = EMHSourceChange::Blocked;
            Analyzed.Errors.Add(HashError);
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }

        if (Row == nullptr)
        {
            Analyzed.Change = EMHSourceChange::Create;
        }
        else if (Kind == EMHResourceKind::StaticMesh && Analyzed.GeometryHash != Row->AppliedGeometryHash)
        {
            Analyzed.Change = EMHSourceChange::UpdateGeometry;
        }
        else if (Analyzed.DescriptorHash != Row->AppliedDescriptorHash)
        {
            Analyzed.Change = Kind == EMHResourceKind::StaticMesh
                ? EMHSourceChange::UpdateDescriptor
                : EMHSourceChange::UpdateProperties;
        }
        else if (Analyzed.SourcePath != Row->SourcePath)
        {
            Analyzed.Change = EMHSourceChange::Move;
        }
        else if (!Row->PayloadFingerprint.IsEmpty() && Analyzed.Fingerprint != Row->PayloadFingerprint)
        {
            // Both semantic hashes are equal, so nothing is reimported; the row
            // still may not advance without an explicit confirmation.
            Analyzed.Change = EMHSourceChange::NoChangeExternal;
            Analyzed.Warnings.Add(FString::Printf(
                TEXT("MH_W_PAYLOAD_EXTERNAL_MODIFIED: %s changed on disk while both semantic hashes stayed equal"),
                *Analyzed.SourcePath));
            Analyzed.Errors.Add(FString::Printf(
                TEXT("MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED: %s needs explicit confirmation before the Ledger advances"),
                *Analyzed.SourcePath));
        }
        else
        {
            Analyzed.Change = EMHSourceChange::NoChange;
        }

        Analyzed.bLedgerAdvanceAllowed = MHSourceChangeAdvancesLedger(Analyzed.Change);
        OutAnalysis.Entries.Add(MoveTemp(Analyzed));
    }
}

bool MHLedgerRowFromAnalysis(const FMHSourceAnalysisEntry& Entry, FMHLedgerRow& OutRow)
{
    if (!Entry.bLedgerAdvanceAllowed)
    {
        return false;
    }
    // Asset stays empty: C1 imports nothing, and only the importer of C2 knows
    // which package a row points at.
    OutRow = FMHLedgerRow();
    OutRow.Kind = Entry.Kind;
    OutRow.SourcePath = Entry.SourcePath;
    OutRow.AppliedGeometryHash = Entry.GeometryHash;
    OutRow.AppliedDescriptorHash = Entry.DescriptorHash;
    OutRow.PayloadFingerprint = Entry.Fingerprint;
    OutRow.ImportedAt = FDateTime::UtcNow();
    // C1 classifies only; C2 replaces this with the real import result.
    OutRow.ImportStatus = MHSourceChangeLabel(Entry.Change);
    return true;
}

} // namespace UE::MimirComposite
