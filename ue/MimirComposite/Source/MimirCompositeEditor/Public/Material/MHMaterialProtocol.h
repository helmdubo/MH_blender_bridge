#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

struct FMHUnrealMaterialInstanceData;

enum class EMHMaterialMode : uint8
{
    Class,
    Library,
    UnrealInstance
};

struct MIMIRCOMPOSITEEDITOR_API FMHMaterialParameter
{
    bool bString = false;
    bool bBool = false;
    bool bVector = false;
    float Scalar = 0.0f;
    FVector4f Vector = FVector4f::Zero();
    FString String;
    bool Bool = false;
};

/** Strict, lossless-in-domain representation of a Source Protocol v4 material. */
struct MIMIRCOMPOSITEEDITOR_API FMHMaterialDocument
{
    EMHMaterialMode Mode = EMHMaterialMode::Class;
    FString Parent;
    bool bHasTwoSided = false;
    bool bTwoSided = false;
    TMap<int32, FString> Textures;
    TMap<FString, FMHMaterialParameter> Params;
    /** Explicit UE 5.7 instance snapshot; absent for unchanged class/library forms. */
    TSharedPtr<FMHUnrealMaterialInstanceData> UnrealInstance;
};

MIMIRCOMPOSITEEDITOR_API bool MHIsCanonicalMaterialToken(const FString& Value);

/** Parse the closed §5 grammar, including its texture-reference diagnostic. */
MIMIRCOMPOSITEEDITOR_API bool MHParseMaterialV4(
    TConstArrayView<uint8> Bytes,
    FMHMaterialDocument& OutDocument,
    FString& OutError);

/** Emit the exact shared canonical byte form: UTF-8, 2-space indent, LF + final LF. */
MIMIRCOMPOSITEEDITOR_API bool MHWriteCanonicalMaterialV4(
    const FMHMaterialDocument& Document,
    TArray<uint8>& OutBytes,
    FString& OutError);

} // namespace UE::MimirComposite
