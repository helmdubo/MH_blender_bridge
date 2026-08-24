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
MIMIRCOMPOSITEEDITOR_API bool MHValidateCompositeClosureV4(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError);

/**
 * Compile to a real component tree. Source transforms are UE world transforms:
 * attach first, then SetWorldTransform, so authored parents never double-apply.
 */
MIMIRCOMPOSITEEDITOR_API FMHCompositeCompileResult MHCompileCompositeV4(
    AActor& Target,
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings);

/** Build/destroy a real transient component tree; import uses this before mutation. */
MIMIRCOMPOSITEEDITOR_API bool MHProbeCompositeBuildV4(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError);

} // namespace UE::MimirComposite
