#include "UI/MHCompositeOutlinerEditActions.h"

#include "Editing/MHCompositeEditDocument.h"
#include "UI/MHCompositeOutlinerModel.h"

using namespace UE::MimirComposite;

FGuid MHOutlinerAddParentFor(const FMHCompositeOutlinerItem* Target, const UMHCompositeEditDocument& Draft)
{
    static_cast<void>(Target); static_cast<void>(Draft);
    return FGuid();
}

bool MHDescribeOutlinerAssetAdd(
    const UObject* Asset,
    const FMHCompositeOutlinerItem* Target,
    const UMHCompositeEditDocument& Draft,
    FMHOutlinerAddRequest& OutRequest,
    FString& OutError)
{
    static_cast<void>(Asset); static_cast<void>(Target); static_cast<void>(Draft);
    OutRequest = FMHOutlinerAddRequest();
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: not implemented");
    return false;
}
