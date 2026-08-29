#pragma once

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "CoreMinimal.h"

namespace UE::MimirComposite
{

struct MIMIRCOMPOSITEEDITOR_API FMHCompositeTransform
{
    FVector TranslationCm = FVector::ZeroVector;
    FQuat RotationQuat = FQuat::Identity;
    FVector Scale = FVector::OneVector;
};

struct MIMIRCOMPOSITEEDITOR_API FMHCompositeNode
{
    EMHCompositeNodeKind Kind = EMHCompositeNodeKind::Group;
    FString Resource;
    FString Name;
    FMHCompositeTransform Transform;
    FString Profile;
    /**
     * Source provenance only (13 §7.1): the authored Dagor `place_type`.
     * INDEX_NONE means the wire field was absent; an explicit 0 is preserved
     * as 0. UE never executes placement, so nothing may branch on this value.
     */
    int32 PlaceType = INDEX_NONE;
    /**
     * Carrier for `ignoreParentInstSeed` (13 §7.2). Consumed since V5-S6.3 by
     * the appearance stage only: it keys AppearanceSignature and never enters
     * the layout stream, the layout preimage, or ResolvedSignature.
     */
    bool bAppearanceSeedBoundary = false;
    TArray<FMHCompositeOption> Options;
    TArray<FMHCompositeNode> Children;
};

struct MIMIRCOMPOSITEEDITOR_API FMHCompositeDocument
{
    TArray<FMHCompositeNode> Nodes;
};

MIMIRCOMPOSITEEDITOR_API bool MHIsCanonicalCompositeToken(const FString& Value);

/** Parse the closed v5 §6 grammar, rejecting duplicate JSON keys. */
MIMIRCOMPOSITEEDITOR_API bool MHParseCompositeV5(
    TConstArrayView<uint8> Bytes,
    FMHCompositeDocument& OutDocument,
    FString& OutError);

/** Emit the §5 byte mode with significant node order and identity elision. */
MIMIRCOMPOSITEEDITOR_API bool MHWriteCanonicalCompositeV5(
    const FMHCompositeDocument& Document,
    TArray<uint8>& OutBytes,
    FString& OutError);

/** Full source-wins apply/extract seams used by import, publish and local-edit detection. */
MIMIRCOMPOSITEEDITOR_API bool MHApplyCompositeV5(
    UMHCompositeAsset& Asset,
    const FMHCompositeDocument& Document,
    TConstArrayView<FMHPlacementProfile> InlinedProfiles,
    FString& OutError);
MIMIRCOMPOSITEEDITOR_API bool MHApplyCompositeV5(
    UMHCompositeAsset& Asset,
    const FMHCompositeDocument& Document,
    FString& OutError);
MIMIRCOMPOSITEEDITOR_API bool MHExtractCompositeV5(
    const UMHCompositeAsset& Asset,
    FMHCompositeDocument& OutDocument,
    FString& OutError);

/** Parse/write one closed `.placement` v1 profile document. */
MIMIRCOMPOSITEEDITOR_API bool MHParsePlacementProfileV1(
    TConstArrayView<uint8> Bytes,
    FMHPlacementProfile& OutProfile,
    FString& OutError);
MIMIRCOMPOSITEEDITOR_API bool MHWriteCanonicalPlacementProfileV1(
    const FMHPlacementProfile& Profile,
    TArray<uint8>& OutBytes,
    FString& OutError);

} // namespace UE::MimirComposite
