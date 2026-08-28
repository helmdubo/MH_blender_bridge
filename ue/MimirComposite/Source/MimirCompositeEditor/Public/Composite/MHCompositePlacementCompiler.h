#pragma once

#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"

class AActor;
class UActorComponent;
class UMHCompositeSettings;
class USceneComponent;
class UStaticMesh;

namespace UE::MimirComposite
{
struct MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult
{
    TArray<TObjectPtr<UActorComponent>> Components;
    TArray<TObjectPtr<USceneComponent>> TopLevelComponents;
    TArray<TObjectPtr<USceneComponent>> NodeComponents;
    TArray<TObjectPtr<USceneComponent>> LeafComponents;
    TArray<FString> Warnings;
    FString Error;
    UStaticMesh* PendingMesh = nullptr;
    bool Succeeded() const { return Error.IsEmpty() && PendingMesh == nullptr; }
};

/** Reconcile only plan leaves; stable components survive seed-only changes. */
MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult MHCompileCompositePlacementV5(
    AActor& Target, const FMHResolvedCompositePlan& Plan,
    const FMHRandomComposite& RootDefinition, const UMHCompositeSettings& Settings,
    TConstArrayView<TObjectPtr<UActorComponent>> PreviousComponents);

/** No resolution or signature: explicit diagnostics when no applied plan is available. */
MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult MHBuildCompositeDiagnosticView(
    AActor& Target, const FString& Label, const FString& Diagnostic);

/** Move cached plan matrices without consuming random draws. */
MIMIRCOMPOSITEEDITOR_API bool MHUpdateCompositePlacementBasis(
    AActor& Target, const FMHResolvedCompositePlan& Plan,
    const FMHRandomComposite& RootDefinition,
    TConstArrayView<TObjectPtr<USceneComponent>> Handles,
    TConstArrayView<TObjectPtr<USceneComponent>> Nodes,
    TConstArrayView<TObjectPtr<USceneComponent>> Leaves, FString& OutError);
} // namespace UE::MimirComposite
