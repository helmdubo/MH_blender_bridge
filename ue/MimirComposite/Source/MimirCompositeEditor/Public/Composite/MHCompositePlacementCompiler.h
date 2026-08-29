#pragma once

#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"

class AActor;
class UActorComponent;
class UMHCompositeSettings;
class USceneComponent;

namespace UE::MimirComposite
{
struct FMHCompositeDefinitionEntry;

struct MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult
{
    TArray<TObjectPtr<UActorComponent>> Components;
    TArray<TObjectPtr<USceneComponent>> TopLevelComponents;
    TArray<TObjectPtr<USceneComponent>> LeafComponents;
    TArray<FString> Warnings;
    FString Error;
    bool Succeeded() const { return Error.IsEmpty(); }
};

/** Reconcile only plan leaves; stable components survive seed-only changes. */
MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult MHCompileCompositePlacementV5(
    AActor& Target, const FMHResolvedCompositePlan& Plan,
    const FMHRandomComposite& RootDefinition, const UMHCompositeSettings& Settings,
    TConstArrayView<TObjectPtr<UActorComponent>> PreviousComponents,
    FMHCompositeDefinitionEntry* Definition = nullptr);

/** No resolution or signature: explicit diagnostics when no applied plan is available. */
MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult MHBuildCompositeDiagnosticView(
    AActor& Target, const FString& Label, const FString& Diagnostic);

/**
 * Instrumentation only: elementary probes issued against the previous component
 * view while compiling a placement. Never read by production logic; the S6.2
 * acceptance test asserts this stays linear in the number of leaves.
 */
MIMIRCOMPOSITEEDITOR_API uint64 MHGetPlacementPreviousComponentProbes();
MIMIRCOMPOSITEEDITOR_API void MHResetPlacementPreviousComponentProbes();

/** Move cached plan matrices without consuming random draws. */
MIMIRCOMPOSITEEDITOR_API bool MHUpdateCompositePlacementBasis(
    AActor& Target, const FMHResolvedCompositePlan& Plan,
    const FMHRandomComposite& RootDefinition,
    TConstArrayView<TObjectPtr<USceneComponent>> Handles,
    TConstArrayView<TObjectPtr<USceneComponent>> Leaves, FString& OutError);
} // namespace UE::MimirComposite
