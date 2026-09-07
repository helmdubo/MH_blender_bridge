#pragma once

#include "CoreMinimal.h"

class SWidget;

namespace UE::MimirComposite
{

/** Create the Composite Outliner content for an editor toolkit host. */
MIMIRCOMPOSITEEDITOR_API TSharedRef<SWidget> MHCreateCompositeOutlinerWidget();

} // namespace UE::MimirComposite
