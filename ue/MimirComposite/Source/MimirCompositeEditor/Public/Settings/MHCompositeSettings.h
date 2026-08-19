#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "MHCompositeSettings.generated.h"

/** docs/05 section 8: how a texture path outside texture_root is treated. */
UENUM()
enum class EMHTexturePolicy : uint8
{
    /** MH_W_TEXTURE_OUTSIDE_ROOT; the affected material still imports. */
    Transitional,
    /** MH_E_TEXTURE_OUTSIDE_ROOT; the affected material is blocked. */
    Strict
};

/** docs/07 section 6: policy for a three-way material instance conflict. */
UENUM()
enum class EMHConflictPolicy : uint8
{
    /** Ask before touching a locally edited material instance. */
    Prompt,
    /** Source wins. */
    Overwrite,
    /** Local edit wins. */
    Keep
};

/** docs/07 section 10: startup scan behaviour. */
UENUM()
enum class EMHStartupScanMode : uint8
{
    /** Silent auto-import; the contract default. */
    Silent,
    /** Show the plan and wait for Execute. */
    Prompt
};

/** docs/07 section 7: which geometry backend produces UStaticMesh assets. */
UENUM()
enum class EMHGeometryBackend : uint8
{
    /** Direct FBX SDK backend with mandatory Carrier B; the production backend. */
    MhFbx,
    /** UFbxFactory comparison backend; test-only, never a passport fallback. */
    Legacy
};

/**
 * Project settings of docs/07 section 11. Every value is reader-side policy:
 * none of it is written into payloads, and the Ledger location stays editor
 * owned and cannot be pointed into the source tree.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Mimir Composite"))
class MIMIRCOMPOSITEEDITOR_API UMHCompositeSettings final : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UMHCompositeSettings();

    virtual FName GetCategoryName() const override;
    virtual FName GetSectionName() const override;

    /** Absolute scan boundary of the Clean Sources v2 payload set. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FDirectoryPath SourceRoot;

    /** Package root every imported asset is mirrored under. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString ContentRoot;

    /** Package root searched for master materials by shader_class. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString MasterRoot;

    /** Texture basename search boundary; empty means SourceRoot. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FDirectoryPath TextureRoot;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    EMHTexturePolicy TexturePolicy = EMHTexturePolicy::Transitional;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    EMHConflictPolicy ConflictPolicy = EMHConflictPolicy::Prompt;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    EMHStartupScanMode StartupScanMode = EMHStartupScanMode::Silent;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString StaticMeshPrefix;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString MaterialInstancePrefix;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString TexturePrefix;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString CompositeAssetPrefix;

    /** Lumen Mesh Cards budget applied by the finalize stage. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite", meta = (ClampMin = "1"))
    int32 LumenCardsMax = 32;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    EMHGeometryBackend GeometryBackend = EMHGeometryBackend::MhFbx;

    /** SourceRoot as a plain path; empty when the project never configured one. */
    FString GetSourceRootPath() const;

    /** TextureRoot when it is set, otherwise SourceRoot (docs/05 section 2). */
    FString GetEffectiveTextureRootPath() const;
};
