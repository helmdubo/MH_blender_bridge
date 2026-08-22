#pragma once

#include "Composite/MHCompositeTypes.h"
#include "CoreMinimal.h"

namespace UE::MimirComposite
{

/** Clean Sources v2 resolve outcome for one ResourceUID. */
enum class EMHResolveStatus : uint8
{
    /** Exactly one valid candidate, or several byte-identical ones. */
    Resolved,
    /** No valid candidate under source_root. */
    Unresolved,
    /** Same UID has divergent revisions or declarations of different kinds. */
    DivergentRevisions,
    /** Valid candidates exist but none matches the expected kind. */
    KindMismatch
};

struct MIMIRCOMPOSITEEDITOR_API FMHResolveOutcome
{
    EMHResolveStatus Status = EMHResolveStatus::Unresolved;

    /** Chosen payload path when Status == Resolved. */
    FString PayloadPath;

    /** Embedded resource name of the chosen payload. */
    FString Name;

    /** Payload fingerprint of the chosen candidate when Status == Resolved. */
    FString Fingerprint;

    /** Semantic hashes captured from the same immutable scan snapshot. */
    FString GeometryHash;
    FString DescriptorHash;

    /** Every candidate path carrying this UID, chosen one included. */
    TArray<FString> CandidatePaths;

    /** MH_E_* / MH_W_* diagnostic for non-Resolved or duplicate outcomes. */
    FString Diagnostic;
};

/** One primary payload excluded from the immutable scan snapshot. */
struct MIMIRCOMPOSITEEDITOR_API FMHSourceQuarantine
{
    /** Normalized absolute path used to correlate an existing Ledger row. */
    FString PayloadPath;

    /** Complete path-qualified MH_E_* diagnostic. */
    FString Diagnostic;
};

/**
 * Immutable discovery view produced by one initialized resolver. Consumers use
 * this instead of reaching into a concrete scan implementation.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHSourceSnapshot
{
    /** Every ResourceUID with at least one valid candidate, sorted and unique. */
    TArray<FString> ResourceUids;

    /** Scan-level MH_W_* facts that do not belong to one resolvable UID. */
    TArray<FString> Warnings;

    /** Scan-level MH_E_* facts that do not belong to one resolvable UID. */
    TArray<FString> Errors;

    /** Structured invalid-payload facts used to block, never infer REMOVE. */
    TArray<FMHSourceQuarantine> Quarantined;
};

/**
 * Resolver seam of docs/07 §3. Implementations read embedded payload identity;
 * nothing above this interface may depend on how candidates were discovered.
 */
class MIMIRCOMPOSITEEDITOR_API IMHSourceResolver
{
public:
    virtual ~IMHSourceResolver() = default;

    /** Returns the stable discovery snapshot captured by this resolver. */
    virtual FMHSourceSnapshot GetSnapshot() const = 0;

    virtual FMHResolveOutcome Resolve(const FString& ResourceUid, EMHResourceKind ExpectedKind) = 0;
};

} // namespace UE::MimirComposite
