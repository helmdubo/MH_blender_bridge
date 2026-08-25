#pragma once

#include "ActorFactories/ActorFactory.h"
#include "MHCompositeActorFactory.generated.h"

/** Content Browser/viewport adapter for placing managed composite assets. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHCompositeActorFactory final : public UActorFactory
{
    GENERATED_BODY()

public:
    UMHCompositeActorFactory();

    virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
    virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
    virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
