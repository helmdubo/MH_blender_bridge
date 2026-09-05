#include "Composite/MHMaterializeLayout.h"

#include "Composite/MHCompositeTransformAdmission.h"

namespace UE::MimirComposite
{

namespace
{

FMHResourceKey GraphKey(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = Kind;
    Key.LogicalName = Name;
    return Key;
}

} // namespace

void MHCollectRecipeGraphDependencies(const FMHRandomSourceGraph& Graph, TSet<FMHResourceKey>& OutDependencies)
{
    TFunction<void(const FMHRandomNode&)> Visit = [&](const FMHRandomNode& Node)
    {
        if (Node.Kind == EMHRandomSemanticKind::Mesh) OutDependencies.Add(GraphKey(EMHResourceKind::StaticMesh, Node.Resource));
        if (!Node.Profile.IsEmpty()) OutDependencies.Add(GraphKey(EMHResourceKind::PlacementProfile, Node.Profile));
        for (const FMHRandomOption& Option : Node.Options)
        {
            if (Option.Kind == EMHRandomSemanticKind::Mesh) OutDependencies.Add(GraphKey(EMHResourceKind::StaticMesh, Option.Resource));
        }
        for (const FMHRandomNode& Child : Node.Children) Visit(Child);
    };
    for (const TPair<FString, FMHRandomComposite>& Pair : Graph.Composites)
    {
        OutDependencies.Add(GraphKey(EMHResourceKind::Composite, Pair.Key));
        for (const FMHRandomNode& Node : Pair.Value.Nodes) Visit(Node);
    }
}

FMHMaterializeResult MHMaterializeLayout(
    const FMHCompiledRecipe& Recipe,
    const int32 Seed,
    const int32 AppearanceSeed,
    const FTransform& ActorTransform)
{
    return MHMaterializeLayout(Recipe, Seed, AppearanceSeed, FMHResolveCallContext(), ActorTransform);
}

FMHMaterializeResult MHMaterializeLayout(
    const FMHCompiledRecipe& Recipe,
    const int32 Seed,
    const int32 AppearanceSeed,
    const FMHResolveCallContext& Context,
    const FTransform& ActorTransform)
{
    // Preview plane (§2.5): recipe graph -> Layout -> Appearance -> transform
    // admission -> world-space leaves. No asset load, no spawn, no world read,
    // no closure, no signature. Errors keep the placement compiler's codes.
    FMHMaterializeResult Result;
    Result.Seed = Seed;
    Result.AppearanceSeed = AppearanceSeed;
    const TSharedRef<FMHRandomSourceGraph> Graph = MakeShared<FMHRandomSourceGraph>();
    const TSharedRef<FMHResolvedCompositePlan> Plan = MakeShared<FMHResolvedCompositePlan>();
    FString Error;
    if (!MHBuildRecipeGraph(Recipe, *Graph, Error))
    {
        Result.Error = Error.StartsWith(TEXT("MH_E_")) ? Error : TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: ") + Error;
        return Result;
    }
    // The graph is reported even when layout fails: its resources are what a
    // caller must watch to retry once a missing dependency appears.
    Result.Graph = Graph;
    if (!MHResolvePreviewGraph(*Graph, Seed, AppearanceSeed, Context, *Plan, Error))
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
