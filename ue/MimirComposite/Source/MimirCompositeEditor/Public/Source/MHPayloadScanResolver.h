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

    /** Runs the scan snapshot; fails only when source_root itself is unusable. */
    bool Initialize(FString& OutError);

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
    };

    void AddPayloadFile(const FString& Path);

    FString SourceRoot;
    TMap<FString, TArray<FCandidate>> CandidatesByUid;
    TArray<FString> Quarantined;
    TArray<FString> LegacySkipped;
    int32 CandidateFileCount = 0;
};

} // namespace UE::MimirComposite
