#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

struct MIMIRCOMPOSITEEDITOR_API FMHFbxPassportSlot
{
    FString SlotName;
    FString MaterialUid;
    FString MaterialNameHint;
};

/** Validated mh.fbx_passport v1 document read from Carrier B consensus. */
struct MIMIRCOMPOSITEEDITOR_API FMHFbxPassport
{
    FString ResourceUid;
    FString Name;
    FString LodPolicy;
    TArray<int32> LodLevels;
    FString GeometryHash;
    TArray<FMHFbxPassportSlot> MaterialSlots;
    FString PropertiesJson;
    FString Exporter;

    /** Raw carrier text shared by every MESH Model node. */
    FString CarrierText;
    int32 CopyCount = 0;
};

/**
 * Validates one Carrier B value without opening an FBX file. The text must be
 * byte-for-byte equal to Python canonical_passport(document), including NFC,
 * key order, compact separators, JSON escaping, and number spelling.
 */
MIMIRCOMPOSITEEDITOR_API bool MHParseFbxPassportText(
    const FString& CarrierText,
    FMHFbxPassport& OutPassport,
    FString& OutError);

/** Builds the exact Python descriptor_hash byte input (geometry_hash omitted). */
MIMIRCOMPOSITEEDITOR_API bool MHBuildFbxPassportDescriptorBytes(
    const FMHFbxPassport& Passport,
    TArray<uint8>& OutBytes,
    FString& OutError);

/**
 * Reads the mandatory Carrier B passport: every MESH Model node must carry the
 * mh_fbx_passport custom property and all copies must match byte-for-byte.
 * Any violation fails with an MH_E_PASSPORT_INVALID-prefixed error.
 */
MIMIRCOMPOSITEEDITOR_API bool MHReadFbxPassport(
    const FString& FilePath,
    FMHFbxPassport& OutPassport,
    FString& OutError);

} // namespace UE::MimirComposite
