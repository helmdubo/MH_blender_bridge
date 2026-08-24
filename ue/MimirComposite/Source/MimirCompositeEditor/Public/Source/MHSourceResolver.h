#pragma once

#include "Composite/MHCompositeTypes.h"
#include "CoreMinimal.h"
#include "Misc/Crc.h"

namespace UE::MimirComposite
{

/** Source Protocol v4 identity. Source folders never participate in identity. */
struct MIMIRCOMPOSITEEDITOR_API FMHResourceKey
{
    EMHResourceKind Kind = EMHResourceKind::StaticMesh;
    FString LogicalName;

    bool IsCanonical() const;
    FString ToString() const;

    friend bool operator==(const FMHResourceKey& A, const FMHResourceKey& B)
    {
        return A.Kind == B.Kind && A.LogicalName == B.LogicalName;
    }

    friend uint32 GetTypeHash(const FMHResourceKey& Key)
    {
        return HashCombine(
            ::GetTypeHash(static_cast<uint8>(Key.Kind)),
            FCrc::StrCrc32(*Key.LogicalName));
    }
};

MIMIRCOMPOSITEEDITOR_API const TCHAR* MHResourceKindLabel(EMHResourceKind Kind);
MIMIRCOMPOSITEEDITOR_API bool MHResourceKindFromLabel(
    const FString& Label,
    EMHResourceKind& OutKind);

/**
 * Classifies one source filename by the complete v4 extension and validates
 * its logical stem. Unknown extensions return false with an empty error.
 */
MIMIRCOMPOSITEEDITOR_API bool MHResourceKeyFromSourceFile(
    const FString& Path,
    FMHResourceKey& OutKey,
    FString& OutError);

enum class EMHResolveStatus : uint8
{
    Resolved,
    Unresolved,
    Ambiguous,
    Invalid
};

struct MIMIRCOMPOSITEEDITOR_API FMHResolveOutcome
{
    EMHResolveStatus Status = EMHResolveStatus::Unresolved;
    FString PayloadPath;
    FString RawHash;
    TArray<FString> CandidatePaths;
    FString Diagnostic;
};

/** One source file excluded from the immutable scan snapshot. */
struct MIMIRCOMPOSITEEDITOR_API FMHSourceQuarantine
{
    FString PayloadPath;
    FString Diagnostic;
};

/** Immutable discovery view produced by one initialized resolver. */
struct MIMIRCOMPOSITEEDITOR_API FMHSourceSnapshot
{
    TArray<FMHResourceKey> ResourceKeys;
    TArray<FString> Warnings;
    TArray<FString> Errors;
    TArray<FMHSourceQuarantine> Quarantined;
};

/** Replaceable Source Protocol v4 resolver seam. */
class MIMIRCOMPOSITEEDITOR_API IMHSourceResolver
{
public:
    virtual ~IMHSourceResolver() = default;

    virtual FMHSourceSnapshot GetSnapshot() const = 0;
    virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) = 0;
};

} // namespace UE::MimirComposite
