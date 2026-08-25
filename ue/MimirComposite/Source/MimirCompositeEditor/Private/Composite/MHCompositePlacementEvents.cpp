#include "Composite/MHCompositePlacementEvents.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "UObject/UObjectIterator.h"

namespace UE::MimirComposite
{

void MHNotifyGeneratedResourceChanged(const FMHResourceKey& Key)
{
    if (!Key.IsCanonical())
    {
        return;
    }

    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        if (Actor == nullptr || Actor->IsTemplate() || Actor->GetWorld() == nullptr ||
            !Actor->DependsOnResource(Key))
        {
            continue;
        }
        Actor->RebuildComposite();
    }
}

void MHNotifyCompositeAssetChanged(UMHCompositeAsset& Asset)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Asset.LogicalName;
    MHNotifyGeneratedResourceChanged(Key);
}

} // namespace UE::MimirComposite
