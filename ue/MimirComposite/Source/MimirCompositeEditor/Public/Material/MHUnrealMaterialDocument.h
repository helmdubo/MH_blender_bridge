#pragma once

#include "CoreMinimal.h"
#include "Material/MHMaterialProtocol.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "Materials/MaterialParameters.h"
#include "StaticParameterSet.h"
#include "UObject/SoftObjectPath.h"

class FJsonObject;
class UMaterialInstanceConstant;
class UMaterialInterface;

namespace UE::MimirComposite
{

/** UE 5.7 local instance state. Referenced assets remain explicit project dependencies. */
struct MIMIRCOMPOSITEEDITOR_API FMHUnrealMaterialInstanceData
{
    FSoftObjectPath Parent;
    TArray<TPair<FMaterialParameterInfo, float>> Scalars;
    TArray<TPair<FMaterialParameterInfo, FLinearColor>> Vectors;
    TArray<TPair<FMaterialParameterInfo, FSoftObjectPath>> Textures;
    TArray<FStaticSwitchParameter> StaticSwitches;
    TArray<FStaticComponentMaskParameter> StaticComponentMasks;
    FMaterialInstanceBasePropertyOverrides BaseOverrides;
};

/** Capture supported local state exactly; reject unsupported state before publication. */
MIMIRCOMPOSITEEDITOR_API bool MHExtractUnrealMaterialV1(
    const UMaterialInstanceConstant& Material, FMHMaterialDocument& OutDocument, FString& OutError);

/** Resolve every asset and validate ancestry before replacing the target state. */
MIMIRCOMPOSITEEDITOR_API bool MHApplyUnrealMaterialV1(
    UMaterialInstanceConstant& Material, UMaterialInterface& Parent,
    const FMHMaterialDocument& Document, FString& OutError);

MIMIRCOMPOSITEEDITOR_API bool MHParseUnrealMaterialV1(
    const TSharedPtr<FJsonObject>& Payload, FMHMaterialDocument& OutDocument, FString& OutError);

/** Sorted object keys and parameter identities, UTF-8, 2-space indent, LF and final LF. */
MIMIRCOMPOSITEEDITOR_API bool MHWriteUnrealMaterialV1(
    const FMHMaterialDocument& Document, TArray<uint8>& OutBytes, FString& OutError);

} // namespace UE::MimirComposite
