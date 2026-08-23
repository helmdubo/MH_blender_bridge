#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

namespace UE::MimirComposite
{

struct MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult
{
	bool bSuccess = false;
	FString Error;

	static FMHCanonicalResult Success();
	static FMHCanonicalResult Failure(FString InError);
};

struct MIMIRCOMPOSITERUNTIME_API FMHTexturePathResult
{
	bool bSuccess = false;
	bool bExternal = false;
	FString Path;
	FString Error;
};

/** Parse JSON while preserving the original spelling of every number. */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHParseJsonUtf8(
	TConstArrayView<uint8> Bytes,
	TSharedPtr<FJsonValue>& OutValue);

/** Source Schema v1: binary64 multiplication followed by round-half-even. */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHQuantize(
	double Value,
	int32 Precision,
	int64& OutQuantized);

/** Normalize, p=6 quantize, then sign-canonicalize an xyzw quaternion. */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHCanonicalizeQuaternion(
	TConstArrayView<double> Quaternion,
	TArray<int64>& OutQuaternion);

/** Normalize a string to Unicode NFC. Fails closed when ICU is unavailable. */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHNormalizeNfc(
	FStringView Input,
	FString& OutNormalized);

/** Serialize an integer-only canonical value tree as compact UTF-8 JSON. */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHCanonicalJsonBytes(
	const TSharedPtr<FJsonValue>& Value,
	TArray<uint8>& OutBytes);

/** Canonicalize an object supplied as pairs, preserving case-distinct JSON keys. */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHCanonicalJsonObjectPairsBytes(
	TConstArrayView<TPair<FString, TSharedPtr<FJsonValue>>> Pairs,
	TArray<uint8>& OutBytes);

/** Build the normalized on-disk and integer-only canonical material payloads. */
MIMIRCOMPOSITERUNTIME_API FMHCanonicalResult MHMaterialForms(
	FStringView ShaderClass,
	const TSharedPtr<FJsonValue>& Params,
	const TSharedPtr<FJsonValue>& Textures,
	TSharedPtr<FJsonValue>& OutDisk,
	TSharedPtr<FJsonValue>& OutCanonical);

/** XXH3-64 over bytes, formatted as xxh3 plus sixteen lowercase hex digits. */
MIMIRCOMPOSITERUNTIME_API FString MHXxh3Hash(TConstArrayView<uint8> Bytes);

/** Canonicalize an already expanded authored texture path per frozen v1 section 5.3. */
MIMIRCOMPOSITERUNTIME_API FMHTexturePathResult MHCanonicalizeTexturePath(
	FStringView AuthoredPath,
	FStringView SourceRoot);

/** Validate an on-disk texture path without repairing it. */
MIMIRCOMPOSITERUNTIME_API FMHTexturePathResult MHValidateDiskTexturePath(
	FStringView DiskPath,
	FStringView SourceRoot);

} // namespace UE::MimirComposite
