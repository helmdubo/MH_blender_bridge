#include "UI/MHCompositeOutlinerModel.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"

namespace
{

using namespace UE::MimirComposite;

EMHRandomSemanticKind OutlinerNodeKind(const EMHCompositeNodeKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeNodeKind::Mesh: return EMHRandomSemanticKind::Mesh;
    case EMHCompositeNodeKind::Actor: return EMHRandomSemanticKind::Actor;
    case EMHCompositeNodeKind::Composite: return EMHRandomSemanticKind::Composite;
    case EMHCompositeNodeKind::Random: return EMHRandomSemanticKind::Random;
    case EMHCompositeNodeKind::GameObj: return EMHRandomSemanticKind::GameObj;
    case EMHCompositeNodeKind::Group: return EMHRandomSemanticKind::Group;
    }
    checkNoEntry();
    return EMHRandomSemanticKind::Group;
}

EMHRandomSemanticKind OutlinerOptionKind(const EMHCompositeOptionKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeOptionKind::Mesh: return EMHRandomSemanticKind::Mesh;
    case EMHCompositeOptionKind::Actor: return EMHRandomSemanticKind::Actor;
    case EMHCompositeOptionKind::Composite: return EMHRandomSemanticKind::Composite;
    case EMHCompositeOptionKind::Empty: return EMHRandomSemanticKind::Empty;
    case EMHCompositeOptionKind::GameObj: return EMHRandomSemanticKind::GameObj;
    }
    checkNoEntry();
    return EMHRandomSemanticKind::Empty;
}

FString OutlinerKindLabel(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh: return TEXT("mesh");
    case EMHRandomSemanticKind::Actor: return TEXT("actor");
    case EMHRandomSemanticKind::Composite: return TEXT("composite");
    case EMHRandomSemanticKind::Group: return TEXT("group");
    case EMHRandomSemanticKind::Random: return TEXT("random");
    case EMHRandomSemanticKind::Empty: return TEXT("empty");
    case EMHRandomSemanticKind::GameObj: return TEXT("gameobj");
    }
    return TEXT("unknown");
}

FString OutlinerNodeLabel(const FMHCompositeAssetNode& Node)
{
    if (!Node.Name.IsEmpty()) return Node.Name;
    if (!Node.Resource.IsEmpty()) return Node.Resource;
    return OutlinerKindLabel(OutlinerNodeKind(Node.Kind));
}

FString OutlinerOptionLabel(const FMHCompositeOption& Option)
{
    if (Option.Kind == EMHCompositeOptionKind::Empty) return TEXT("--");
    return Option.Resource.IsEmpty() ? OutlinerKindLabel(OutlinerOptionKind(Option.Kind)) : Option.Resource;
}

} // namespace

