#pragma once

#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Random/MHRandomStream.h"
#include "MHCompositeActor.generated.h"

class UActorComponent;
class UMHCompositeAsset;
class USceneComponent;
class UStaticMesh;

/** Persisted level instance of one managed composite; its component view is always derived. */
UCLASS(NotBlueprintable)
class MIMIRCOMPOSITEEDITOR_API AMHCompositeActor final : public AActor
{
    GENERATED_BODY()

public:
    AMHCompositeActor();

    // IsEditorOnly also suppresses the owner's primitive scene proxies in Game
    // View (but not their shadows). Exclude the source actor from cooked loads,
    // not from normal editor rendering/hit proxies. PIE/cook still use the bridge.
    virtual bool NeedsLoadForClient() const override { return false; }
    virtual bool NeedsLoadForServer() const override { return false; }
    virtual void Serialize(FArchive& Archive) override;

    void SetCompositeAsset(UMHCompositeAsset* Asset);
    UMHCompositeAsset* GetCompositeAsset() const;

    int32 GetSeed() const { return Seed; }
    bool GetAutoSeed() const { return bAutoSeed; }
    void SetSeed(int32 NewSeed);
    void Reseed();
    void SetAutoSeed(bool bEnabled);
    static int32 GenerateAutoSeed(int32 DifferentFrom = 0);
    const UE::MimirComposite::FMHResolvedCompositePlan* GetResolvedPlan() const;
    const FString& GetLastPlacementError() const { return LastPlacementError; }
    EMHCompositeSeedEffect GetSeedAffectsResult() const { return SeedAffectsResult; }

    /** Transient authoring session; no edit state is persisted. */
    void SetPlacementEditMode(bool bEnabled);
    bool IsPlacementEditMode() const { return bPlacementEditMode; }
    bool GetEditedCompositeDocument(UE::MimirComposite::FMHCompositeDocument& OutDocument) const;

    /** Rebuild from managed applied assets, never from the source filesystem. */
    void RebuildComposite(bool bRefreshAppliedGraph = true);

    /** A dependency changed since the last admission attempt, not merely a failed plan. */
    bool NeedsDeferredPreviewRefresh() const;

    const TArray<TObjectPtr<UActorComponent>>& GetDerivedComponents() const
    {
        return DerivedComponents;
    }

    /** Root-document authoring handles in source order, without sampled offsets. */
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
    virtual void PostActorCreated() override;
    virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return bPlacementEditMode; }
    virtual void Destroyed() override;

#if WITH_EDITOR
    virtual void PostEditUndo() override;
    virtual void PostEditImport() override;
    virtual bool CanEditChange(const FProperty* InProperty) const override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void ClearDerivedComponents();
    void RebuildPlacement(bool bSeedOnly);
    void UpdatePlacementBasis(USceneComponent*, EUpdateTransformFlags, ETeleportType);
    void AttachRootTransformHook();
    void ReportPlacementError();
    void DeferPreviewForMesh(UStaticMesh& Mesh);

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TObjectPtr<USceneComponent> CompositeRoot;

    /** Soft path is the single persisted identity row for this level instance. */
    UPROPERTY(VisibleInstanceOnly, Category = "Mimir")
    TSoftObjectPtr<UMHCompositeAsset> CompositeAsset;

    /** The sole persisted random input of a placement; explicit zero is valid. */
    UPROPERTY(EditInstanceOnly, Category = "Mimir|Random")
    int32 Seed = 0;

    /** Default duplicate reseeds; explicit Lock/Keep Seed disables it. */
    UPROPERTY(EditInstanceOnly, Category = "Mimir|Random", meta = (DisplayName = "Auto Seed on Duplicate"))
    bool bAutoSeed = true;

    UPROPERTY(VisibleInstanceOnly, Transient, DuplicateTransient, TextExportTransient, Category = "Mimir|Random")
    FString ResolvedSignature;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<UActorComponent>> DerivedComponents;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<USceneComponent>> TopLevelPlacementComponents;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<USceneComponent>> NodePlacementComponents;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<USceneComponent>> LeafPlacementComponents;

    TSet<UE::MimirComposite::FMHResourceKey> PlacementDependencies;
    TArray<FString> LastPlacementWarnings;
    FString LastPlacementError;
    TSharedPtr<const UE::MimirComposite::FMHRandomSourceGraph> AppliedGraph;
    TSharedPtr<const UE::MimirComposite::FMHResolvedCompositePlan> ResolvedPlan;
    TOptional<UE::MimirComposite::FMHRandomSourceGraph> EditingGraph;
    TOptional<UE::MimirComposite::FMHCompositeDocument> EditingDocument;
    TArray<FTransform> LastEditHandleTransforms;
    FTransform LastEditBasis = FTransform::Identity;
    EMHCompositeSeedEffect SeedAffectsResult = EMHCompositeSeedEffect::None;
    bool bPlanAvailable = false;
    uint64 AppliedGraphRevision = 0;
    uint64 LastPreviewAttemptSerial = 0;
    TWeakObjectPtr<UStaticMesh> PendingPreviewMesh;
    // Only a rejected placement basis can recover by reapplying the old plan.
    // A rejected newer definition must pass full graph admission again.
    bool bBasisRejected = false;

    bool bRebuildInProgress = false;
    bool bPlacementEditMode = false;
};
