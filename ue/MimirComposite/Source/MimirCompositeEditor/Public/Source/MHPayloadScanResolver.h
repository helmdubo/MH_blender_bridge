#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

/**
 * Clean Sources v2 scan resolver: recursively collects the three primary
 * payload types under source_root and resolves UIDs from embedded identity
 * (composite/material self-identity, FBX Carrier B passport). Per-file
 * failures quarantine only that payload; the scan itself stays usable.
 */
class MIMIRCOMPOSITEEDITOR_API FMHPayloadScanResolver final : public IMHSourceResolver
{
public:
    explicit FMHPayloadScanResolver(FString InSourceRoot);

    /** Runs one fail-closed scan snapshot, including traversal/set stability checks. */
    bool Initialize(FString& OutError);

    virtual FMHSourceSnapshot GetSnapshot() const override
    {
        FMHSourceSnapshot Snapshot;
        Snapshot.ResourceUids = GetAllUids();
        Snapshot.Quarantined = QuarantineEntries;
        for (const FMHSourceQuarantine& Entry : QuarantineEntries)
        {
            Snapshot.Errors.Add(Entry.Diagnostic);
        }
        for (const FString& Entry : LegacySkipped)
        {
            Snapshot.Warnings.Add(FString::Printf(TEXT("legacy v1 (migration only): %s"), *Entry));
        }
        return Snapshot;
    }

    virtual FMHResolveOutcome Resolve(const FString& ResourceUid, EMHResourceKind ExpectedKind) override;

    /** Per-file "path: MH_E_..." diagnostics for payloads excluded from the candidate set. */
    const TArray<FString>& GetQuarantined() const { return Quarantined; }

    /** Legacy v1 composites that only the migration utility may read. */
    const TArray<FString>& GetLegacySkipped() const { return LegacySkipped; }

    /** Every ResourceUID the scan found a valid candidate for, sorted. */
    TArray<FString> GetAllUids() const;

    int32 GetCandidateFileCount() const { return CandidateFileCount; }

private:
    struct FCandidate
    {
        EMHResourceKind Kind = EMHResourceKind::Composite;
        FString Uid;
        FString Name;
        FString Path;
        FString Fingerprint;
        FString GeometryHash;
        FString DescriptorHash;
    };

    void AddPayloadFile(const FString& Path);
    bool DiscoverPayloadPaths(TArray<FString>& OutPaths, FString& OutError) const;
    void QuarantinePayload(const FString& Path, const FString& Diagnostic);

    FString SourceRoot;
    TMap<FString, TArray<FCandidate>> CandidatesByUid;
    TArray<FString> Quarantined;
    TArray<FMHSourceQuarantine> QuarantineEntries;
    TArray<FString> LegacySkipped;
    int32 CandidateFileCount = 0;
};

} // namespace UE::MimirComposite
