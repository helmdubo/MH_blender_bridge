#pragma once

#include "CoreMinimal.h"
#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "MHCompositeThumbnailRenderer.generated.h"

class FCanvas;
class FMHCompositeThumbnailScene;
class FRenderTarget;

/** Live Content Browser preview of the resolved mesh placement of a composite. */
UCLASS()
class UMHCompositeThumbnailRenderer final : public UDefaultSizedThumbnailRenderer
{
    GENERATED_BODY()

public:
    virtual void Draw(
        UObject* Object,
        int32 X,
        int32 Y,
        uint32 Width,
        uint32 Height,
        FRenderTarget* RenderTarget,
        FCanvas* Canvas,
        bool bAdditionalViewFamily) override;
    virtual bool CanVisualizeAsset(UObject* Object) override;
    virtual EThumbnailRenderFrequency GetThumbnailRenderFrequency(UObject* Object) const override;
    virtual void BeginDestroy() override;

private:
    FMHCompositeThumbnailScene* ThumbnailScene = nullptr;
};
