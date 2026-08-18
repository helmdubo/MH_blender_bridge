#pragma once

#include "Canonical/MHCanonical.h"
#include "Composite/MHCompositeTypes.h"
#include "CoreMinimal.h"

namespace UE::MimirComposite
{

/** Parsed image of one strict mh.composite v2 payload. */
struct MIMIRCOMPOSITERUNTIME_API FMHCompositeDocument
{
	FString Uid;
	FString Name;
	FString ResourcePropertiesJson;
	TArray<FMHCompositeNode> Nodes;
};

/**
 * Strict Clean Sources v2 reader for mh.composite. Fails closed with an
 * MH_E_*-prefixed error on any schema violation; a valid v1 spelling fails
 * with the MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED marker so the caller
 * can exclude it from the production candidate set without guessing.
 */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHParseCompositeV2(
	TConstArrayView<uint8> Bytes,
	FMHCompositeDocument& OutDocument);

/** Shared identity helpers for payload codecs. */
MIMIRCOMPOSITERUNTIME_API bool MHIsCanonicalUuid(const FString& Value);
MIMIRCOMPOSITERUNTIME_API bool MHIsValidResourceName(const FString& Value);

} // namespace UE::MimirComposite
