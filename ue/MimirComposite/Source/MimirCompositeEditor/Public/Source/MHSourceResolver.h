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
    /** Multiple candidates of the same UID with divergent fingerprints. */
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

    /** Every candidate path carrying this UID, chosen one included. */
    TArray<FString> CandidatePaths;

    /** MH_E_* / MH_W_* diagnostic for non-Resolved or duplicate outcomes. */
    FString Diagnostic;
};

/**
 * Resolver seam of docs/07 §3. Implementations read embedded payload identity;
 * nothing above this interface may depend on how candidates were discovered.
 */
class MIMIRCOMPOSITEEDITOR_API IMHSourceResolver
{
public:
    virtual ~IMHSourceResolver() = default;

    virtual FMHResolveOutcome Resolve(const FString& ResourceUid, EMHResourceKind ExpectedKind) = 0;
};

} // namespace UE::MimirComposite
