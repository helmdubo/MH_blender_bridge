#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MHCompositeAsset.generated.h"

UENUM()
enum class EMHCompositeSeedEffect : uint8
{
    None,
    ChildSeedsOnly,
    Transform,
    Topology
};

UENUM()
enum class EMHCompositeNodeKind : uint8
{
    Mesh,
    Actor,
    Composite,
    Group,
    Random,
    // Append only: existing serialized ordinals are frozen.
    GameObj
};

UENUM()
enum class EMHCompositeOptionKind : uint8
{
    Mesh,
    Actor,
    Composite,
    Empty,
    // Append only: existing serialized ordinals are frozen.
    GameObj
};

USTRUCT()
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeOption
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    EMHCompositeOptionKind Kind = EMHCompositeOptionKind::Empty;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString Resource;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    float Weight = 0.0f;
};

USTRUCT()
struct MIMIRCOMPOSITEEDITOR_API FMHPlacementRange
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    float Base = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    float Deviation = 0.0f;
};

/** One parsed .placement v1 payload, inlined into its dependent composite. */
USTRUCT()
struct MIMIRCOMPOSITEEDITOR_API FMHPlacementProfile
{
    GENERATED_BODY()

public:
    /** Import-only receipt; deliberately excluded from source JSON and Asset Registry tags. */
    const FString& GetAppliedSourceHash() const { return AppliedSourceHash; }
    void SetAppliedSourceHash(FString InHash) { AppliedSourceHash = MoveTemp(InHash); }

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString LogicalName;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    bool bHasOffsetCm = false;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TArray<FMHPlacementRange> OffsetCm;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    bool bHasRotationDeg = false;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TArray<FMHPlacementRange> RotationDeg;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    bool bHasUniformScale = false;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FMHPlacementRange UniformScale;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    bool bHasVerticalScale = false;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FMHPlacementRange VerticalScale;

private:
    /** Raw hash of the exact .placement bytes applied by the last successful import. */
    UPROPERTY(meta = (AllowPrivateAccess = "true"))
    FString AppliedSourceHash;
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

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString Profile;

    /** Source provenance only: authored Dagor place_type; INDEX_NONE means absent. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    int32 PlaceType = INDEX_NONE;

    /** Source provenance only: authored ignoreParentInstSeed; no consumer before V5-S6.3. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    bool bAppearanceSeedBoundary = false;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TArray<FMHCompositeOption> Options;

    /** Inline placement-v1 body (owner revision of OPEN-V5-15, 2026-08-31). */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    bool bHasInlinePlacement = false;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FMHPlacementProfile InlinePlacement;
};

/** Editor-only managed representation of one Source Protocol v5 composite. */
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

    /** Source-only profiles resolved at import; no placement-profile UAsset exists. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TArray<FMHPlacementProfile> InlinedPlacementProfiles;

    /** Derived import classification; never part of source JSON, hashes or registry tags. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    EMHCompositeSeedEffect SeedAffectsResult = EMHCompositeSeedEffect::None;

#if WITH_EDITOR
    virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif
};
