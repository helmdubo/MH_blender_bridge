#include "Composite/MHCompositePlacementEvents.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePreviewCache.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHCompositePlacementEvents, Display, All);

namespace UE::MimirComposite
{

int32 MHRebuildAllLoadedCompositeActors()
{
    MHInvalidateCompositePreviewCache();
    TArray<TWeakObjectPtr<AMHCompositeActor>> Actors;
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
        Actors.Add(Actor);
    }
    int32 RebuiltCount = 0;
    for (const TWeakObjectPtr<AMHCompositeActor>& Weak : Actors)
        if (AMHCompositeActor* Actor = Weak.Get(); IsValid(Actor) && !Actor->IsActorBeingDestroyed())
        {
            Actor->RebuildComposite(false);
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

    MHInvalidateCompositePreviewCache(&Key);

    TArray<TWeakObjectPtr<AMHCompositeActor>> Actors;
    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        UWorld* World = IsValid(Actor) ? Actor->GetWorld() : nullptr;
        if (!IsValid(Actor) || Actor->IsTemplate() || Actor->IsActorBeingDestroyed() ||
            Actor->IsPlacementEditMode() || World == nullptr || World->IsGameWorld() ||
            World->IsBeingCleanedUp() || World->IsCleanedUp() ||
            !Actor->DependsOnResource(Key))
        {
            continue;
        }
        Actors.Add(Actor);
    }
    UE_LOG(LogMHCompositePlacementEvents, Verbose, TEXT("Notify begin key=%s actors=%d"), *Key.ToString(), Actors.Num());
    for (const TWeakObjectPtr<AMHCompositeActor>& Weak : Actors)
        if (AMHCompositeActor* Actor = Weak.Get(); IsValid(Actor) && !Actor->IsActorBeingDestroyed())
        {
            UE_LOG(LogMHCompositePlacementEvents, Verbose, TEXT("Notify key=%s actor=%s"), *Key.ToString(), *Actor->GetPathName());
            Actor->RebuildComposite(false);
        }
    UE_LOG(LogMHCompositePlacementEvents, Verbose, TEXT("Notify end key=%s"), *Key.ToString());
}

void MHNotifyCompositeAssetChanged(UMHCompositeAsset& Asset)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Asset.LogicalName;
    MHNotifyGeneratedResourceChanged(Key);
}

} // namespace UE::MimirComposite
