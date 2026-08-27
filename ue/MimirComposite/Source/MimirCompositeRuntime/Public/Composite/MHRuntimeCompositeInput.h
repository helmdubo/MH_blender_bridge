#pragma once

#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"
#include "UObject/ObjectPtr.h"
#include "MHRuntimeCompositeInput.generated.h"

/** Hard cook references for the complete source closure, never just selected leaves. */
USTRUCT()
struct MIMIRCOMPOSITERUNTIME_API FMHRuntimeCompositeBinding
{
    GENERATED_BODY()

    /** Canonical resource key, or actor:<ActorClassRegistry token>. */
    UPROPERTY()
    FString ResourceKey;

    UPROPERTY()
    TObjectPtr<UObject> Object = nullptr;
};

/**
 * Seed-free cooked transport for the existing resolver input. This is an internal
 * UObject serialization carrier, not a new source protocol or a resolved plan.
 * Profile raw-hash values are copied into GraphBytes; no source tree is read.
 */
USTRUCT()
struct MIMIRCOMPOSITERUNTIME_API FMHRuntimeCompositeInput
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<uint8> GraphBytes;

    /** Includes unselected and zero-weight variants, their dependencies, and actor classes. */
    UPROPERTY()
    TArray<FMHRuntimeCompositeBinding> Bindings;
};

namespace UE::MimirComposite
{

/** Deterministic internal binary transport; arrays keep source order, maps are sorted. */
MIMIRCOMPOSITERUNTIME_API bool MHEncodeRuntimeCompositeGraph(
    const FMHRandomSourceGraph& Graph,
    TArray<uint8>& OutBytes,
    FString& OutError);

/** Strict bounded read; output is changed only after complete admission. */
MIMIRCOMPOSITERUNTIME_API bool MHDecodeRuntimeCompositeGraph(
    TConstArrayView<uint8> Bytes,
    FMHRandomSourceGraph& OutGraph,
    FString& OutError);

/** Seed-free admission and sorted endpoint keys for ALL reachable source options. */
MIMIRCOMPOSITERUNTIME_API bool MHCollectRuntimeCompositeBindingKeys(
    const FMHRandomSourceGraph& Graph,
    TArray<FString>& OutKeys,
    FString& OutError);

/** Exact complete binding set; rejects duplicates, missing/extra keys and uncookable types. */
MIMIRCOMPOSITERUNTIME_API bool MHValidateRuntimeCompositeBindings(
    const FMHRandomSourceGraph& Graph,
    TConstArrayView<FMHRuntimeCompositeBinding> Bindings,
    FString& OutError);

} // namespace UE::MimirComposite
