#pragma once

#include "CoreMinimal.h"

class AMHCompositeActor;

namespace UE::MimirComposite
{

enum class EMHCompositeSeedTarget : uint8
{
    Layout,
    Appearance
};

/** Execute one Details-panel seed action for the complete edited selection. */
MIMIRCOMPOSITEEDITOR_API bool MHGenerateCompositeSeedsForDetails(
    TConstArrayView<TWeakObjectPtr<AMHCompositeActor>> Actors,
    EMHCompositeSeedTarget Target,
    FString& OutError);

/** Register the editor-only AMHCompositeActor Details customization. */
MIMIRCOMPOSITEEDITOR_API void MHRegisterCompositeActorDetails();

/** Release the editor-only AMHCompositeActor Details customization. */
MIMIRCOMPOSITEEDITOR_API void MHUnregisterCompositeActorDetails();

} // namespace UE::MimirComposite
