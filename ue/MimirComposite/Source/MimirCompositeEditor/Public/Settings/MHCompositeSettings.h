#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "UObject/SoftObjectPath.h"
#include "MHCompositeSettings.generated.h"

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
    /** UFbxFactory comparison backend; test-only, never a production fallback. */
    Legacy
};

/**
 * Source Protocol v4 project settings. File identity never depends on these
 * roots; they only locate source files and registered UE parents.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Mimir Composite"))
class MIMIRCOMPOSITEEDITOR_API UMHCompositeSettings final : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UMHCompositeSettings();

    virtual FName GetCategoryName() const override;
    virtual FName GetSectionName() const override;

    /** Absolute scan boundary of the Source Protocol v4 source tree. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FDirectoryPath SourceRoot;

    /** Package root containing class parents named exactly by class token. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString MasterRoot;

    /** Package root containing strict library parents named by library token. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite|Materials")
    FString LibraryRoot;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    EMHStartupScanMode StartupScanMode = EMHStartupScanMode::Silent;

    /** Ask before an editor command overwrites an existing source document. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite|Source")
    bool bConfirmSourceOverwrite = true;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString StaticMeshPrefix;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString TexturePrefix;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    FString CompositeAssetPrefix;

    /** Source Protocol v4 actor token -> exact actor class. Blender never validates this registry. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite|Actors")
    TMap<FString, FSoftClassPath> ActorClassRegistry;

    /**
     * Package root searched for UPhysicalMaterial assets whose name is exactly a
     * Dagor phmat registry token. A missing asset is a warning, never a block.
     */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite|Collision")
    FString PhysicalMaterialRoot;

    /**
     * First Custom Primitive Data float index a placed composite mesh leaf
     * writes its MH_APPEARANCE_CHANNELS appearance channels into. A material
     * reads them from a Scalar or Vector parameter with Use Custom Primitive
     * Data. Editor preview, PIE and packaged all use this one index.
     */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite|Appearance", meta = (ClampMin = "0"))
    int32 AppearanceCustomDataBaseIndex = 0;

    /** Lumen Mesh Cards budget applied by the finalize stage. */
    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite", meta = (ClampMin = "1"))
    int32 LumenCardsMax = 32;

    UPROPERTY(EditAnywhere, config, Category = "Mimir Composite")
    EMHGeometryBackend GeometryBackend = EMHGeometryBackend::MhFbx;

    /** SourceRoot as a plain path; empty when the project never configured one. */
    FString GetSourceRootPath() const;

};
