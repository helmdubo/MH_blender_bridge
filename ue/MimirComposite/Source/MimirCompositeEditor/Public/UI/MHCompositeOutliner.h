#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

MIMIRCOMPOSITEEDITOR_API extern const FName MHCompositeOutlinerTabName;

/** Register/unregister the plugin-owned dockable read-only outliner tab. */
MIMIRCOMPOSITEEDITOR_API void MHRegisterCompositeOutliner();
MIMIRCOMPOSITEEDITOR_API void MHUnregisterCompositeOutliner();
MIMIRCOMPOSITEEDITOR_API void MHOpenCompositeOutliner();

} // namespace UE::MimirComposite
