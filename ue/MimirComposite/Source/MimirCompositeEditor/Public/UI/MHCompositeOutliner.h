#pragma once

#include "CoreMinimal.h"

class SWidget;

namespace UE::MimirComposite
{

MIMIRCOMPOSITEEDITOR_API extern const FName MHCompositeOutlinerTabName;

/** Register/unregister the plugin-owned dockable read-only outliner tab. */
MIMIRCOMPOSITEEDITOR_API void MHRegisterCompositeOutliner();
MIMIRCOMPOSITEEDITOR_API void MHUnregisterCompositeOutliner();
MIMIRCOMPOSITEEDITOR_API void MHOpenCompositeOutliner();

/** Create the same Outliner content used by the nomad tab, for alternate Slate hosts. */
MIMIRCOMPOSITEEDITOR_API TSharedRef<SWidget> MHCreateCompositeOutlinerWidget();

} // namespace UE::MimirComposite
