#include "Composite/MHCompositeActorFactory.h"

#include "AssetRegistry/AssetData.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeActorFactory)

UMHCompositeActorFactory::UMHCompositeActorFactory()
{
    DisplayName = INVTEXT("MH Composite");
    NewActorClass = AMHCompositeActor::StaticClass();
    bUseSurfaceOrientation = false;
    bShowInEditorQuickMenu = false;
}

bool UMHCompositeActorFactory::CanCreateActorFrom(
    const FAssetData& AssetData,
    FText& OutErrorMsg)
{
    if (!AssetData.IsValid() || !AssetData.IsInstanceOf(UMHCompositeAsset::StaticClass()))
    {
        OutErrorMsg = INVTEXT("A managed MH Composite asset is required");
        return false;
    }
    return true;
}

void UMHCompositeActorFactory::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
    Super::PostSpawnActor(Asset, NewActor);
    AMHCompositeActor* CompositeActor = Cast<AMHCompositeActor>(NewActor);
    UMHCompositeAsset* CompositeAsset = Cast<UMHCompositeAsset>(Asset);
    if (CompositeActor != nullptr && CompositeAsset != nullptr)
    {
        CompositeActor->SetCompositeAsset(CompositeAsset);
        CompositeActor->SetActorLabel(CompositeAsset->GetName());
    }
}

UObject* UMHCompositeActorFactory::GetAssetFromActorInstance(AActor* ActorInstance)
{
    const AMHCompositeActor* CompositeActor = Cast<AMHCompositeActor>(ActorInstance);
    return CompositeActor != nullptr ? CompositeActor->GetCompositeAsset() : nullptr;
}
