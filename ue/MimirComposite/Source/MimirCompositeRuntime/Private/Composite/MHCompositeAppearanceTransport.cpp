#include "Composite/MHCompositeAppearanceTransport.h"

#include "Components/StaticMeshComponent.h"
#include "SceneTypes.h"

namespace UE::MimirComposite
{

bool MHIsAdmissibleAppearanceCustomDataBaseIndex(const int32 BaseIndex)
{
    // The engine silently drops writes past its own array, which would leave a
    // leaf carrying a partial appearance. Refuse the whole window instead.
    return BaseIndex >= 0 &&
        BaseIndex + MH_APPEARANCE_CHANNELS <= FCustomPrimitiveData::NumCustomPrimitiveDataFloats;
}

bool MHApplyLeafAppearanceCustomData(USceneComponent* Component,
    const FMHResolvedCompositeLeaf& Leaf, const int32 BaseIndex)
{
    UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component);
    if (!IsValid(Mesh) || !MHIsAdmissibleAppearanceCustomDataBaseIndex(BaseIndex)) return false;
    for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
    {
        Mesh->SetCustomPrimitiveDataFloat(BaseIndex + Channel, Leaf.AppearanceChannels[Channel]);
    }
    return true;
}

int32 MHApplyCompositeAppearanceCustomData(TConstArrayView<TObjectPtr<USceneComponent>> Leaves,
    const FMHResolvedCompositePlan& Plan, const int32 BaseIndex)
{
    // Fail closed before any write: a component view that no longer matches the
    // plan is repaired by a full rebuild, never by a partial walk over the
    // shorter array, which would leave the surplus silently stale.
    if (Leaves.Num() != Plan.Leaves.Num()) return INDEX_NONE;
    int32 Applied = 0;
    for (int32 Index = 0; Index < Leaves.Num(); ++Index)
    {
        if (MHApplyLeafAppearanceCustomData(Leaves[Index], Plan.Leaves[Index], BaseIndex)) ++Applied;
    }
    return Applied;
}

} // namespace UE::MimirComposite
