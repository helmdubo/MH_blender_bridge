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
}
