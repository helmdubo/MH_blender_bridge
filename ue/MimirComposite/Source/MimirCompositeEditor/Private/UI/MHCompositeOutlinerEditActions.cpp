#include "UI/MHCompositeOutlinerEditActions.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Engine/StaticMesh.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UI/MHCompositeOutlinerModel.h"

using namespace UE::MimirComposite;

FGuid MHOutlinerAddParentFor(const FMHCompositeOutlinerItem* Target, const UMHCompositeEditDocument& Draft)
{
    if (Target == nullptr || !Target->DraftNodeId.IsValid() || Draft.FindNodeIndex(Target->DraftNodeId) == INDEX_NONE) return FGuid();
    // Into a group, next to anything else.
    return Target->Kind == EMHRandomSemanticKind::Group ? Target->DraftNodeId : Draft.GetParentId(Target->DraftNodeId);
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
        OutRequest.Kind = EMHCompositeNodeKind::Composite;
        OutRequest.Resource = Composite->LogicalName;
    }
    else if (const UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
    {
        // Only a managed mesh (imported through the source protocol) has a
        // logical name the composite grammar can reference.
        const UMHStaticMeshImportData* Receipt = Cast<UMHStaticMeshImportData>(Mesh->GetAssetImportData());
        if (Receipt == nullptr || Receipt->LogicalName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s is not a managed MH static mesh (no import receipt)"), *Mesh->GetName());
            return false;
        }
        OutRequest.Kind = EMHCompositeNodeKind::Mesh;
        OutRequest.Resource = Receipt->LogicalName;
    }
    else
    {
        OutError = FString::Printf(TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s is neither a managed composite nor a managed static mesh"), *Asset->GetName());
        return false;
    }
    OutRequest.ParentId = MHOutlinerAddParentFor(Target, Draft);
    return true;
}
