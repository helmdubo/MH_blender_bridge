#pragma once

#include "CoreMinimal.h"

class UVolumeTexture;

namespace UE::MimirComposite
{
/** Build the deterministic Windows fallback from Dagor's noiseTex.cpp, before
 * block compression. This is not a copy of an AssetViewer perlin_voltex asset.
 * Caller owns the new texture, compilation barrier and persistence.
 */
MIMIRCOMPOSITEEDITOR_API bool MHBuildDagorWindNoise(UVolumeTexture& Texture, FString& OutError);

/** Require the frozen upstream source bytes and unchanged numeric texture policy. */
MIMIRCOMPOSITEEDITOR_API bool MHValidateDagorWindNoise(const UVolumeTexture& Texture, FString& OutError);
}
