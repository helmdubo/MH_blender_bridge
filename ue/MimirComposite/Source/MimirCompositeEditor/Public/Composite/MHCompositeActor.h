#pragma once

#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Random/MHRandomStream.h"
#include "MHCompositeActor.generated.h"

class UActorComponent;
class UMHCompositeAsset;
class USceneComponent;
class UStaticMesh;
namespace UE::MimirComposite { struct FMHEndpointInterfaceDelta; }

/** Persisted level instance of one managed composite; its component view is always derived. */
/**
 * Persisted call context of a placement (R4-pre-3, 16 §2.10). Empty for a
 * placement authored as a root; filled by Break for a child composite so the
 * child keeps the streams it had inside its parent, and by Make Unique for
 * This Placement (R6-U1) so the placement keeps its root-level streams under
 * the unique root. Never computed from the scene; only those two writers (or
 * cleared by the user).
 */
USTRUCT()
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeCallContext
{
    GENERATED_BODY()

    /** Format version of this record; 1 = namespace + appearance boundary. */
    UPROPERTY()
    int32 Version = 1;
    /** NodePath the recipe root is walked under; empty = own root. */
    UPROPERTY(VisibleInstanceOnly, Category = "Mimir|Random")
    FString StreamNamespace;
    /** Appearance boundary for leaves without a declared one; empty = own root. */
    UPROPERTY(VisibleInstanceOnly, Category = "Mimir|Random")
    FString AppearanceBoundary;

    bool IsEmpty() const { return StreamNamespace.IsEmpty() && AppearanceBoundary.IsEmpty(); }
    UE::MimirComposite::FMHResolveCallContext ToResolveContext() const
    {
        UE::MimirComposite::FMHResolveCallContext Result;
        Result.NodePathPrefix = StreamNamespace;
        Result.AppearanceBoundaryPath = AppearanceBoundary;
        return Result;
    }
};

UCLASS(NotBlueprintable)
class MIMIRCOMPOSITEEDITOR_API AMHCompositeActor final : public AActor
{
    GENERATED_BODY()

public:
    AMHCompositeActor();

    // IsEditorOnly suppresses primitive rendering in Game View as well as cook.
    // Keep ordinary editor rendering/native HActor picking; the existing bridge
    // replaces this source placement at PIE/cook handoff.
    virtual bool NeedsLoadForClient() const override { return false; }
    virtual bool NeedsLoadForServer() const override { return false; }
    virtual void Serialize(FArchive& Archive) override;

    void SetCompositeAsset(UMHCompositeAsset* Asset);
    UMHCompositeAsset* GetCompositeAsset() const;

    const FMHCompositeCallContext& GetCallContext() const { return CallContext; }
    /** Rebuilds the preview under the new context. */
    void SetCallContext(const FMHCompositeCallContext& NewContext);

    int32 GetSeed() const { return Seed; }
    bool GetAutoSeed() const { return bAutoSeed; }
    void SetSeed(int32 NewSeed);
    void Reseed();
    void SetAutoSeed(bool bEnabled);
    static int32 GenerateAutoSeed(int32 DifferentFrom = 0);

    /**
     * Stored value only. There is deliberately no computed default and no
     * fallback here: a getter that derived from Seed would silently reroll the
     * appearance on every layout reseed (§3, acceptance 5).
     */
    int32 GetAppearanceSeed() const { return AppearanceSeed; }
    bool GetAutoAppearanceSeed() const { return bAutoAppearanceSeed; }
    void SetAppearanceSeed(int32 NewSeed);
    void ReseedAppearance();
    void SetAutoAppearanceSeed(bool bEnabled);
    /** True once this placement owns a stored AppearanceSeed (migrated or authored). */
    bool HasStoredAppearanceSeed() const { return bAppearanceSeedStored; }

    /**
     * Resident preview plan for the current seeds (R2b-2/R2b-3): Layout +
     * Appearance on the recipe graph, no closure, no signature. Null while no
     * preview is built or while the last build failed. Proof lives in
     * UMHProofCacheSubsystem.
     */
    const UE::MimirComposite::FMHResolvedCompositePlan* GetResolvedPlan() const;
    const FString& GetLastPlacementError() const { return LastPlacementError; }
    EMHCompositeSeedEffect GetSeedAffectsResult() const { return SeedAffectsResult; }
    /** True while a resolved candidate waits for all selected mesh endpoints. */
    bool IsPreviewLoading() const { return PendingPlacementPlan.IsValid(); }

