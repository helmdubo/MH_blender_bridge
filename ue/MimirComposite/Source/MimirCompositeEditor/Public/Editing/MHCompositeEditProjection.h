#pragma once

#include "Composite/MHInstancePool.h"
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
 * CE-2b (docs/contracts/composite_edit_ce0.md, spec §5.4–5.5): the one
 * transient actor that carries the edit projection of a session — a
 * component per resolved node of the edited definition under the selected
 * occurrence. Never saved, never in PIE or cook, hidden from the World
 * Outliner (the Composite Outliner is the tree). Owner decision 2026-09-06:
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
    /** First component of a session node (its leaf, or its handle). */
    USceneComponent* FindComponentForNodeId(const FGuid& NodeId) const;
    /** CE-3b: the component at a plan origin (a Composite Outliner row's node path); null when the origin is not projected. */
    USceneComponent* FindComponentForOrigin(const FString& Origin) const;
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
    /** Scene proxies that already carry the editing state (CE-3b). */
    TMap<TWeakObjectPtr<const UPrimitiveComponent>, const FPrimitiveSceneProxy*> TintedProxies;
    TSharedPtr<UE::MimirComposite::FMHResolvedCompositePlan> Plan;
    UE::MimirComposite::FMHPoolSuppressionLease Lease;
    FString OccurrencePrefix;
    FString DefinitionPrefix;
};
