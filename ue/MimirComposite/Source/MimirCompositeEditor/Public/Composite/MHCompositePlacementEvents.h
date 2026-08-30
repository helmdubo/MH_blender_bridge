#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceResolver.h"

class UMHCompositeAsset;

namespace UE::MimirComposite
{

/** Rebuild loaded placement views which observed this generated resource key. */
MIMIRCOMPOSITEEDITOR_API void MHNotifyGeneratedResourceChanged(const FMHResourceKey& Key);

/** Rebuild every loaded placement view after the startup source pass settles. */
MIMIRCOMPOSITEEDITOR_API int32 MHRebuildAllLoadedCompositeActors();

/** Compatibility helper for callers which already hold the committed composite asset. */
MIMIRCOMPOSITEEDITOR_API void MHNotifyCompositeAssetChanged(UMHCompositeAsset& Asset);

#if WITH_DEV_AUTOMATION_TESTS
/** Test-only observation of the existing generated-resource notification funnel. */
MIMIRCOMPOSITEEDITOR_API void MHSetGeneratedResourceChangedObserverForTests(
    TFunction<void(const FMHResourceKey&)> Observer);
#endif

} // namespace UE::MimirComposite