    /** Transient authoring session; no edit state is persisted. */
    void SetPlacementEditMode(bool bEnabled);
    bool IsPlacementEditMode() const { return bPlacementEditMode; }
    /** The edited definition's document: the root's, or the nested definition's under the edit scope (R6-D1). */
    bool GetEditedCompositeDocument(UE::MimirComposite::FMHCompositeDocument& OutDocument) const;
    /**
     * R6-D1 (docs/16 §2.7): scope of the next Placement Edit session — the
     * definition invoked at InvocationNodePath of the resident plan (empty =
     * the root definition). Its nodes get handles under the invocation's
     * effective world transform; handle edits write the nested draft.
     */
    void SetEditScope(const FString& InvocationNodePath);
    const FString& GetEditScopeInvocationPath() const { return EditScopeInvocationPath; }
    /** Handles of the scoped definition's nodes while a nested session is active (empty for a root session). */
    const TArray<TObjectPtr<USceneComponent>>& GetEditScopeHandles() const { return EditScopeHandles; }
    /**
     * R6-UX1: the session handle that moves the node at NodePath — the scope
     * handle of its top-level ancestor inside the edited definition, or the
     * top-level placement handle in a root session. Null outside a session
     * or for paths outside the edited subtree.
     */
    USceneComponent* FindSessionHandleForNodePath(const FString& NodePath) const;
    /** World bounds of everything the edited definition materializes here (handles and leaves); invalid without a scope. */
    FBox GetEditScopeBounds() const;
    /** Wireframe frame around the edited subtree while a nested session is active. */
    UBoxComponent* GetEditScopeFrame() const { return EditScopeFrame; }

    /** Rebuild from managed applied assets, never from the source filesystem. */
    void RebuildComposite();

    /** Instrumentation only: full placement rebuilds performed by this actor. */
    uint32 GetPlacementRebuildCount() const { return PlacementRebuildCount; }
    /** Increments on every successful preview build or edit-session step (R2b-2); zero before the first. */
    uint32 GetPreviewRevision() const { return PreviewRevision; }

    /** Instrumentation only: rebuilds forced by a fail-closed state desync. */
    uint32 GetPlacementDesyncCount() const { return PlacementDesyncCount; }

    /** Instrumentation only: rebuilds entered before this actor was registered. */
    uint32 GetPlacementUnregisteredBuildCount() const { return PlacementUnregisteredBuildCount; }

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

    /** Materialized leaves in plan order, index-aligned with Plan.Leaves. */
    const TArray<TObjectPtr<USceneComponent>>& GetLeafPlacementComponents() const
    {
        return LeafPlacementComponents;
    }

    /**
     * Plan-aligned component/instance rows used by viewport and Outliner
     * navigation. Pooled rows are refreshed to their current ISM address
     * (16 §2.8: the handle is the identity, the index moves under swap-remove).
     */
    const TArray<UE::MimirComposite::FMHCompositeLeafMaterialization>&
        GetLeafMaterializations() const
    {
        RefreshPooledRows();
        return LeafMaterializations;
    }

    /** Row rendered by (Component, InstanceIndex); pooled ISM addresses resolve through the pool. */
    const UE::MimirComposite::FMHCompositeLeafMaterialization* FindLeafMaterialization(
        const USceneComponent* Component, int32 InstanceIndex = INDEX_NONE) const;
    /** Row of the leaf materialized at NodePath, or nullptr. */
    const UE::MimirComposite::FMHCompositeLeafMaterialization* FindLeafMaterializationByNodePath(const FString& NodePath) const;

    /** Navigation selection only; source and resolved-plan authority are untouched. */
    bool SelectPlacementLeaf(const USceneComponent* Component, int32 InstanceIndex = INDEX_NONE);
    bool SelectPlacementLeafByNodePath(const FString& NodePath);
    /** Nearest enclosing composite occurrence for an exact resident leaf; empty means the root occurrence. */
    bool FindPlacementOccurrenceForLeafPath(const FString& LeafPath, FString& OutOccurrencePath) const;
    /** Clears transient viewport leaf context while preserving ordinary actor selection. */
    void ClearPlacementLeafSelection();
    const FString& GetSelectedPlacementLeafPath() const { return SelectedPlacementLeafPath; }
    const FString& GetSelectedPlacementOccurrencePath() const { return SelectedPlacementOccurrencePath; }
    /** Pool selection predicate: highlight only the selected object; direct actor selection highlights all. Edit scope is separate. */
    bool ShouldHighlightPlacementLeafPath(const FString& NodePath) const;

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
    virtual void PostRegisterAllComponents() override;
    virtual void PostActorCreated() override;
    virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return bPlacementEditMode; }
    virtual void Destroyed() override;
    /** Own components plus the pooled instances of this placement (16 §2.8): F / focus frames the whole placement. */
    virtual FBox GetComponentsBoundingBox(bool bNonColliding = false, bool bIncludeFromChildActors = false) const override;

