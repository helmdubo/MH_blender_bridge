#pragma once

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"
#include "Source/MHSourceResolver.h"

class UMHCompositeSettings;
class UStaticMesh;

namespace UE::MimirComposite
{

/** Unique generated claim by ResourceKey; no source-tree lookup or fallback winner. */
MIMIRCOMPOSITEEDITOR_API UObject* MHLoadAppliedResource(const FMHResourceKey& Key, FString& OutError);

/**
 * One-operation Asset Registry claim snapshot. Duplicate and malformed-class
 * claims anywhere in the project remain visible; no source or durable cache.
 * Reuse within one graph/materialization operation, never across notifications.
 */
class MIMIRCOMPOSITEEDITOR_API FMHAppliedResourceLookup
{
public:
    FMHAppliedResourceLookup();
    ~FMHAppliedResourceLookup();
    UObject* Load(const FMHResourceKey& Key, FString& OutError);

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};

MIMIRCOMPOSITEEDITOR_API bool MHIsSpawnableCompositeActorClass(const UClass* Class);

/** Applied-only input. No filesystem resolver or source-index scan participates. */
MIMIRCOMPOSITEEDITOR_API bool MHBuildAppliedCompositeGraph(
    const UMHCompositeAsset& Root,
    const UMHCompositeSettings& Settings,
    FMHRandomSourceGraph& OutGraph,
    TSet<FMHResourceKey>& OutDependencies,
    FString& OutError,
    bool bAllowBlockingCompilation = true,
    UStaticMesh** OutPendingMesh = nullptr);

/** Visual impact classification; even a visually constant random node still consumes draws. */
MIMIRCOMPOSITEEDITOR_API EMHCompositeSeedEffect MHClassifyCompositeGraph(
    const FMHRandomSourceGraph& Graph);

/** Import-time classification from source-shaped definitions and inlined profiles. */
MIMIRCOMPOSITEEDITOR_API EMHCompositeSeedEffect MHClassifyCompositeDefinition(
    const UMHCompositeAsset& Asset);

} // namespace UE::MimirComposite
