#pragma once

#include "CoreMinimal.h"
#include "Material/MHMaterialProtocol.h"

class UMaterialInstanceConstant;
class UMaterialInterface;
class UTexture;
class UMHCompositeSettings;

namespace UE::MimirComposite
{

class IMHSourceResolver;
struct FMHSourceAnalysisEntry;

struct MIMIRCOMPOSITEEDITOR_API FMHMaterialOperationResult
{
    UMaterialInstanceConstant* Material = nullptr;
    TArray<FString> Warnings;
    FString Error;
    bool bCreated = false;

    bool Succeeded() const { return Material != nullptr && Error.IsEmpty(); }
};

struct MIMIRCOMPOSITEEDITOR_API FMHMaterialAdoptTarget
{
    /** Absolute folder under source_root. */
    FString Folder;
    FString LogicalName;
};

MIMIRCOMPOSITEEDITOR_API bool MHValidateMaterialAdoptTarget(
    const FString& SourceRoot,
    const FMHMaterialAdoptTarget& Target,
    FString& OutPath,
    FString& OutRelativePath,
    FString& OutError);

/**
 * Extract only v4-serializable local state; no inherited values are emitted.
 * Class form (owner decision 2026-09-03): local state the grammar cannot carry
 * (parameter names outside the canonical token grammar, layer-scoped
 * parameters, atlas scalars, unsupported parameter types, base-property
 * overrides other than TwoSided, static parameters, texture slots outside
 * tex0-tex15) is dropped, one human-readable line per dropped item in
 * OutWarnings when supplied. Library form stays strict (docs/10 OPEN-V4-8).
 */
MIMIRCOMPOSITEEDITOR_API bool MHExtractMaterialV4(
    const UMaterialInstanceConstant& Material,
    const UMHCompositeSettings& Settings,
    FMHMaterialDocument& OutDocument,
    FString& OutError,
    TArray<FString>* OutWarnings = nullptr);

/** Full source-wins apply. Texture objects must be supplied for every texture token. */
MIMIRCOMPOSITEEDITOR_API bool MHApplyMaterialV4(
    UMaterialInstanceConstant& Material,
    UMaterialInterface& Parent,
    const FMHMaterialDocument& Document,
    const TMap<FString, UTexture*>& Textures,
    FString& OutError);

/** Create or update the deterministic generated MI in-place and persist its receipt. */
MIMIRCOMPOSITEEDITOR_API FMHMaterialOperationResult MHImportMaterialV4(
    const FMHSourceAnalysisEntry& Entry,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings);

/** Explicit full-overwrite publish; Adopt is required when the MI has no source receipt. */
MIMIRCOMPOSITEEDITOR_API FMHMaterialOperationResult MHPublishMaterialV4(
    UMaterialInstanceConstant& Material,
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings,
    const FMHMaterialAdoptTarget* AdoptTarget = nullptr);

/** True and warning text when current extract differs or cannot round-trip. */
MIMIRCOMPOSITEEDITOR_API bool MHDetectManagedMaterialLocalModification(
    const UMaterialInstanceConstant& Material,
    const UMHCompositeSettings& Settings,
    FString& OutWarning);

#if WITH_DEV_AUTOMATION_TESTS
/** One-shot deterministic seam immediately before the final source commit guard. */
MIMIRCOMPOSITEEDITOR_API void MHSetBeforeMaterialSourceCommitTestHook(TFunction<void()> Hook);
#endif

} // namespace UE::MimirComposite
