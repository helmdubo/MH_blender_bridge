#pragma once

#include "Composite/MHInstancePool.h"
#include "Components/StaticMeshComponent.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Random/MHRandomStream.h"
#include "UObject/Object.h"
#include "MHCompositeEditProjection.generated.h"

class UMHCompositeAsset;
class UMHCompositeEditSession;
class FPrimitiveSceneProxy;
class UPrimitiveComponent;
class USceneComponent;

/**
 * The single authoring frame for one session node.  Visual components are
 * explicitly bound to this identity when a projection refresh succeeds; the
 * frame therefore remains coherent even when an origin is reused after a
 * structural edit.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeEditNodeFrame
{
    FGuid NodeId;
    FGuid ParentNodeId;
    FTransform AuthoredLocal = FTransform::Identity;
    FMatrix WorldMatrix = FMatrix::Identity;
    FMatrix ParentWorldMatrix = FMatrix::Identity;
    bool bGeneratedTransform = false;
};

/** Mesh visual whose outline follows logical edit selection only. */
UCLASS(Transient, NotBlueprintable)
class MIMIRCOMPOSITEEDITOR_API UMHCompositeEditMeshComponent final : public UStaticMeshComponent
{
    GENERATED_BODY()

public:
    UMHCompositeEditMeshComponent();
    void SetEditSelected(bool bSelected);
    virtual bool ShouldRenderSelected() const override;

private:
    bool IsEditIndividuallySelected(const UPrimitiveComponent* Component) const;
    bool bEditSelected = false;
};

/**
 * CE-2b (docs/contracts/composite_edit_ce0.md, spec §5.4–5.5): the one
 * transient actor that carries the edit projection of a session — a
 * component per resolved node of the edited definition under the selected
 * occurrence. Spawned transient and duplicate-transient, so it is never saved,
 * cooked, or copied into PIE; it remains visible in editor Game View and is
 * hidden from the World Outliner (the Composite Outliner is the tree). Owner decision 2026-09-06:
 * components of one projection actor, not an actor per node.
 */
UCLASS(Transient, NotPlaceable, NotBlueprintable)
class MIMIRCOMPOSITEEDITOR_API AMHCompositeEditProjectionActor final : public AActor
{
    GENERATED_BODY()

public:
    AMHCompositeEditProjectionActor();
};

/**
 * CE-2b: the edit projection of a session. Resolves the session draft under
 * the placement's frozen context (through the recipe compiler, never a second
 * resolver), suppresses the pooled instances of the selected occurrence with
 * a lease, and shows the occurrence as components of the projection actor:
 * a static mesh per mesh leaf (same mesh, same appearance channels), a
 * scene handle per group / random / actor / nested-reference node. Other
 * occurrences of the definition and every other placement keep the published
 * view (CE-ADR-4: local preview, shared save).
 */
UCLASS(Transient)
class MIMIRCOMPOSITEEDITOR_API UMHCompositeEditProjection : public UObject
{
    GENERATED_BODY()

public:
    bool Open(UMHCompositeEditSession& Session, FString& OutError);
    /** Re-resolves the draft and re-places the components; components keep their identity per plan origin. */
    bool Refresh(FString& OutError);
    /** Releases the lease and destroys the projection actor. Safe to call twice. */
    void Close();

    bool IsOpen() const { return ProjectionActor.IsValid(); }
    AMHCompositeEditProjectionActor* GetProjectionActor() const { return ProjectionActor.Get(); }
    const TArray<TObjectPtr<USceneComponent>>& GetComponents() const { return Components; }
    /** Plan origin (node path) of a projection component; empty for foreign components. */
    FString GetOriginForComponent(const USceneComponent* Component) const;
    /** Session node the component belongs to: the leaf's node, or the random node that picked it. */
    FGuid GetNodeIdForComponent(const USceneComponent* Component) const;
    /** The deterministic component at the authored node's exact structural frame. */
    USceneComponent* FindComponentForNodeId(const FGuid& NodeId) const;
    /** The authoring frame built from the exact current-definition node in the resolved plan. */
    bool GetNodeFrame(const FGuid& NodeId, FMHCompositeEditNodeFrame& OutFrame) const;
    /** Visuals bound to this node; optionally includes visuals of authored descendants. */
    TArray<USceneComponent*> GetComponentsForNodeId(const FGuid& NodeId, bool bIncludeDescendants = false) const;
    /** Bounds of all primitive visuals relevant to this logical node. */
    bool GetNodeBounds(const FGuid& NodeId, FBox& OutBounds) const;
    /** Updates mesh outlines from the logical selection (groups include their descendants). */
    void UpdateSelection(const TArray<FGuid>& NodeIds);
    /** CE-3b: the component at a plan origin (a Composite Outliner row's node path); null when the origin is not projected. */
    USceneComponent* FindComponentForOrigin(const FString& Origin) const;
    /** CE-4a: world transform of the session node's parent (the occurrence for top-level nodes) — the frame a local transform is authored in. */
    bool GetParentWorldForComponent(const USceneComponent* Component, FTransform& OutParentWorld) const;
    /**
     * CE-3b: marks the projection's primitives as "being edited" for the
     * renderer (`PushLevelInstanceEditingStateToProxy`), so the mode's
     * `EditingLevelInstance` show flag dims everything else. Idempotent per
     * scene proxy: call after Refresh and from the mode's tick, a re-created
     * proxy (mesh/material/visibility change) is pushed again.
     */
    void PushEditingTint();
    const UE::MimirComposite::FMHResolvedCompositePlan* GetPlan() const { return Plan.Get(); }
    const UE::MimirComposite::FMHPoolSuppressionLease& GetLease() const { return Lease; }

private:
    bool BuildDraftGraph(UE::MimirComposite::FMHRandomSourceGraph& OutGraph, FString& OutError);
    /** Plan path under the session's occurrence (all of the placement for a root session). */
    bool UnderOccurrence(const FString& Path) const;
    USceneComponent* PlaceComponent(const FString& Origin, UClass* Class, const FMatrix& WorldMatrix, const TFunction<void(USceneComponent&)>& Configure);
    void AcquireLease();

    UPROPERTY()
    TObjectPtr<UMHCompositeAsset> DraftAsset;
    UPROPERTY()
    TArray<TObjectPtr<USceneComponent>> Components;
    TWeakObjectPtr<UMHCompositeEditSession> Session;
    TWeakObjectPtr<AMHCompositeEditProjectionActor> ProjectionActor;
    TMap<FString, TWeakObjectPtr<USceneComponent>> ComponentsByOrigin;
    /** Successful-refresh bindings. Never inferred from the current draft during a query. */
    TMap<TWeakObjectPtr<USceneComponent>, FGuid> NodeIdByComponent;
    TMap<FGuid, FMHCompositeEditNodeFrame> NodeFrames;
    TMap<FGuid, TWeakObjectPtr<USceneComponent>> FrameComponentByNodeId;
    /** Scene proxies that already carry the editing state (CE-3b). */
    TMap<TWeakObjectPtr<const UPrimitiveComponent>, const FPrimitiveSceneProxy*> TintedProxies;
    TSharedPtr<UE::MimirComposite::FMHResolvedCompositePlan> Plan;
    UE::MimirComposite::FMHPoolSuppressionLease Lease;
    FString OccurrencePrefix;
    FString DefinitionPrefix;
};