namespace UE::MimirComposite
{

AMHCompositeActor* MHResolveCompositeOutlinerActor(
    const TArray<UObject*>& SelectedActors,
    const TArray<UInstancedStaticMeshComponent*>& SelectedInstances)
{
    AMHCompositeActor* Actor = nullptr;
    for (UObject* Object : SelectedActors)
    {
        if (AMHCompositeActor* Composite = Cast<AMHCompositeActor>(Object))
        {
            if (Actor != nullptr && Actor != Composite) return nullptr;
            Actor = Composite;
        }
    }
    if (Actor != nullptr) return Actor;

    for (UInstancedStaticMeshComponent* Instance : SelectedInstances)
    {
        if (!IsValid(Instance)) continue;
        AMHCompositeActor* Owner = Cast<AMHCompositeActor>(Instance->GetOwner());
        if (Owner == nullptr) continue;
        if (Actor != nullptr && Actor != Owner) return nullptr;
        Actor = Owner;
    }
    return Actor;
}

FMHCompositeOutlinerFreshness FMHCompositeOutlinerFreshness::Capture(
    const AMHCompositeActor& Actor)
{
    FMHCompositeOutlinerFreshness Result;
    Result.Seed = Actor.GetSeed();
    Result.AppearanceSeed = Actor.GetAppearanceSeed();
    Result.ResolvedSignature = Actor.GetCompactResolvedSignature();
    Result.AppearanceSignature = Actor.GetCompactAppearanceSignature();
    Result.PlacementSignature = Actor.GetCompactPlacementSignature();
    return Result;
}

bool FMHCompositeOutlinerFreshness::IsComplete() const
{
    return !ResolvedSignature.IsEmpty() &&
        !AppearanceSignature.IsEmpty() &&
        !PlacementSignature.IsEmpty();
}

bool FMHCompositeOutlinerFreshness::Matches(
    const FMHCompositeOutlinerFreshness& Other) const
{
    return IsComplete() && Other.IsComplete() &&
        Seed == Other.Seed && AppearanceSeed == Other.AppearanceSeed &&
        ResolvedSignature == Other.ResolvedSignature &&
        AppearanceSignature == Other.AppearanceSignature &&
        PlacementSignature == Other.PlacementSignature;
}

bool FMHCompositeOutlinerRefreshState::NeedsRebuild(
    AMHCompositeActor* Actor,
    const FMHCompositeOutlinerFreshness& CurrentFreshness) const
{
    return !IsValid(Actor) || BuiltActor.Get() != Actor ||
        !BuiltFreshness.Matches(CurrentFreshness);
}

void FMHCompositeOutlinerRefreshState::RecordRebuild(
    AMHCompositeActor* Actor,
    const FMHCompositeOutlinerFreshness& CurrentFreshness,
    const bool bSucceeded)
{
    if (!bSucceeded || !IsValid(Actor) || !CurrentFreshness.IsComplete())
    {
        Reset();
        return;
    }
    BuiltActor = Actor;
    BuiltFreshness = CurrentFreshness;
}

void FMHCompositeOutlinerRefreshState::Reset()
{
    BuiltActor.Reset();
    BuiltFreshness = FMHCompositeOutlinerFreshness();
}

FMHCompositeOutlinerModel::FMHCompositeOutlinerModel(FAssetResolver InAssetResolver)
    : AssetResolver(MoveTemp(InAssetResolver))
{
}

bool FMHCompositeOutlinerModel::Build(
    const UMHCompositeAsset& InRootAsset,
    const FMHResolvedCompositePlan* Plan,
    const FString& PlanUnavailableReason)
{
    RootAsset = const_cast<UMHCompositeAsset*>(&InRootAsset);
    Roots.Reset();
    ItemsByPath.Reset();
    ItemsByComponent.Reset();
    ItemsByInstance.Reset();
    ComponentsByPath.Reset();
    MissingEndpointPaths.Reset();
    FString Error;
    const TArray<FString> Ancestry{InRootAsset.LogicalName};
    if (InRootAsset.LogicalName.IsEmpty() || !BuildAssetRows(
            InRootAsset,
            InRootAsset.LogicalName,
            nullptr,
            Ancestry,
            Roots,
            Error))
    {
        OverlayStatus = Error.IsEmpty() ? TEXT("source composite has no logical name") : MoveTemp(Error);
        Roots.Reset();
        ItemsByPath.Reset();
        return false;
    }
    return RefreshOverlay(Plan, PlanUnavailableReason);
}

bool FMHCompositeOutlinerModel::BuildFromActor(AMHCompositeActor& Actor)
{
    const UMHCompositeAsset* Asset = Actor.GetCompositeAsset();
    const FMHResolvedCompositePlan* Plan = Actor.GetResolvedPlan();
    if (Asset == nullptr || !Build(*Asset, Plan, Actor.GetLastPlacementError())) return false;
    if (Plan == nullptr) return true;

    const TArray<FMHCompositeLeafMaterialization>& Leaves = Actor.GetLeafMaterializations();
    for (int32 Index = 0; Index < Plan->Leaves.Num(); ++Index)
    {
        const FString& Path = Plan->Leaves[Index].Origin;
        const FMHCompositeLeafMaterialization* Row =
            Leaves.IsValidIndex(Index) ? &Leaves[Index] : nullptr;
        USceneComponent* Component = Row != nullptr ? Row->Component.Get() : nullptr;
        if (IsValid(Component))
            ComponentsByPath.Add(Path, {Component, Row->InstanceIndex, Row->ResolvedNodeIndex});
        else MissingEndpointPaths.Add(Path);
    }
    const TArray<TObjectPtr<USceneComponent>>& Handles = Actor.GetTopLevelPlacementComponents();
    for (int32 Index = 0; Index < Roots.Num() && Index < Handles.Num(); ++Index)
    {
        if (IsValid(Handles[Index]))
            ComponentsByPath.FindOrAdd(Roots[Index]->NodePath, {Handles[Index], INDEX_NONE, INDEX_NONE});
    }
    for (const TPair<FString, TSharedPtr<FMHCompositeOutlinerItem>>& Pair : ItemsByPath)
        BindComponentToItem(Pair.Value);
    return true;
}

bool FMHCompositeOutlinerModel::RefreshOverlay(
    const FMHResolvedCompositePlan* Plan,
    const FString& PlanUnavailableReason)
{
    ResolvedLocalTrsByPath.Reset();
    SelectedOptionsByPath.Reset();
    ResolvedLeafPaths.Reset();
    OverlayStatus = PlanUnavailableReason;
    for (const TPair<FString, TSharedPtr<FMHCompositeOutlinerItem>>& Pair : ItemsByPath)
    {
        FMHCompositeOutlinerItem& Item = *Pair.Value;
        Item.bHasResolvedOverlay = false;
        Item.bSelectedOption = false;
        Item.bMissingEndpoint = false;
        Item.SampledLocalTrs.Reset();
    }
    if (Plan == nullptr) return true;
    if (!RootAsset.IsValid() ||
        (!Plan->Nodes.IsEmpty() && !Plan->Nodes[0].NodePath.StartsWith(
            RootAsset->LogicalName + TEXT(":"), ESearchCase::CaseSensitive)))
    {
        OverlayStatus = TEXT("resolved plan does not match the selected source composite");
        return true;
    }

    OverlayStatus.Reset();
    for (const FMHResolvedCompositeNode& Node : Plan->Nodes)
    {
        ResolvedLocalTrsByPath.Add(Node.NodePath, Node.LocalTrs);
        if (Node.SemanticKind == EMHRandomSemanticKind::Random)
            SelectedOptionsByPath.Add(Node.NodePath, Node.SelectedOptionIndex);
    }
    for (const FMHResolvedCompositeLeaf& Leaf : Plan->Leaves)
        ResolvedLeafPaths.Add(Leaf.Origin);
    for (const TPair<FString, TSharedPtr<FMHCompositeOutlinerItem>>& Pair : ItemsByPath)
        ApplyOverlayToItem(Pair.Value);
    return true;
}

bool FMHCompositeOutlinerModel::ExpandItem(const TSharedPtr<FMHCompositeOutlinerItem>& Item)
{
    if (!Item.IsValid() || !Item->IsCompositeReference()) return false;
    if (Item->bNestedChildrenLoaded) return Item->ExpansionError.IsEmpty();
    Item->bNestedChildrenLoaded = true;
    if (Item->CompositeAncestry.Contains(Item->Resource))
    {
        Item->ExpansionError = TEXT("composite cycle: ") + Item->Resource;
        Item->bMissingEndpoint = true;
        return false;
    }

    FString Error;
    UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(
        ResolveAsset(EMHRandomSemanticKind::Composite, Item->Resource, Error));
    if (Asset == nullptr)
    {
        Item->ExpansionError = Error.IsEmpty()
            ? TEXT("missing composite asset: ") + Item->Resource
            : MoveTemp(Error);
        Item->bMissingEndpoint = true;
        return false;
    }
    if (Asset->LogicalName != Item->Resource)
    {
        Item->ExpansionError = TEXT("composite asset identity mismatch: expected ") + Item->Resource;
        Item->bMissingEndpoint = true;
        return false;
    }
    TArray<FString> Ancestry = Item->CompositeAncestry;
    Ancestry.Add(Item->Resource);
    TArray<TSharedPtr<FMHCompositeOutlinerItem>> NestedRoots;
    if (!BuildAssetRows(
            *Asset,
            Item->NodePath + TEXT(">") + Item->Resource,
            Item,
            Ancestry,
            NestedRoots,
            Error))
    {
        Item->ExpansionError = MoveTemp(Error);
        Item->bMissingEndpoint = true;
        return false;
    }
    if (!NestedRoots.IsEmpty()) Item->Children.Insert(NestedRoots, 0);
    return true;
}

TSharedPtr<FMHCompositeOutlinerItem> FMHCompositeOutlinerModel::FindByNodePath(
    const FString& NodePath)
{
    for (;;)
    {
        if (const TSharedPtr<FMHCompositeOutlinerItem>* Found = ItemsByPath.Find(NodePath))
            return *Found;
        TSharedPtr<FMHCompositeOutlinerItem> Border;
        for (const TPair<FString, TSharedPtr<FMHCompositeOutlinerItem>>& Pair : ItemsByPath)
        {
            const TSharedPtr<FMHCompositeOutlinerItem>& Candidate = Pair.Value;
            if (Candidate->IsCompositeReference() && !Candidate->bNestedChildrenLoaded &&
                NodePath.StartsWith(
                    Candidate->NodePath + TEXT(">") + Candidate->Resource + TEXT(":"),
                    ESearchCase::CaseSensitive))
            {
                Border = Candidate;
                break;
            }
        }
        if (!Border.IsValid() || !ExpandItem(Border)) return nullptr;
    }
}

TSharedPtr<FMHCompositeOutlinerItem> FMHCompositeOutlinerModel::FindForComponent(
    const USceneComponent* Component)
{
    const TWeakPtr<FMHCompositeOutlinerItem>* Found = ItemsByComponent.Find(Component);
    if (Found != nullptr) return Found->Pin();

    FString DesiredPath;
    for (const TPair<FString, FPlacementRow>& Pair : ComponentsByPath)
    {
        if (Pair.Value.Component.Get() == Component && Pair.Value.InstanceIndex == INDEX_NONE)
        {
            DesiredPath = Pair.Key;
            break;
        }
    }
    if (DesiredPath.IsEmpty()) return nullptr;

    // Viewport selection is itself a navigation request. Expand only the
    // composite borders that contain the requested path; unrelated definitions
    // stay unloaded.
    return FindByNodePath(DesiredPath);
}

TSharedPtr<FMHCompositeOutlinerItem> FMHCompositeOutlinerModel::FindForInstance(
    const USceneComponent* Component, const int32 InstanceIndex)
{
    const TMap<int32, TWeakPtr<FMHCompositeOutlinerItem>>* ByIndex =
        ItemsByInstance.Find(Component);
    if (ByIndex != nullptr)
    {
        if (const TWeakPtr<FMHCompositeOutlinerItem>* Found = ByIndex->Find(InstanceIndex))
            return Found->Pin();
    }
    FString DesiredPath;
    for (const TPair<FString, FPlacementRow>& Pair : ComponentsByPath)
    {
        if (Pair.Value.Component.Get() == Component && Pair.Value.InstanceIndex == InstanceIndex)
        {
            DesiredPath = Pair.Key;
            break;
        }
    }
    return DesiredPath.IsEmpty() ? nullptr : FindByNodePath(DesiredPath);
}

FString FMHCompositeOutlinerModel::GetCopyName(const FMHCompositeOutlinerItem& Item) const
{
    if (!Item.Resource.IsEmpty()) return Item.Resource;
    if (!Item.AuthoredName.IsEmpty()) return Item.AuthoredName;
    return Item.Label;
}

bool FMHCompositeOutlinerModel::GetNavigation(
    const FMHCompositeOutlinerItem& Item,
    const FString& SourceRoot,
    FMHCompositeOutlinerNavigation& OutNavigation) const
{
    OutNavigation = FMHCompositeOutlinerNavigation();
    if (Item.Resource.IsEmpty() || Item.Kind == EMHRandomSemanticKind::Empty) return false;
    FString Error;
    UObject* Asset = ResolveAsset(Item.Kind, Item.Resource, Error);
    if (Asset == nullptr) return false;
    if (const UMHCompositeAsset* Composite = Cast<UMHCompositeAsset>(Asset);
        Composite != nullptr && Composite->LogicalName != Item.Resource)
    {
        return false;
    }
    OutNavigation.Name = Item.Resource;
    OutNavigation.Asset = Asset;
    if (const UMHCompositeAsset* Composite = Cast<UMHCompositeAsset>(Asset))
    {
        const FString Relative = Composite->SourceRelativePath.IsEmpty()
            ? Item.Resource + TEXT(".composite")
            : Composite->SourceRelativePath;
        OutNavigation.SourceFilepath = SourceRoot.IsEmpty()
            ? Relative
            : FPaths::ConvertRelativePathToFull(FPaths::Combine(SourceRoot, Relative));
    }
    return true;
}

bool FMHCompositeOutlinerModel::BuildAssetRows(
    const UMHCompositeAsset& Asset,
    const FString& Prefix,
    const TSharedPtr<FMHCompositeOutlinerItem>& NestedParent,
    const TArray<FString>& CompositeAncestry,
    TArray<TSharedPtr<FMHCompositeOutlinerItem>>& OutRoots,
    FString& OutError)
{
    // Validate the persisted pre-order in full before admitting even one row.
    // A corrupt nested asset must not leave a partially navigable subtree.
    for (int32 SourceIndex = 0; SourceIndex < Asset.Nodes.Num(); ++SourceIndex)
    {
        const int32 ParentIndex = Asset.Nodes[SourceIndex].ParentIndex;
        if (ParentIndex < INDEX_NONE || ParentIndex >= SourceIndex)
        {
            OutError = FString::Printf(
                TEXT("invalid source parent index %d at node %d"), ParentIndex, SourceIndex);
            return false;
        }
    }
    TArray<TSharedPtr<FMHCompositeOutlinerItem>> BySourceIndex;
    BySourceIndex.Reserve(Asset.Nodes.Num());
    TMap<int32, int32> NextChildOrdinal;
    int32 NextRootOrdinal = 0;
    for (int32 SourceIndex = 0; SourceIndex < Asset.Nodes.Num(); ++SourceIndex)
    {
        const FMHCompositeAssetNode& Source = Asset.Nodes[SourceIndex];
        if (Source.ParentIndex != INDEX_NONE && !BySourceIndex.IsValidIndex(Source.ParentIndex))
        {
            OutError = FString::Printf(
                TEXT("invalid source parent index %d at node %d"), Source.ParentIndex, SourceIndex);
            return false;
        }

        TSharedPtr<FMHCompositeOutlinerItem> Parent;
        int32 TopLevelIndex = INDEX_NONE;
        FString NodePath;
        if (Source.ParentIndex == INDEX_NONE)
        {
            TopLevelIndex = NextRootOrdinal;
            NodePath = FString::Printf(TEXT("%s:nodes[%d]"), *Prefix, NextRootOrdinal++);
        }
        else
        {
            Parent = BySourceIndex[Source.ParentIndex];
            TopLevelIndex = Parent->TopLevelNodeIndex;
            int32& Ordinal = NextChildOrdinal.FindOrAdd(Source.ParentIndex);
            NodePath = FString::Printf(TEXT("%s/children[%d]"), *Parent->NodePath, Ordinal++);
        }
        if (ItemsByPath.Contains(NodePath))
        {
            OutError = TEXT("duplicate source NodePath: ") + NodePath;
            return false;
        }

        TSharedRef<FMHCompositeOutlinerItem> Item = MakeShared<FMHCompositeOutlinerItem>();
        Item->Kind = OutlinerNodeKind(Source.Kind);
        Item->Label = OutlinerNodeLabel(Source);
        Item->AuthoredName = Source.Name;
        Item->Resource = Source.Resource;
        Item->NodePath = NodePath;
        Item->Profile = Source.Profile;
        Item->FixedTransform = Source.Transform;
        Item->PlaceType = Source.PlaceType;
        Item->bAppearanceSeedBoundary = Source.bAppearanceSeedBoundary;
        Item->SourceNodeIndex = SourceIndex;
        Item->TopLevelNodeIndex = TopLevelIndex;
        Item->SourceAsset = const_cast<UMHCompositeAsset*>(&Asset);
        Item->CompositeAncestry = CompositeAncestry;
        Item->Parent = Parent.IsValid() ? Parent : NestedParent;
        BySourceIndex.Add(Item);
        ItemsByPath.Add(NodePath, Item);

        for (int32 OptionIndex = 0; OptionIndex < Source.Options.Num(); ++OptionIndex)
        {
            const FMHCompositeOption& SourceOption = Source.Options[OptionIndex];
            TSharedRef<FMHCompositeOutlinerItem> Option = MakeShared<FMHCompositeOutlinerItem>();
            Option->ItemType = EMHCompositeOutlinerItemType::Option;
            Option->Kind = OutlinerOptionKind(SourceOption.Kind);
            Option->Label = OutlinerOptionLabel(SourceOption);
            Option->Resource = SourceOption.Resource;
            Option->NodePath = FString::Printf(TEXT("%s/options[%d]"), *NodePath, OptionIndex);
            Option->SourceNodeIndex = SourceIndex;
            Option->TopLevelNodeIndex = TopLevelIndex;
            Option->OptionIndex = OptionIndex;
            Option->Weight = SourceOption.Weight;
            Option->SourceAsset = const_cast<UMHCompositeAsset*>(&Asset);
            Option->CompositeAncestry = CompositeAncestry;
            Option->Parent = Item;
            Item->Children.Add(Option);
            ItemsByPath.Add(Option->NodePath, Option);
        }

        if (Parent.IsValid())
        {
            Parent->Children.Add(Item);
            ++Parent->AuthoredChildCount;
        }
        else
        {
            OutRoots.Add(Item);
        }
        ApplyOverlayToItem(Item);
        BindComponentToItem(Item);
        for (const TSharedPtr<FMHCompositeOutlinerItem>& Option : Item->Children)
        {
            ApplyOverlayToItem(Option);
            BindComponentToItem(Option);
        }
    }
    return true;
}

UObject* FMHCompositeOutlinerModel::ResolveAsset(
    const EMHRandomSemanticKind Kind,
    const FString& Resource,
    FString& OutError) const
{
    OutError.Reset();
    if (AssetResolver) return AssetResolver(Kind, Resource, OutError);

    if (Kind == EMHRandomSemanticKind::Actor)
    {
        const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
        const FSoftClassPath* Path = Settings != nullptr ? Settings->ActorClassRegistry.Find(Resource) : nullptr;
        UClass* Class = Path != nullptr ? Path->TryLoadClass<AActor>() : nullptr;
        if (Class == nullptr) OutError = TEXT("actor registry has no class for ") + Resource;
        return Class;
    }

    FMHResourceKey Key;
    if (Kind == EMHRandomSemanticKind::Composite) Key.Kind = EMHResourceKind::Composite;
    else if (Kind == EMHRandomSemanticKind::Mesh) Key.Kind = EMHResourceKind::StaticMesh;
    else return nullptr;
    Key.LogicalName = Resource;
    return UMHEndpointPrototypeRegistry::ResolveEndpoint(Key, OutError);
}

void FMHCompositeOutlinerModel::ApplyOverlayToItem(
    const TSharedPtr<FMHCompositeOutlinerItem>& Item)
{
    if (!Item.IsValid()) return;
    Item->bHasResolvedOverlay = false;
    Item->bSelectedOption = false;
    Item->bMissingEndpoint = MissingEndpointPaths.Contains(Item->NodePath);
    Item->SampledLocalTrs.Reset();
    if (const FMHRandomTrs* Trs = ResolvedLocalTrsByPath.Find(Item->NodePath))
    {
        Item->bHasResolvedOverlay = true;
        Item->SampledLocalTrs = *Trs;
    }
    if (ResolvedLeafPaths.Contains(Item->NodePath)) Item->bHasResolvedOverlay = true;
    if (Item->IsOption())
    {
        const TSharedPtr<FMHCompositeOutlinerItem> Parent = Item->Parent.Pin();
        const int32* Selected = Parent.IsValid() ? SelectedOptionsByPath.Find(Parent->NodePath) : nullptr;
        Item->bSelectedOption = Selected != nullptr && *Selected == Item->OptionIndex;
        Item->bHasResolvedOverlay |= Item->bSelectedOption;
    }
}

void FMHCompositeOutlinerModel::BindComponentToItem(
    const TSharedPtr<FMHCompositeOutlinerItem>& Item)
{
    if (!Item.IsValid()) return;
    Item->PlacementComponent.Reset();
    Item->PlacementInstanceIndex = INDEX_NONE;
    Item->ResolvedNodeIndex = INDEX_NONE;
    if (const FPlacementRow* Found = ComponentsByPath.Find(Item->NodePath))
    {
        if (USceneComponent* Component = Found->Component.Get())
        {
            Item->PlacementComponent = Component;
            Item->PlacementInstanceIndex = Found->InstanceIndex;
            Item->ResolvedNodeIndex = Found->ResolvedNodeIndex;
            if (Found->InstanceIndex == INDEX_NONE) ItemsByComponent.Add(Component, Item);
            else ItemsByInstance.FindOrAdd(Component).Add(Found->InstanceIndex, Item);
        }
    }
    Item->bMissingEndpoint |= MissingEndpointPaths.Contains(Item->NodePath);
}

} // namespace UE::MimirComposite
