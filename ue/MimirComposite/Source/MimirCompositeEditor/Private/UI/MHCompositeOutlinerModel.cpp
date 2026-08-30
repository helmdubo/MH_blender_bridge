#include "UI/MHCompositeOutlinerModel.h"

#include "Composite/MHCompositeActor.h"

namespace UE::MimirComposite
{

FMHCompositeOutlinerModel::FMHCompositeOutlinerModel(FAssetResolver InAssetResolver)
    : AssetResolver(MoveTemp(InAssetResolver))
{
}

bool FMHCompositeOutlinerModel::Build(
    const UMHCompositeAsset& InRootAsset,
    const FMHResolvedCompositePlan*,
    const FString& PlanUnavailableReason)
{
    RootAsset = const_cast<UMHCompositeAsset*>(&InRootAsset);
    Roots.Reset();
    ItemsByPath.Reset();
    ItemsByComponent.Reset();
    OverlayStatus = PlanUnavailableReason;
    return false;
}

bool FMHCompositeOutlinerModel::BuildFromActor(AMHCompositeActor& Actor)
{
    const UMHCompositeAsset* Asset = Actor.GetCompositeAsset();
    return Asset != nullptr && Build(*Asset, Actor.GetResolvedPlan(), Actor.GetLastPlacementError());
}

bool FMHCompositeOutlinerModel::RefreshOverlay(
    const FMHResolvedCompositePlan*,
    const FString& PlanUnavailableReason)
{
    OverlayStatus = PlanUnavailableReason;
    return false;
}

bool FMHCompositeOutlinerModel::ExpandItem(const TSharedPtr<FMHCompositeOutlinerItem>&)
{
    return false;
}

TSharedPtr<FMHCompositeOutlinerItem> FMHCompositeOutlinerModel::FindByNodePath(
    const FString& NodePath) const
{
    const TSharedPtr<FMHCompositeOutlinerItem>* Found = ItemsByPath.Find(NodePath);
    return Found != nullptr ? *Found : nullptr;
}

TSharedPtr<FMHCompositeOutlinerItem> FMHCompositeOutlinerModel::FindForComponent(
    const USceneComponent* Component) const
{
    const TWeakPtr<FMHCompositeOutlinerItem>* Found = ItemsByComponent.Find(Component);
    return Found != nullptr ? Found->Pin() : nullptr;
}

FString FMHCompositeOutlinerModel::GetCopyName(const FMHCompositeOutlinerItem& Item) const
{
    return Item.Label;
}

bool FMHCompositeOutlinerModel::GetNavigation(
    const FMHCompositeOutlinerItem&,
    const FString&,
    FMHCompositeOutlinerNavigation& OutNavigation) const
{
    OutNavigation = FMHCompositeOutlinerNavigation();
    return false;
}

} // namespace UE::MimirComposite
