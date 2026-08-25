#pragma once

#include "Composite/MHCompositePlacementEvents.h"
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

    /** Transient seam used by the S6 Edit operation; no edit state is persisted. */
    void SetPlacementEditMode(bool bEnabled);
    bool IsPlacementEditMode() const { return bPlacementEditMode; }

    /** Force a source-keyed rebuild of the transient component view. */
    void RebuildComposite();

    const TArray<TObjectPtr<UActorComponent>>& GetDerivedComponents() const
    {
        return DerivedComponents;
    }

    /** Root-document node components in authored order, for the future Edit session. */
    const TArray<TObjectPtr<USceneComponent>>& GetTopLevelPlacementComponents() const
    {
        return TopLevelPlacementComponents;
    }

    const TArray<TObjectPtr<USceneComponent>>& GetTopLevelComponents() const
    {
        return TopLevelPlacementComponents;
    }

    const TArray<FString>& GetLastPlacementWarnings() const
    {
        return LastPlacementWarnings;
    }

    const TArray<FString>& GetPlacementWarnings() const
    {
        return LastPlacementWarnings;
    }

    bool DependsOnResource(const UE::MimirComposite::FMHResourceKey& Key) const;

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
    UPROPERTY(VisibleInstanceOnly, Category = "Mimir")
    TSoftObjectPtr<UMHCompositeAsset> CompositeAsset;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<UActorComponent>> DerivedComponents;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<USceneComponent>> TopLevelPlacementComponents;

    TSet<UE::MimirComposite::FMHResourceKey> PlacementDependencies;
    TArray<FString> LastPlacementWarnings;

    bool bRebuildInProgress = false;
    bool bPlacementEditMode = false;
};
