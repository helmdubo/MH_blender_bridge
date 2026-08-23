#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

/** Direct source-tree scanner used until the Project Resource Index lands in S4. */
class MIMIRCOMPOSITEEDITOR_API FMHPayloadScanResolver final : public IMHSourceResolver
{
public:
    explicit FMHPayloadScanResolver(FString InSourceRoot);

    bool Initialize(FString& OutError);
    virtual FMHSourceSnapshot GetSnapshot() const override;
    virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) override;

    int32 GetCandidateFileCount() const { return CandidateFileCount; }

private:
    struct FCandidate
    {
        FMHResourceKey Key;
        FString Path;
        FString RawHash;
    };

    void AddPayloadFile(const FString& Path);
    bool DiscoverPayloadPaths(TArray<FString>& OutPaths, FString& OutError) const;
    void QuarantinePayload(const FString& Path, const FString& Diagnostic);

    FString SourceRoot;
    TMap<FMHResourceKey, TArray<FCandidate>> CandidatesByKey;
    TArray<FMHSourceQuarantine> QuarantineEntries;
    int32 CandidateFileCount = 0;
};

} // namespace UE::MimirComposite
