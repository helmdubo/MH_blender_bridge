#pragma once

#include "CoreMinimal.h"

class UTypedElementSelectionSet;

namespace UE::MimirComposite
{

/**
 * Viewport selection seam of the instance pool (16 §2.8, R5b-2b): a hit on a
 * pooled ISM instance resolves, through the pool's ReverseLookup, to the
 * owning AMHCompositeActor's element - never to the service pool actor. Stock
 * ISM instances keep the level editor's behaviour (instance -> component ->
 * owning actor). Registered on the level editor's element selection set;
 * independent of the Composite Outliner widget.
 */
MIMIRCOMPOSITEEDITOR_API bool MHRegisterPoolInstanceSelection(UTypedElementSelectionSet& SelectionSet);
MIMIRCOMPOSITEEDITOR_API bool MHIsPoolInstanceSelectionRegistered(const UTypedElementSelectionSet& SelectionSet);

} // namespace UE::MimirComposite
