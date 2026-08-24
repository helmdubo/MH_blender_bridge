#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MHCompositeAsset.generated.h"

UENUM()
enum class EMHCompositeNodeKind : uint8
{
    Mesh,
    Actor,
    Composite,
    Group
};

/** Persisted source-shaped node. ParentIndex preserves authored pre-order. */
USTRUCT()
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeAssetNode
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    int32 ParentIndex = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    EMHCompositeNodeKind Kind = EMHCompositeNodeKind::Group;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString Resource;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString Name;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FTransform Transform = FTransform::Identity;
};

/** Editor-only managed representation of one Source Protocol v4 composite. */
UCLASS(BlueprintType)
class MIMIRCOMPOSITEEDITOR_API UMHCompositeAsset final : public UDataAsset
{
    GENERATED_BODY()

public:
    virtual bool IsEditorOnly() const override { return true; }

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString LogicalName;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString SourceRelativePath;

    /** Raw BLAKE3-160 of source bytes last applied by UE. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString SourceHash;

    /** BLAKE3-160 of canonical JSON extracted immediately after apply. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString AppliedHash;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TArray<FMHCompositeAssetNode> Nodes;

#if WITH_EDITOR
    virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif
};
