#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UMaterialFunction;
class UMaterialParameterCollection;
class UTexture2D;
class UVolumeTexture;

namespace UE::MimirComposite
{
/** Build the common material function. Caller owns new assets and persistence. */
MIMIRCOMPOSITEEDITOR_API bool MHBuildPivotWindFunction(
    UMaterialFunction& Function, UMaterialParameterCollection& Collection,
    UTexture2D& DefaultPosition, UTexture2D& DefaultDirection,
    UVolumeTexture& NoiseVolume, FString& OutError);

/** Validate generated v2 node topology, bindings and response defaults before reuse. */
MIMIRCOMPOSITEEDITOR_API bool MHValidatePivotWindFunction(
    const UMaterialFunction& Function, const UMaterialParameterCollection& Collection,
    const UTexture2D& DefaultPosition, const UTexture2D& DefaultDirection,
    const UVolumeTexture& NoiseVolume, FString& OutError);

/** Read-only admission for the complete original v1 graph, including its frozen HLSL. */
MIMIRCOMPOSITEEDITOR_API bool MHValidateLegacyPivotWindFunction(
    const UMaterialFunction& Function, const UMaterialParameterCollection& Collection,
    const UTexture2D& DefaultPosition, const UTexture2D& DefaultDirection,
    FString& OutError);

/** Explicit v1 -> v2 upgrade. Validates before mutation, retains existing parameters
 * and output connector IDs. Caller protects every consumer with FMaterialUpdateContext
 * before calling, recompiles those consumers afterwards, and owns persistence.
 */
MIMIRCOMPOSITEEDITOR_API bool MHUpgradePivotWindFunction(
    UMaterialFunction& Function, UMaterialParameterCollection& Collection,
    UTexture2D& DefaultPosition, UTexture2D& DefaultDirection,
    UVolumeTexture& NoiseVolume, FString& OutError);

/** Attach wind to a conventional surface material, preserving its surface inputs.
 * Refuses an unrelated WPO graph or material-attributes root. Repeated attachment
 * of the same function is a validated no-op. Caller establishes FMaterialUpdateContext.
 */
MIMIRCOMPOSITEEDITOR_API bool MHAttachPivotWind(
    UMaterial& Material, UMaterialFunction& Function, FString& OutError);

/** Validate the material-side wind/normal wiring and required rendering flags before reuse. */
MIMIRCOMPOSITEEDITOR_API bool MHValidatePivotWindAttachment(
    const UMaterial& Material, const UMaterialFunction& Function, FString& OutError);
}
