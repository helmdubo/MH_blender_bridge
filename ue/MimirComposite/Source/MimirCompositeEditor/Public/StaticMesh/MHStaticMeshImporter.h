#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceConstant;
class UStaticMesh;

namespace UE::MimirComposite
{

class IMHSourceResolver;
struct FMHSceneIR;
struct FMHSourceAnalysisEntry;

struct MIMIRCOMPOSITEEDITOR_API FMHStaticMeshOperationResult
{
    UStaticMesh* StaticMesh = nullptr;
    TArray<FString> Warnings;
    FString Error;
    bool bCreated = false;
    bool bRebuilt = false;
    bool bReceiptUpdated = false;

    bool Succeeded() const { return StaticMesh != nullptr && Error.IsEmpty(); }
};

/** Fully resolved, mutation-free input to the UStaticMesh builder. */
struct MIMIRCOMPOSITEEDITOR_API FMHStaticMeshBuildPlan
{
    const FMHSceneIR* Scene = nullptr;
    TMap<FString, UMaterialInstanceConstant*> Materials;

    bool IsValid() const { return Scene != nullptr; }
};

/** Full source-wins replacement of every managed UStaticMesh domain. */
class MIMIRCOMPOSITEEDITOR_API FMHStaticMeshBuilder final
{
public:
    static bool Rebuild(
        UStaticMesh& StaticMesh,
        const FMHStaticMeshBuildPlan& Plan,
        FString& OutError);
};

/**
 * Create/update the deterministic managed mesh. bForceReimport bypasses the
 * equal raw-hash/importer-version NO_CHANGE fast path.
 */
MIMIRCOMPOSITEEDITOR_API FMHStaticMeshOperationResult MHImportStaticMeshV4(
    const FMHSourceAnalysisEntry& Entry,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    bool bForceReimport = false);

} // namespace UE::MimirComposite
