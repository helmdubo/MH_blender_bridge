#include "Composite/MHMaterializeLayout.h"

#include "Composite/MHCompositeTransformAdmission.h"

namespace UE::MimirComposite
{

FMHMaterializeResult MHMaterializeLayout(
    const FMHCompiledRecipe& Recipe,
    const int32 Seed,
    const int32 AppearanceSeed,
    const FTransform& ActorTransform)
{
    // Preview plane (§2.5): recipe graph -> Layout -> Appearance -> transform
    // admission -> world-space leaves. No asset load, no spawn, no world read,
    // no closure, no signature. Errors keep the placement compiler's codes.
    FMHMaterializeResult Result;
    Result.Seed = Seed;
    Result.AppearanceSeed = AppearanceSeed;
    const TSharedRef<FMHResolvedCompositePlan> Plan = MakeShared<FMHResolvedCompositePlan>();
    FString Error;
    if (!MHResolveRecipePreview(Recipe, Seed, AppearanceSeed, *Plan, Error))
    {
        Result.Error = Error.StartsWith(TEXT("MH_E_")) ? Error : TEXT("MH_E_COMPOSITE_GRAMMAR: ") + Error;
        return Result;
    }
    if (!MHValidateResolvedPlacementTransforms(*Plan, ActorTransform, Error))
    {
        Result.Error = Error;
        return Result;
    }
    const FMatrix ActorMatrix = ActorTransform.ToMatrixWithScale();
    Result.Placements.Reserve(Plan->Leaves.Num());
    for (const FMHResolvedCompositeLeaf& Leaf : Plan->Leaves)
    {
        FMHLeafPlacement& Placement = Result.Placements.AddDefaulted_GetRef();
        Placement.Kind = Leaf.Kind;
        Placement.Resource = Leaf.Resource;
        Placement.ResourceKey = MHRecipeResourceKey(Leaf.Kind, Leaf.Resource);
        Placement.NodePath = Leaf.Origin;
        Placement.WorldMatrix = Leaf.WorldMatrix * ActorMatrix;
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        {
            Placement.AppearanceChannels[Channel] = Leaf.AppearanceChannels[Channel];
        }
        Placement.DisplayName = Leaf.DisplayName;
        Placement.RootNodeIndex = Leaf.RootNodeIndex;
        Placement.OwningResolvedNodeIndex = Leaf.OwningResolvedNodeIndex;
        Placement.AppearanceBoundaryPath = Leaf.AppearanceBoundaryPath;
    }
    Result.Plan = Plan;
    return Result;
}

} // namespace UE::MimirComposite
