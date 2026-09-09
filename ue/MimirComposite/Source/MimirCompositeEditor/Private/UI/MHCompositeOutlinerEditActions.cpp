#include "UI/MHCompositeOutlinerEditActions.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditSession.h"
#include "Engine/StaticMesh.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UI/MHCompositeOutlinerModel.h"

using namespace UE::MimirComposite;

FMHOutlinerCommandStamp FMHOutlinerCommandStamp::Capture(const UMHCompositeEditSession& InSession)
{
    FMHOutlinerCommandStamp Result;
    Result.Session = &InSession;
    Result.SessionId = InSession.GetSessionId();
    Result.Epoch = InSession.GetEpoch();
    Result.DraftChangeSerial = InSession.GetDraft() != nullptr ? InSession.GetDraft()->GetChangeSerial() : 0;
    return Result;
}

bool FMHOutlinerCommandStamp::Matches(const UMHCompositeEditSession& InSession) const
{
    return Session.Get() == &InSession && InSession.IsOpen() &&
        SessionId == InSession.GetSessionId() && Epoch == InSession.GetEpoch() &&
        InSession.GetDraft() != nullptr && DraftChangeSerial == InSession.GetDraft()->GetChangeSerial();
}

bool MHResolveOutlinerAddParent(
    const FMHCompositeOutlinerItem* Target,
    const bool bExplicitRoot,
    const UMHCompositeEditDocument& Draft,
    FGuid& OutParentId,
    FString& OutError)
{
    OutParentId.Invalidate();
    OutError.Reset();
    if (Target == nullptr)
    {
        if (bExplicitRoot) return true;
        OutError = TEXT("MH_E_INVALID_AUTHORING_TARGET: select an authored node or choose Current Composite / Root");
        return false;
    }
    if (Target->IsOption())
    {
        OutError = TEXT("MH_E_INVALID_AUTHORING_TARGET: a content variant cannot contain child nodes");
        return false;
    }
    if (!Target->DraftNodeId.IsValid() || Draft.FindNodeIndex(Target->DraftNodeId) == INDEX_NONE)
    {
        OutError = TEXT("MH_E_INVALID_AUTHORING_TARGET: locked context is read-only; use Edit Contents first");
        return false;
    }
    OutParentId = Target->DraftNodeId;
    return true;
}

bool MHResolveOutlinerSiblingInsertion(
    const FMHCompositeOutlinerItem* Target,
    const bool bBelow,
    const UMHCompositeEditDocument& Draft,
    FGuid& OutParentId,
    int32& OutSiblingIndex,
    FString& OutError)
{
    OutParentId.Invalidate();
    OutSiblingIndex = INDEX_NONE;
    FGuid ExactTarget;
    if (!MHResolveOutlinerAddParent(Target, false, Draft, ExactTarget, OutError)) return false;
    OutParentId = Draft.GetParentId(ExactTarget);
    const TArray<FGuid> Siblings = Draft.GetChildIds(OutParentId);
    const int32 TargetIndex = Siblings.IndexOfByKey(ExactTarget);
    if (TargetIndex == INDEX_NONE)
    {
        OutError = TEXT("MH_E_INVALID_AUTHORING_TARGET: target is not in its authored sibling list");
        OutParentId.Invalidate();
        return false;
    }
    OutSiblingIndex = TargetIndex + (bBelow ? 1 : 0);
    return true;
}

bool MHDescribeOutlinerAssetAdd(
    const UObject* Asset,
    const FMHCompositeOutlinerItem* Target,
    const UMHCompositeEditDocument& Draft,
    FMHOutlinerAddRequest& OutRequest,
    FString& OutError)
{
    OutRequest = FMHOutlinerAddRequest();
    OutError.Reset();
    FMHCompositeOption Described;
    if (!MHDescribeOutlinerAssetOption(Asset, Described, OutError)) return false;
    OutRequest.Kind = Described.Kind == EMHCompositeOptionKind::Composite
        ? EMHCompositeNodeKind::Composite
        : EMHCompositeNodeKind::Mesh;
    OutRequest.Resource = Described.Resource;
    if (!MHResolveOutlinerAddParent(Target, false, Draft, OutRequest.ParentId, OutError)) return false;
    return true;
}

bool MHDescribeOutlinerAssetAddBatch(
    const TConstArrayView<UObject*> Assets,
    const FMHCompositeOutlinerItem* Target,
    const bool bExplicitRoot,
    const UMHCompositeEditDocument& Draft,
    TArray<FMHOutlinerAddRequest>& OutRequests,
    FString& OutError)
{
    OutRequests.Reset();
    OutError.Reset();
    FGuid ParentId;
    if (!MHResolveOutlinerAddParent(Target, bExplicitRoot, Draft, ParentId, OutError)) return false;
    if (Assets.IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select one or more managed assets in the Content Browser");
        return false;
    }
    TArray<FMHOutlinerAddRequest> Validated;
    Validated.Reserve(Assets.Num());
    for (const UObject* Asset : Assets)
    {
        FMHOutlinerAddRequest Request;
        FMHCompositeOption Described;
        if (!MHDescribeOutlinerAssetOption(Asset, Described, OutError)) return false;
        Request.Kind = Described.Kind == EMHCompositeOptionKind::Composite
            ? EMHCompositeNodeKind::Composite
            : EMHCompositeNodeKind::Mesh;
        Request.Resource = Described.Resource;
        Request.ParentId = ParentId;
        Validated.Add(MoveTemp(Request));
    }
    OutRequests = MoveTemp(Validated);
    return true;
}

bool MHDescribeOutlinerAssetOption(
    const UObject* Asset,
    FMHCompositeOption& OutOption,
    FString& OutError)
{
    OutOption = FMHCompositeOption();
    // Managed-resource admission is independent of the tree destination.
    if (Asset == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: nothing to add");
        return false;
    }
    if (const UMHCompositeAsset* Composite = Cast<UMHCompositeAsset>(Asset))
    {
        if (Composite->LogicalName.IsEmpty())
        {
            OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the composite has no logical name");
            return false;
        }
        OutOption.Kind = EMHCompositeOptionKind::Composite;
        OutOption.Resource = Composite->LogicalName;
    }
    else if (const UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
    {
        const UMHStaticMeshImportData* Receipt = Cast<UMHStaticMeshImportData>(Mesh->GetAssetImportData());
        if (Receipt == nullptr || Receipt->LogicalName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s is not a managed MH static mesh (no import receipt)"), *Mesh->GetName());
            return false;
        }
        OutOption.Kind = EMHCompositeOptionKind::Mesh;
        OutOption.Resource = Receipt->LogicalName;
    }
    else
    {
        OutError = FString::Printf(TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s is neither a managed composite nor a managed static mesh"), *Asset->GetName());
        return false;
    }
    OutOption.Weight = 1.0f;
    return true;
}
