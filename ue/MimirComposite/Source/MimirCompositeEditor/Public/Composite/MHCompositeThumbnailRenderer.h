#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "ThumbnailRendering/DefaultSizedThumbnailRenderer.h"
#include "MHCompositeThumbnailRenderer.generated.h"

class UMHCompositeAsset;
class FThumbnailPreviewScene;
struct FMHCompositeThumbnailCache;
namespace UE::MimirComposite { struct FMHResourceKey; struct FMHResolvedCompositePlan; }

/** Native Content Browser thumbnail, isolated from level placements and proof. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHCompositeThumbnailRenderer final : public UDefaultSizedThumbnailRenderer
{
    GENERATED_BODY()
public:
    UMHCompositeThumbnailRenderer();
    virtual ~UMHCompositeThumbnailRenderer() override;
    virtual bool CanVisualizeAsset(UObject* Object) override;
    virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
        FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily) override;
    virtual void BeginDestroy() override;
    void ReleaseResources();
    void InvalidateResource(const UE::MimirComposite::FMHResourceKey& Key);

#if WITH_DEV_AUTOMATION_TESTS
    const UE::MimirComposite::FMHResolvedCompositePlan* GetPreparedPlanForTests(UMHCompositeAsset* Asset) const;
    const FThumbnailPreviewScene* GetPreviewSceneForTests() const;
    bool HasRequestedMeshForTests(const FString& LogicalName) const;
    bool FlushAsyncLoadsForTests();
    bool TickPendingForTests() { return TickPending(0.0f); }
#endif

private:
    bool TickPending(float DeltaTime);
    void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
    TUniquePtr<FMHCompositeThumbnailCache> Cache;
    FTSTicker::FDelegateHandle PendingTicker;
    FDelegateHandle PropertyChangedHandle;
};

namespace UE::MimirComposite
{
/** Invalidates only observed thumbnails; never creates a renderer or touches placements. */
void MHInvalidateCompositeThumbnails(const FMHResourceKey& Key);
void MHReleaseCompositeThumbnails();
}
