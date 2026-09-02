#pragma once

#include "Composite/MHCompiledRecipe.h"
#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"

namespace UE::MimirComposite
{

/** One leaf of a materialized layout, in world space (actor transform applied). */
struct MIMIRCOMPOSITEEDITOR_API FMHLeafPlacement
{
    EMHRandomSemanticKind Kind = EMHRandomSemanticKind::Empty;
    FString Resource;
    /** Canonical "kind:name" key of the endpoint (static_mesh:, actor:, gameobj:). */
    FString ResourceKey;
    /** Leaf origin NodePath in the resolver grammar. */
    FString NodePath;
    /** §2.7 identity for NodeOverrides; zero until R6. */
    uint64 NodeFingerprint = 0;
    /** Leaf matrix in root-placement space times the actor transform. */
    FMatrix WorldMatrix = FMatrix::Identity;
    float AppearanceChannels[MH_APPEARANCE_CHANNELS] = {};
    /** Set by NodeOverrides from R6 on. */
    bool bOverridden = false;
    FString DisplayName;
    /** Authored top-level handle of the recipe root this leaf descends from. */
    int32 RootNodeIndex = INDEX_NONE;
    int32 OwningResolvedNodeIndex = INDEX_NONE;
    FString AppearanceBoundaryPath;
};

struct MIMIRCOMPOSITEEDITOR_API FMHMaterializeResult
{
    bool Succeeded() const { return Error.IsEmpty(); }

    FString Error;
    int32 Seed = 0;
    int32 AppearanceSeed = 0;
    TArray<FMHLeafPlacement> Placements;
    TArray<FString> Warnings;
    /**
     * Resident layout + appearance plan behind the placements (Outliner,
     * reseed diff). Never carries a closure or a signature: preview plane.
     */
    TSharedPtr<const FMHResolvedCompositePlan> Plan;
    /** Preview graph the plan was resolved on (recipe composites, no RawHashes). */
    TSharedPtr<const FMHRandomSourceGraph> Graph;
};

/** Graph resources a materialized recipe observes: composites, meshes, profiles. */
MIMIRCOMPOSITEEDITOR_API void MHCollectRecipeGraphDependencies(
    const FMHRandomSourceGraph& Graph,
    TSet<FMHResourceKey>& OutDependencies);

/**
 * Recipe Model v2 §2.5. Pure function over a compiled recipe: no asset load,
 * no spawn, no world read, no hash. Layout + Appearance on the recipe graph,
 * transform admission against ActorTransform, leaves in world space.
 * NodeOverrides (R6) are added as a further input when they exist.
 */
MIMIRCOMPOSITEEDITOR_API FMHMaterializeResult MHMaterializeLayout(
    const FMHCompiledRecipe& Recipe,
    int32 Seed,
    int32 AppearanceSeed,
    const FTransform& ActorTransform);

} // namespace UE::MimirComposite
