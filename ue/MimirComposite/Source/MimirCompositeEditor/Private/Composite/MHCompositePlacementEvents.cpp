#include "Composite/MHCompositePlacementEvents.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

namespace UE::MimirComposite
{

int32 MHRebuildAllLoadedCompositeActors()
{
    int32 RebuiltCount = 0;
    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        UWorld* World = IsValid(Actor) ? Actor->GetWorld() : nullptr;
        if (!IsValid(Actor) || Actor->IsTemplate() || Actor->IsActorBeingDestroyed() ||
            Actor->IsPlacementEditMode() || World == nullptr || World->IsGameWorld() ||
            World->IsBeingCleanedUp() || World->IsCleanedUp())
        {
            continue;
        }
        Actor->RebuildComposite();
        ++RebuiltCount;
    }
    return RebuiltCount;
}

void MHNotifyGeneratedResourceChanged(const FMHResourceKey& Key)
{
    if (!Key.IsCanonical())
    {
        return;
    }

    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        UWorld* World = IsValid(Actor) ? Actor->GetWorld() : nullptr;
        if (!IsValid(Actor) || Actor->IsTemplate() || Actor->IsActorBeingDestroyed() ||
            World == nullptr || World->IsBeingCleanedUp() || World->IsCleanedUp() ||
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
