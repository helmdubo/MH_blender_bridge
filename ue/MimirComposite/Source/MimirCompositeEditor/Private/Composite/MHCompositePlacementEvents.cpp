#include "Composite/MHCompositePlacementEvents.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeDefinitionSubsystem.h"
#include "Editor.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

namespace UE::MimirComposite
{

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
TFunction<void(const FMHResourceKey&)> GGeneratedResourceChangedObserverForTests;
}
#endif

int32 MHRebuildAllLoadedCompositeActors()
{
    if (GEditor != nullptr)
    {
        if (UMHCompositeDefinitionSubsystem* Definitions =
                GEditor->GetEditorSubsystem<UMHCompositeDefinitionSubsystem>())
        {
            Definitions->InvalidateAllDefinitions();
        }
    }
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

#if WITH_DEV_AUTOMATION_TESTS
    if (GGeneratedResourceChangedObserverForTests)
    {
        GGeneratedResourceChangedObserverForTests(Key);
    }
#endif

    if (GEditor != nullptr)
    {
        if (UMHCompositeDefinitionSubsystem* Definitions =
                GEditor->GetEditorSubsystem<UMHCompositeDefinitionSubsystem>())
        {
            Definitions->InvalidateDefinition(Key);
        }
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

#if WITH_DEV_AUTOMATION_TESTS
void MHSetGeneratedResourceChangedObserverForTests(
    TFunction<void(const FMHResourceKey&)> Observer)
{
    GGeneratedResourceChangedObserverForTests = MoveTemp(Observer);
}
#endif

} // namespace UE::MimirComposite
