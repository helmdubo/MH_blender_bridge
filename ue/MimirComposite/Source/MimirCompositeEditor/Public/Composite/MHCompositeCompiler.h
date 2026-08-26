#pragma once

#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"

class AActor;
class UActorComponent;
class UMHCompositeSettings;

namespace UE::MimirComposite
{

class IMHSourceResolver;

struct MIMIRCOMPOSITEEDITOR_API FMHCompositeCompileResult
{
    TArray<TObjectPtr<UActorComponent>> Components;
    FString Error;

    bool Succeeded() const { return Error.IsEmpty(); }
};

/** Resolve the complete source closure and all generated mesh/actor endpoints. */
MIMIRCOMPOSITEEDITOR_API bool MHValidateCompositeClosureV5(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError);

/**
 * Compile to a real component tree using parent-local transforms. Random/profile
 * materialization remains fail-closed until the V5-S5 resolved-plan consumer.
 */
MIMIRCOMPOSITEEDITOR_API FMHCompositeCompileResult MHCompileCompositeV5(
    AActor& Target,
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings);

/** Build/destroy a real transient component tree; import uses this before mutation. */
MIMIRCOMPOSITEEDITOR_API bool MHProbeCompositeBuildV5(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError);

} // namespace UE::MimirComposite
