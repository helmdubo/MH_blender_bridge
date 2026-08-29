#pragma once

#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"

class USceneComponent;

namespace UE::MimirComposite
{

/**
 * Materialization side of the S6.3 appearance stage: the resolved channels of a
 * leaf reach its material as Custom Primitive Data floats. Nothing here enters
 * ResolvedSignature, AppearanceSignature or PlacementSignature, and nothing
 * here is read back by the resolver; the plan remains the only authority.
 *
 * Only mesh leaves are transported. A UChildActorComponent has no primitive of
 * its own and the components of the actor it spawns belong to a separate,
 * unratified policy, so actor leaves are deliberately left untouched.
 */
MIMIRCOMPOSITERUNTIME_API bool MHApplyLeafAppearanceCustomData(
    USceneComponent* Component, const FMHResolvedCompositeLeaf& Leaf, int32 BaseIndex);

/**
 * Whole-view variant for the paths that do not create components: the leaf view
 * must agree with the plan exactly, otherwise nothing is written at all and the
 * caller's own rebuild path repairs the desynchronized view. Returns the number
 * of leaves written, or INDEX_NONE when the view was refused.
 */
MIMIRCOMPOSITERUNTIME_API int32 MHApplyCompositeAppearanceCustomData(
    TConstArrayView<TObjectPtr<USceneComponent>> Leaves,
    const FMHResolvedCompositePlan& Plan, int32 BaseIndex);

/** True when BaseIndex..BaseIndex+MH_APPEARANCE_CHANNELS-1 all fit the engine array. */
MIMIRCOMPOSITERUNTIME_API bool MHIsAdmissibleAppearanceCustomDataBaseIndex(int32 BaseIndex);

} // namespace UE::MimirComposite
