#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceResolver.h"

class AActor;
class UActorComponent;
class UMHCompositeAsset;
class UMHCompositeSettings;
class USceneComponent;

namespace UE::MimirComposite
{

/** Tolerant generated-asset view described by §6.1; strict import probing is separate. */
struct MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult
{
    TArray<TObjectPtr<UActorComponent>> Components;
    TArray<TObjectPtr<USceneComponent>> TopLevelComponents;
    TSet<FMHResourceKey> Dependencies;
    TArray<FString> Warnings;
    FString Error;

    bool Succeeded() const { return Error.IsEmpty(); }
};

/**
 * Compile one managed asset into a transient placement view. Missing generated
 * endpoints become visible placeholders and remain in Dependencies so a
 * same-name resource notification can heal the view.
 */
MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult MHCompileCompositePlacementV5(
    AActor& Target,
    const UMHCompositeAsset* Asset,
    const FString& ExpectedLogicalName,
    const UMHCompositeSettings& Settings);

} // namespace UE::MimirComposite
