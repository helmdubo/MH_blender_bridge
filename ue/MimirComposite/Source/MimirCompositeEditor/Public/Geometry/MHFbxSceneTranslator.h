#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

struct FMHSceneIR;

/** Blender FBX bytes -> neutral Source Protocol v4 scene. */
class MIMIRCOMPOSITEEDITOR_API IMHGeometryTranslator
{
public:
    virtual ~IMHGeometryTranslator() = default;

    /** Parse the exact captured bytes; implementations never reopen source paths. */
    virtual bool Translate(
        const FString& ResourceName,
        TConstArrayView<uint8> SourceBytes,
        FMHSceneIR& OutScene,
        FString& OutError) = 0;
};

/** Direct Autodesk FBX SDK implementation of IMHGeometryTranslator. */
class MIMIRCOMPOSITEEDITOR_API FMHFbxSceneTranslator final : public IMHGeometryTranslator
{
public:
    virtual bool Translate(
        const FString& ResourceName,
        TConstArrayView<uint8> SourceBytes,
        FMHSceneIR& OutScene,
        FString& OutError) override;
};

} // namespace UE::MimirComposite