#if WITH_EDITOR
    virtual void PreEditUndo() override;
    virtual void PostEditUndo() override;
    virtual void PostEditUndo(TSharedPtr<ITransactionObjectAnnotation> TransactionAnnotation) override;
    virtual void PostEditImport() override;
    virtual void SetIsTemporarilyHiddenInEditor(bool bIsHidden) override;
    virtual bool CanEditChange(const FProperty* InProperty) const override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual bool GetReferencedContentObjects(TArray<UObject*>& Objects) const override;
#endif

private:
    friend void UE::MimirComposite::MHNotifyGeneratedResourceChanged(const UE::MimirComposite::FMHResourceKey& Key);
    void ReconcileEndpoint(const UE::MimirComposite::FMHResourceKey& Key,
        const UE::MimirComposite::FMHEndpointInterfaceDelta& Delta);
    void ReconcileRecipe(const UE::MimirComposite::FMHResourceKey& Key);
    TArray<TObjectPtr<UActorComponent>> CollectPreviousDerivedComponents() const;
    void ClearDerivedComponents();
#if WITH_EDITOR
    void RestorePlacementAfterUndo();
#endif
    void RebuildPlacement(bool bSeedOnly, bool bRecipeChanged = false);
    void CancelPendingPlacement();
    /** Revalidates transient viewport selection against the newly committed plan/view. */
    void PrunePlacementLeafSelection();
    bool PendingEndpointsSettled(FString& OutError);
    void CommitPendingPlacement();
    void OnEndpointLoadReady(const UE::MimirComposite::FMHResourceKey& Key);
    void UpdatePlacementBasis(USceneComponent*, EUpdateTransformFlags, ETeleportType);
    void AttachRootTransformHook();
    void ReportPlacementError();
    void RefreshPooledRows() const;
    /** Pooled instances follow the actor's editor visibility (16 §2.8 owner operations). */
    void SyncPoolVisibility();

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TObjectPtr<USceneComponent> CompositeRoot;

    /** Soft path is the single persisted identity row for this level instance. */
    UPROPERTY(VisibleInstanceOnly, Category = "Mimir")
    TSoftObjectPtr<UMHCompositeAsset> CompositeAsset;

#if WITH_EDITORONLY_DATA
    /** Package-loading hints for the last committed selected view. Never used
     * as recipe identity, freshness proof or authoritative layout. Older maps
     * have an empty list and stream selected endpoints normally. */
    UPROPERTY()
    TArray<TObjectPtr<UStaticMesh>> SelectedMeshDependencies;
