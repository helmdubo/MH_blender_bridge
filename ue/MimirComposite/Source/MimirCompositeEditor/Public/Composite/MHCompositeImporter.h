#pragma once

#include "Composite/MHCompositeCompiler.h"
#include "CoreMinimal.h"

class UMHCompositeSettings;

namespace UE::MimirComposite
{

class IMHSourceResolver;
struct FMHSourceAnalysisEntry;

struct MIMIRCOMPOSITEEDITOR_API FMHCompositeOperationResult
{
    UMHCompositeAsset* Asset = nullptr;
    TArray<FString> Warnings;
    FString Error;
    bool bCreated = false;

    bool Succeeded() const { return Asset != nullptr && Error.IsEmpty(); }
};

struct MIMIRCOMPOSITEEDITOR_API FMHCompositeAdoptTarget
{
    /** Absolute folder under source_root. */
    FString Folder;
    FString LogicalName;
};

MIMIRCOMPOSITEEDITOR_API bool MHValidateCompositeAdoptTarget(
    const FString& SourceRoot,
    const FMHCompositeAdoptTarget& Target,
    FString& OutPath,
    FString& OutRelativePath,
    FString& OutError);

MIMIRCOMPOSITEEDITOR_API bool MHDetectManagedCompositeLocalModification(
    const UMHCompositeAsset& Asset,
    FString& OutWarning);

/** Resolve/build first, then create/update the deterministic generated asset in-place. */
MIMIRCOMPOSITEEDITOR_API FMHCompositeOperationResult MHImportCompositeV5(
    const FMHSourceAnalysisEntry& Entry,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings);

/** Full-overwrite publish. Adopt is required for an unmanaged asset. */
MIMIRCOMPOSITEEDITOR_API FMHCompositeOperationResult MHPublishCompositeV5(
    UMHCompositeAsset& Asset,
    const FString& SourceRoot,
    const FMHCompositeAdoptTarget* AdoptTarget = nullptr);

#if WITH_DEV_AUTOMATION_TESTS
MIMIRCOMPOSITEEDITOR_API void MHSetBeforeCompositeSourceCommitTestHook(TFunction<void()> Hook);
#endif

} // namespace UE::MimirComposite
