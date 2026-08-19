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
 * Reads the mandatory Carrier B passport: every MESH Model node must carry the
 * mh_fbx_passport custom property and all copies must match byte-for-byte.
 * Any violation fails with an MH_E_PASSPORT_INVALID-prefixed error.
 */
MIMIRCOMPOSITEEDITOR_API bool MHReadFbxPassport(
    const FString& FilePath,
    FMHFbxPassport& OutPassport,
    FString& OutError);

} // namespace UE::MimirComposite