#endif

    /**
     * Layout random input; explicit zero is valid. The serialized name stays
     * `Seed` for level compatibility: only the displayed name changed (§3).
     */
    UPROPERTY(EditInstanceOnly, Category = "Mimir|Random", meta = (DisplayName = "Layout Seed"))
    int32 Seed = 0;

    /** R4-pre-3: where this placement's streams are keyed; see FMHCompositeCallContext. */
    UPROPERTY(VisibleInstanceOnly, AdvancedDisplay, Category = "Mimir|Random", meta = (DisplayName = "Call Context"))
    FMHCompositeCallContext CallContext;

    /** Default duplicate reseeds; explicit Lock/Keep Seed disables it. */
    UPROPERTY(EditInstanceOnly, Category = "Mimir|Random", meta = (DisplayName = "Auto Seed on Duplicate"))
    bool bAutoSeed = true;

    /** Second, independent random input: appearance channels only (§3). */
    UPROPERTY(EditInstanceOnly, Category = "Mimir|Random", meta = (DisplayName = "Appearance Seed"))
    int32 AppearanceSeed = 0;

    UPROPERTY(EditInstanceOnly, Category = "Mimir|Random", meta = (DisplayName = "Auto Appearance Seed on Duplicate"))
    bool bAutoAppearanceSeed = true;

    /**
     * Distinguishes "this placement has no serialized AppearanceSeed" from
     * "its serialized AppearanceSeed happens to be 0". Both this flag and
     * AppearanceSeed equal their archetype defaults on a pre-S6.3 level, so a
     * legacy record carries neither tagged property and loads as not-stored.
     */
    UPROPERTY()
    bool bAppearanceSeedStored = false;


    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<UActorComponent>> DerivedComponents;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<USceneComponent>> TopLevelPlacementComponents;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<USceneComponent>> LeafPlacementComponents;

    /** R6-D1: handles of the nested definition's nodes under the edit scope. */
    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<USceneComponent>> EditScopeHandles;
    /** Resident-plan node path per scope handle (parallel to EditScopeHandles). */
    TArray<FString> EditScopeHandlePaths;
    /** R6-UX1: the frame marking the edited subtree in the scene. */
    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TObjectPtr<UBoxComponent> EditScopeFrame;
    FString EditScopeInvocationPath;
    /** Logical name of the scoped definition and the invocation's world in actor space (resident plan). */
    FString EditScopeComposite;
    FMatrix EditScopeParentLocal = FMatrix::Identity;
    /** Creates/positions the scope handles from the resident plan; destroys them when no scope is active. */
    void SyncEditScopeHandles();
    void SyncEditScopeFrame();
    void DestroyEditScopeHandles();

    /** Derived navigation rows; own components are retained by DerivedComponents, pooled ones by the pool. */
    mutable TArray<UE::MimirComposite::FMHCompositeLeafMaterialization> LeafMaterializations;

    TArray<FString> LastPlacementWarnings;
    FString LastPlacementError;
    TSharedPtr<const UE::MimirComposite::FMHRandomSourceGraph> AppliedGraph;
    /**
     * Recipe graph of the last build attempt, kept even when that build failed:
     * its resources are what a targeted notification must be able to hit so
     * the placement retries once the missing endpoint appears (R2b-3).
     */
    TSharedPtr<const UE::MimirComposite::FMHRandomSourceGraph> ObservedRecipeGraph;
    /**
     * Resident preview plan (Recipe Model v2 §2.10 "LastPlacements"): Layout +
     * Appearance of the current seeds on the recipe graph, no closure, no
     * signature. Basis updates, Outliner and reseed diffs read it; nothing
     * re-resolves it.
     */
    TSharedPtr<const UE::MimirComposite::FMHResolvedCompositePlan> ResidentPlan;
    /** Latest resolved candidate, unpublished until every selected mesh load settles. */
    TSharedPtr<const UE::MimirComposite::FMHRandomSourceGraph> PendingPlacementGraph;
    TSharedPtr<const UE::MimirComposite::FMHResolvedCompositePlan> PendingPlacementPlan;
    TSet<UE::MimirComposite::FMHResourceKey> PendingSelectedMeshKeys;
    /** Keeps admitted meshes alive between per-key completion and the batch commit. */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UStaticMesh>> PendingSelectedMeshes;
    FDelegateHandle EndpointLoadReadyHandle;
    uint64 PendingPlacementEpoch = 0;
    bool bPendingSeedOnly = false;
    bool bPendingRecipeChanged = false;
    uint32 PreviewRevision = 0;
    TOptional<UE::MimirComposite::FMHRandomSourceGraph> EditingGraph;
    TOptional<UE::MimirComposite::FMHCompositeDocument> EditingDocument;
    TArray<FTransform> LastEditHandleTransforms;
    FTransform LastEditBasis = FTransform::Identity;
    EMHCompositeSeedEffect SeedAffectsResult = EMHCompositeSeedEffect::None;
    bool bPlanAvailable = false;
    // Only a rejected placement basis can recover by reapplying the old plan.
    // A rejected newer definition must pass full graph admission again.
    bool bBasisRejected = false;

    bool bRebuildInProgress = false;
    bool bPlacementEditMode = false;
    bool bExtractSelectedLeafForEdit = false;
    FString SelectedPlacementLeafPath;
    FString SelectedPlacementOccurrencePath;
    /** Set by PostLoad; consumed by the single admitted first-build point. */
    bool bNeedsInitialPlacementBuild = false;
    /**
     * Set by the one-time AppearanceSeed migration. MarkPackageDirty is a no-op
     * while a package is still loading, so the dirty flag is raised at the same
     * admitted post-registration point, once, and never re-derives the value.
     */
    bool bNeedsAppearanceSeedDirty = false;

    uint32 PlacementRebuildCount = 0;
    uint32 PlacementDesyncCount = 0;
    uint32 PlacementUnregisteredBuildCount = 0;
};
