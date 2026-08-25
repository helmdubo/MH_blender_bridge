#pragma once

#include "CoreMinimal.h"
#include "EditorFramework/AssetImportData.h"
#include "MHStaticMeshImportData.generated.h"

namespace UE::MimirComposite
{

/** Current owner-ratified static-mesh build semantics. */
inline constexpr int32 MHStaticMeshImporterVersion = 2;

/**
 * Suppresses managed-mesh local-edit tracking for importer-owned mutations.
 *
 * Keep this guard alive while rebuilding a UStaticMesh and while updating its
 * receipt. Guards may be nested.
 */
class MIMIRCOMPOSITEEDITOR_API FMHScopedStaticMeshImportMutation final
{
public:
    FMHScopedStaticMeshImportMutation();
    ~FMHScopedStaticMeshImportMutation();

    FMHScopedStaticMeshImportMutation(const FMHScopedStaticMeshImportMutation&) = delete;
    FMHScopedStaticMeshImportMutation& operator=(const FMHScopedStaticMeshImportMutation&) = delete;
    FMHScopedStaticMeshImportMutation(FMHScopedStaticMeshImportMutation&&) = delete;
    FMHScopedStaticMeshImportMutation& operator=(FMHScopedStaticMeshImportMutation&&) = delete;
};

/** True only while importer-owned static-mesh mutations are being applied. */
MIMIRCOMPOSITEEDITOR_API bool MHIsStaticMeshImportMutationSuppressed();

} // namespace UE::MimirComposite

/** Applied-source receipt persisted on each managed UStaticMesh. */
UCLASS(BlueprintType, EditInlineNew)
class MIMIRCOMPOSITEEDITOR_API UMHStaticMeshImportData final : public UAssetImportData
{
    GENERATED_BODY()

public:
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString LogicalName;

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString SourceRelativePath;

    /** Raw BLAKE3-160 of the FBX bytes last applied by UE. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    FString SourceHash;

    /** Monotonic code version of the build semantics used for this receipt. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    int32 ImporterVersion = 0;

    /** Best-effort editor hook observed a local mutation after the last apply. */
    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    bool bLocallyModified = false;

#if WITH_EDITOR
    virtual void AppendAssetRegistryTags(FAssetRegistryTagsContext Context) override;
#endif
};
