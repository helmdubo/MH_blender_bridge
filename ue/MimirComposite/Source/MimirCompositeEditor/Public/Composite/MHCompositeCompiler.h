#pragma once

#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"

class UMHCompositeSettings;

namespace UE::MimirComposite
{

class IMHSourceResolver;

/** Seed-free admission of the complete source closure and all generated mesh/actor endpoints. */
MIMIRCOMPOSITEEDITOR_API bool MHValidateCompositeClosureV5(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError);

/**
 * Import pre-mutation admission alias. A definition has no placement Seed, so
 * this validates every source option/profile without resolving or spawning.
 * All component materialization belongs to the resolved-plan consumer.
 */
MIMIRCOMPOSITEEDITOR_API bool MHProbeCompositeBuildV5(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError);

} // namespace UE::MimirComposite
