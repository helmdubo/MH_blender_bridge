#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "MHTextureSourceData.generated.h"

/** Applied-source receipt persisted on each managed texture. */
UCLASS(BlueprintType)
class MIMIRCOMPOSITERUNTIME_API UMHTextureSourceData final : public UAssetUserData
{
    GENERATED_BODY()

public:
    /** Defense in depth: receipt instances must never survive cook. */
    virtual bool IsEditorOnly() const override { return true; }

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString LogicalName;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString SourceRelativePath;

    /** Raw BLAKE3-160 of the source bytes last applied by UE. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString SourceHash;

#if WITH_EDITOR
    virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif
};
