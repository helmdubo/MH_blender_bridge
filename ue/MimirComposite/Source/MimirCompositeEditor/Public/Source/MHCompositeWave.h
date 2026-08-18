#pragma once

#include "Codec/MHCompositeCodec.h"
#include "CoreMinimal.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

/** Result of one recursive composite_ref dependency wave. */
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeWaveResult
{
    /** Every composite document reached from the root, keyed by ResourceUID. */
    TMap<FString, FMHCompositeDocument> Composites;

    /** Payload path each reached composite was loaded from (root uses its own path). */
    TMap<FString, FString> PayloadPaths;

    /** MH_E_* diagnostics; non-empty means the wave is blocked for UE import. */
    TArray<FString> Errors;

    /** MH_W_* diagnostics (duplicates and similar non-blocking facts). */
    TArray<FString> Warnings;

    /** composite_ref ResourceUIDs that produced no valid candidate. */
    TArray<FString> UnresolvedComposites;
};

/**
 * Walks composite_ref edges of the root document through the resolver,
 * deduplicating shared composites by UID. A dependency cycle produces
 * MH_E_COMPOSITE_CYCLE naming the cycle chain; unresolved references are
 * recorded and do not stop the rest of the graph.
 */
MIMIRCOMPOSITEEDITOR_API void MHWalkCompositeWave(
    IMHSourceResolver& Resolver,
    const FMHCompositeDocument& Root,
    const FString& RootPayloadPath,
    FMHCompositeWaveResult& OutResult);

} // namespace UE::MimirComposite
