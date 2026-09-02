#pragma once

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"
#include "Source/MHSourceResolver.h"

class UMHCompositeSettings;

namespace UE::MimirComposite
{

/** Proof-plane uniqueness/path check for one generated ResourceKey claim. */
MIMIRCOMPOSITEEDITOR_API bool MHCheckGeneratedAssetClaims(
    const FMHResourceKey& Key,
    FString& OutError);

MIMIRCOMPOSITEEDITOR_API bool MHIsSpawnableCompositeActorClass(const UClass* Class);

/** Applied-only input. No filesystem resolver or source-index scan participates. */
MIMIRCOMPOSITEEDITOR_API bool MHBuildAppliedCompositeGraph(
    const UMHCompositeAsset& Root,
    const UMHCompositeSettings& Settings,
    FMHRandomSourceGraph& OutGraph,
    TSet<FMHResourceKey>& OutDependencies,
    FString& OutError);

/** Visual impact classification; even a visually constant random node still consumes draws. */
MIMIRCOMPOSITEEDITOR_API EMHCompositeSeedEffect MHClassifyCompositeGraph(
    const FMHRandomSourceGraph& Graph);

/** Import-time classification from source-shaped definitions and inlined profiles. */
MIMIRCOMPOSITEEDITOR_API EMHCompositeSeedEffect MHClassifyCompositeDefinition(
    const UMHCompositeAsset& Asset);

} // namespace UE::MimirComposite
