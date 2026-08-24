#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "MHMaterialSourceData.generated.h"

/** Editor-module applied-state receipt persisted on each managed material instance. */
UCLASS(BlueprintType)
class MIMIRCOMPOSITEEDITOR_API UMHMaterialSourceData final : public UAssetUserData
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

    /** BLAKE3-160 of canonical JSON extracted immediately after apply. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString AppliedHash;

    /** Class registry token. Library-form persistence is blocked by OPEN-V4-9. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString ParentClass;

#if WITH_EDITOR
    virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif
};
