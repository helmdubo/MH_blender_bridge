#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceConstant;
class UPhysicalMaterial;
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
    /**
     * Dagor phmat token -> resolved UPhysicalMaterial. A token the project has
     * no asset for is simply absent; the builder never blocks on that.
     */
    TMap<FString, UPhysicalMaterial*> PhysicalMaterials;

    bool IsValid() const { return Scene != nullptr; }
};

/**
 * Side products of one rebuild that the caller owns: the companion trace mesh
 * needs the same save/rollback treatment as the managed mesh itself.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHStaticMeshRebuildOutputs
{
    TArray<FString> Warnings;
    /** Companion UStaticMesh bound as ComplexCollisionMesh; needs saving. */
    UStaticMesh* TraceCollisionMesh = nullptr;
    bool bCreatedTraceCollisionMesh = false;
    /** Companion left over from a previous apply that no longer has trace nodes. */
    UStaticMesh* StaleTraceCollisionMesh = nullptr;
};

/** Full source-wins replacement of every managed UStaticMesh domain. */
class MIMIRCOMPOSITEEDITOR_API FMHStaticMeshBuilder final
{
public:
    static bool Rebuild(
        UStaticMesh& StaticMesh,
        const FMHStaticMeshBuildPlan& Plan,
        FString& OutError,
        FMHStaticMeshRebuildOutputs* OutOutputs = nullptr);
};

/**
 * Package root of the companion trace meshes bound through
 * UStaticMesh::ComplexCollisionMesh. Deliberately outside the managed mesh root
 * so a source resource literally named "<name>_trace" can never collide with a
 * companion.
 */
MIMIRCOMPOSITEEDITOR_API FString MHTraceCollisionMeshPackageName(const FString& LogicalName);

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
