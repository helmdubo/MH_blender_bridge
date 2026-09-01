#pragma once

#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"
#include "UObject/ObjectPtr.h"

class AActor;
class UActorComponent;
class UInstancedStaticMeshComponent;
class UMHCompositeSettings;
class USceneComponent;

namespace UE::MimirComposite
{
struct FMHCompositeDefinitionEntry;

/**
 * Plan-aligned editor materialization row. Static leaves may share Component;
 * InstanceIndex then identifies the exact ISM instance. Non-static leaves and
 * the one leaf extracted for Placement Edit Mode use INDEX_NONE.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeLeafMaterialization
{
    TObjectPtr<USceneComponent> Component = nullptr;
    int32 InstanceIndex = INDEX_NONE;
    int32 ResolvedNodeIndex = INDEX_NONE;
    FString NodePath;

    bool IsInstanced() const { return InstanceIndex != INDEX_NONE; }
};

struct MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult
{
    TArray<TObjectPtr<UActorComponent>> Components;
    TArray<TObjectPtr<USceneComponent>> TopLevelComponents;
    TArray<TObjectPtr<USceneComponent>> LeafComponents;
    TArray<FMHCompositeLeafMaterialization> LeafMaterializations;
    TArray<FString> Warnings;
    FString Error;
    bool Succeeded() const { return Error.IsEmpty(); }
};

/** Reconcile only plan leaves; stable components survive seed-only changes. */
MIMIRCOMPOSITEEDITOR_API FMHCompositePlacementCompileResult MHCompileCompositePlacementV5(
    AActor& Target, const FMHResolvedCompositePlan& Plan,
    const FMHRandomComposite& RootDefinition, const UMHCompositeSettings& Settings,
    TConstArrayView<TObjectPtr<UActorComponent>> PreviousComponents,
    FMHCompositeDefinitionEntry* Definition = nullptr,
    const FString& UninstancedLeafPath = FString());

/**
 * Attempt a seed-only reconciliation against a strictly validated prior view.
 * Returns false without mutation when structural admission fails, so the caller
 * can run the unchanged full compiler. A true return may still carry the same
 * endpoint error that the full compiler would report.
 */
MIMIRCOMPOSITEEDITOR_API bool MHTryCompileCompositePlacementReseedV5(
    AActor& Target, const FMHResolvedCompositePlan& PreviousPlan,
    const FMHResolvedCompositePlan& CandidatePlan,
    const FMHRandomComposite& RootDefinition, const UMHCompositeSettings& Settings,
    TConstArrayView<TObjectPtr<UActorComponent>> PreviousComponents,
    TConstArrayView<TObjectPtr<USceneComponent>> PreviousHandles,
    TConstArrayView<TObjectPtr<USceneComponent>> PreviousLeaves,
    TConstArrayView<FMHCompositeLeafMaterialization> PreviousMaterializations,
    FMHCompositeDefinitionEntry* Definition,
    FMHCompositePlacementCompileResult& OutResult,
    const FString& UninstancedLeafPath = FString());

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
    TConstArrayView<TObjectPtr<USceneComponent>> Leaves,
    TConstArrayView<FMHCompositeLeafMaterialization> Materializations,
    FString& OutError);

/** Appearance-only fast path over ordinary leaves and ISM custom data. */
MIMIRCOMPOSITEEDITOR_API int32 MHApplyCompositePlacementAppearance(
    TConstArrayView<FMHCompositeLeafMaterialization> Materializations,
    const FMHResolvedCompositePlan& Plan, int32 BaseIndex);
} // namespace UE::MimirComposite
