#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace MH::FbxDump
{
inline constexpr int32 QuantizationDigits = 6;

/**
 * Reads an FBX scene without axis/unit conversion, transform evaluation, or
 * geometry mutation and returns the mh.fbxdump:1 JSON document.
 */
MIMIRCOMPOSITEEDITOR_API bool BuildDom(
    const FString& FilePath,
    bool bFull,
    TSharedPtr<FJsonObject>& OutRoot,
    FString& OutError);

/** Serializes a dump DOM as the canonical one-line JSON string. */
MIMIRCOMPOSITEEDITOR_API bool SerializeCanonical(
    const TSharedPtr<FJsonObject>& Root,
    FString& OutJson,
    FString& OutError);
}
