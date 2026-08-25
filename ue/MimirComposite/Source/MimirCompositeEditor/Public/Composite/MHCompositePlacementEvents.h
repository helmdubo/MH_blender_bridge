#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceResolver.h"

class UMHCompositeAsset;

namespace UE::MimirComposite
{

/** Rebuild loaded placement views which observed this generated resource key. */
MIMIRCOMPOSITEEDITOR_API void MHNotifyGeneratedResourceChanged(const FMHResourceKey& Key);

/** Compatibility helper for callers which already hold the committed composite asset. */
MIMIRCOMPOSITEEDITOR_API void MHNotifyCompositeAssetChanged(UMHCompositeAsset& Asset);

} // namespace UE::MimirComposite
