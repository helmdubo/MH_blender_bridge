#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MHCompositeActor.generated.h"

class UActorComponent;
class UMHCompositeAsset;
class USceneComponent;

/** Persisted level instance of one managed composite; its component view is always derived. */
UCLASS(NotBlueprintable)
class MIMIRCOMPOSITEEDITOR_API AMHCompositeActor final : public AActor
{
    GENERATED_BODY()

public:
    AMHCompositeActor();

    void SetCompositeAsset(UMHCompositeAsset* Asset);
    UMHCompositeAsset* GetCompositeAsset() const;

    /** Force a source-keyed rebuild of the transient component view. */
    UFUNCTION(CallInEditor, Category = "Mimir")
    void RebuildComposite();

    const TArray<TObjectPtr<UActorComponent>>& GetDerivedComponents() const
    {
        return DerivedComponents;
    }

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostLoad() override;

#if WITH_EDITOR
    virtual void PostEditUndo() override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void ClearDerivedComponents();

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TObjectPtr<USceneComponent> CompositeRoot;

    /** Soft path is the single persisted identity row for this level instance. */
    UPROPERTY(EditAnywhere, Category = "Mimir")
    TSoftObjectPtr<UMHCompositeAsset> CompositeAsset;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<UActorComponent>> DerivedComponents;

    bool bRebuildInProgress = false;
};

namespace UE::MimirComposite
{

/** Notify loaded level instances only after a managed composite commit succeeds. */
MIMIRCOMPOSITEEDITOR_API void MHNotifyCompositeAssetChanged(UMHCompositeAsset& Asset);

} // namespace UE::MimirComposite
