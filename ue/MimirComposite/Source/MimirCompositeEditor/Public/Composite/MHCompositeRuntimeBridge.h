#pragma once

#include "Composite/MHRuntimeCompositeInput.h"
#include "CoreMinimal.h"

class AMHCompositeActor;
class UWorld;

namespace UE::MimirComposite
{

/** Applied-only, seed-free snapshot and hard bindings for every source-closure endpoint. */
MIMIRCOMPOSITEEDITOR_API bool MHBuildRuntimeCompositeInput(
    const AMHCompositeActor& Placement,
    FMHRuntimeCompositeInput& OutInput,
    FString& OutError);

/** Read-only whole-world handoff admission, also used by Automation failure probes. */
MIMIRCOMPOSITEEDITOR_API bool MHValidateRuntimeCompositeWorld(UWorld& World, FString& OutError);

/** True only during the cooked-save overlay that admitted this exact placement. */
MIMIRCOMPOSITEEDITOR_API bool MHIsRuntimeCompositeCookPrepared(const AMHCompositeActor& Placement);

/** Module-owned hooks; no persisted companion actors or source-tree writes. */
void MHStartupRuntimeCompositeBridge();
void MHShutdownRuntimeCompositeBridge();

} // namespace UE::MimirComposite
