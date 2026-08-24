#pragma once

#include "Composite/MHCompositeAsset.h"
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
    TArray<FMHCompositeNode> Children;
};

struct MIMIRCOMPOSITEEDITOR_API FMHCompositeDocument
{
    TArray<FMHCompositeNode> Nodes;
};

MIMIRCOMPOSITEEDITOR_API bool MHIsCanonicalCompositeToken(const FString& Value);

/** Parse the closed §6 grammar, rejecting duplicate JSON keys. */
MIMIRCOMPOSITEEDITOR_API bool MHParseCompositeV4(
    TConstArrayView<uint8> Bytes,
    FMHCompositeDocument& OutDocument,
    FString& OutError);

/** Emit the §5 byte mode with significant node order and identity elision. */
MIMIRCOMPOSITEEDITOR_API bool MHWriteCanonicalCompositeV4(
    const FMHCompositeDocument& Document,
    TArray<uint8>& OutBytes,
    FString& OutError);

/** Full source-wins apply/extract seams used by import, publish and local-edit detection. */
MIMIRCOMPOSITEEDITOR_API bool MHApplyCompositeV4(
    UMHCompositeAsset& Asset,
    const FMHCompositeDocument& Document,
    FString& OutError);
MIMIRCOMPOSITEEDITOR_API bool MHExtractCompositeV4(
    const UMHCompositeAsset& Asset,
    FMHCompositeDocument& OutDocument,
    FString& OutError);

} // namespace UE::MimirComposite
