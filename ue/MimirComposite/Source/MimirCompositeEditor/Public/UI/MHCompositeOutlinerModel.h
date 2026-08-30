#pragma once

#include "Composite/MHCompositeAsset.h"
#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"
#include "Templates/SharedPointer.h"

class AMHCompositeActor;
class UObject;
class USceneComponent;

namespace UE::MimirComposite
{

enum class EMHCompositeOutlinerItemType : uint8
{
    SourceNode,
    Option
};

/** One read-only presentation row; it never owns or mutates source authority. */
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeOutlinerItem final :
    public TSharedFromThis<FMHCompositeOutlinerItem>
{
    EMHCompositeOutlinerItemType ItemType = EMHCompositeOutlinerItemType::SourceNode;
    EMHRandomSemanticKind Kind = EMHRandomSemanticKind::Group;
    FString Label;
    FString AuthoredName;
    FString Resource;
    FString NodePath;
    FString Profile;
    FTransform FixedTransform = FTransform::Identity;
    int32 PlaceType = INDEX_NONE;
    bool bAppearanceSeedBoundary = false;

    int32 SourceNodeIndex = INDEX_NONE;
    int32 TopLevelNodeIndex = INDEX_NONE;
    int32 OptionIndex = INDEX_NONE;
    float Weight = 0.0f;

    bool bHasResolvedOverlay = false;
    bool bSelectedOption = false;
    bool bMissingEndpoint = false;
    TOptional<FMHRandomTrs> SampledLocalTrs;

    TWeakObjectPtr<UMHCompositeAsset> SourceAsset;
    TWeakObjectPtr<USceneComponent> PlacementComponent;
    TWeakPtr<FMHCompositeOutlinerItem> Parent;
    TArray<TSharedPtr<FMHCompositeOutlinerItem>> Children;

    /** Number of source children, excluding random options and lazy nested roots. */
    int32 AuthoredChildCount = 0;
    bool bNestedChildrenLoaded = false;
    FString ExpansionError;
    TArray<FString> CompositeAncestry;

    bool IsOption() const { return ItemType == EMHCompositeOutlinerItemType::Option; }
    bool IsCompositeReference() const
    {
        return Kind == EMHRandomSemanticKind::Composite && !Resource.IsEmpty();
    }
};

struct MIMIRCOMPOSITEEDITOR_API FMHCompositeOutlinerNavigation
{
    FString Name;
    FString SourceFilepath;
    TWeakObjectPtr<UObject> Asset;
};

/**
 * Testable, non-Slate source-tree and resolved-overlay model. Nested composite
 * assets are admitted lazily when their referencing row is expanded.
 */
class MIMIRCOMPOSITEEDITOR_API FMHCompositeOutlinerModel final
{
public:
    using FAssetResolver =
        TFunction<UObject*(EMHRandomSemanticKind Kind, const FString& Resource, FString& OutError)>;

    explicit FMHCompositeOutlinerModel(FAssetResolver InAssetResolver = FAssetResolver());

    bool Build(
        const UMHCompositeAsset& RootAsset,
        const FMHResolvedCompositePlan* Plan,
        const FString& PlanUnavailableReason = FString());
    bool BuildFromActor(AMHCompositeActor& Actor);
    bool RefreshOverlay(
        const FMHResolvedCompositePlan* Plan,
        const FString& PlanUnavailableReason = FString());
    bool ExpandItem(const TSharedPtr<FMHCompositeOutlinerItem>& Item);

    const TArray<TSharedPtr<FMHCompositeOutlinerItem>>& GetRoots() const { return Roots; }
    const FString& GetOverlayStatus() const { return OverlayStatus; }
    TSharedPtr<FMHCompositeOutlinerItem> FindByNodePath(const FString& NodePath);
    TSharedPtr<FMHCompositeOutlinerItem> FindForComponent(const USceneComponent* Component);

    FString GetCopyName(const FMHCompositeOutlinerItem& Item) const;
    bool GetNavigation(
        const FMHCompositeOutlinerItem& Item,
        const FString& SourceRoot,
        FMHCompositeOutlinerNavigation& OutNavigation) const;

private:
    bool BuildAssetRows(
        const UMHCompositeAsset& Asset,
        const FString& Prefix,
        const TSharedPtr<FMHCompositeOutlinerItem>& NestedParent,
        const TArray<FString>& CompositeAncestry,
        TArray<TSharedPtr<FMHCompositeOutlinerItem>>& OutRoots,
        FString& OutError);
    UObject* ResolveAsset(
        EMHRandomSemanticKind Kind,
        const FString& Resource,
        FString& OutError) const;
    void ApplyOverlayToItem(const TSharedPtr<FMHCompositeOutlinerItem>& Item);
    void BindComponentToItem(const TSharedPtr<FMHCompositeOutlinerItem>& Item);

    FAssetResolver AssetResolver;
    TArray<TSharedPtr<FMHCompositeOutlinerItem>> Roots;
    TMap<FString, TSharedPtr<FMHCompositeOutlinerItem>> ItemsByPath;
    TMap<TWeakObjectPtr<const USceneComponent>, TWeakPtr<FMHCompositeOutlinerItem>> ItemsByComponent;
    TMap<FString, FMHRandomTrs> ResolvedLocalTrsByPath;
    TMap<FString, int32> SelectedOptionsByPath;
    TSet<FString> ResolvedLeafPaths;
    TMap<FString, TWeakObjectPtr<USceneComponent>> ComponentsByPath;
    TSet<FString> MissingEndpointPaths;
    TWeakObjectPtr<UMHCompositeAsset> RootAsset;
    FString OverlayStatus;
};

} // namespace UE::MimirComposite
