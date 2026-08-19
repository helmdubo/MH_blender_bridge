#pragma once

#include "Canonical/MHCanonical.h"
#include "CoreMinimal.h"

namespace UE::MimirComposite
{

/** Parsed image of one strict mh.material v1 payload. */
struct MIMIRCOMPOSITERUNTIME_API FMHMaterialDocument
{
	FString Uid;
	FString Name;
	FString ShaderClass;
	FString ParamsJson;
	FString TexturesJson;
};

/** Strict mh.material v1 reader; fails closed with MH_E_*-prefixed errors. */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHParseMaterialV1(
	TConstArrayView<uint8> Bytes,
	FMHMaterialDocument& OutDocument);

} // namespace UE::MimirComposite
