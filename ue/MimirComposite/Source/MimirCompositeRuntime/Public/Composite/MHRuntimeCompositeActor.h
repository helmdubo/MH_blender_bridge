#pragma once

#include "Composite/MHRuntimeCompositeInput.h"
#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Random/MHRandomStream.h"
#include "MHRuntimeCompositeActor.generated.h"

/** Cooked placement: full seed-free resolver input plus the placement's Seed. */
UCLASS(NotBlueprintable)
class MIMIRCOMPOSITERUNTIME_API AMHRuntimeCompositeActor final : public AActor
{
    GENERATED_BODY()

public:
    AMHRuntimeCompositeActor();

    /** Validate the entire closure and resolve before replacing any live component. */
    bool Configure(const FMHRuntimeCompositeInput& Input, int32 InSeed, FString& OutError);
    bool RebuildRuntime(FString& OutError);
    int32 GetSeed() const { return Seed; }
    const FMHRuntimeCompositeInput& GetRuntimeInput() const { return RuntimeInput; }
    const UE::MimirComposite::FMHResolvedCompositePlan* GetResolvedPlan() const;
    const FString& GetLastRuntimeError() const { return LastRuntimeError; }
    const TArray<TObjectPtr<USceneComponent>>& GetMaterializedComponents() const { return MaterializedComponents; }

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostLoad() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

private:
    bool BuildCandidate(const FMHRuntimeCompositeInput& Input, int32 InSeed,
        UE::MimirComposite::FMHResolvedCompositePlan& OutPlan, FString& OutError) const;
    bool Materialize(const FMHRuntimeCompositeInput& Input,
        const UE::MimirComposite::FMHResolvedCompositePlan& Plan, FString& OutError);
    void AttachTransformHook();
    void UpdatePlacementBasis(USceneComponent*, EUpdateTransformFlags, ETeleportType);
    void ClearMaterializedComponents();

    UPROPERTY(VisibleAnywhere, Category = "Mimir")
    TObjectPtr<USceneComponent> CompositeRoot;

    /** A derived transport snapshot; never a new source/receipt authority. */
    UPROPERTY(VisibleInstanceOnly, Category = "Mimir")
    FMHRuntimeCompositeInput RuntimeInput;

    /** Copied from the authoring placement. Explicit zero remains legal. */
    UPROPERTY(VisibleInstanceOnly, Category = "Mimir|Random")
    int32 Seed = 0;

    UPROPERTY(VisibleInstanceOnly, Transient, Category = "Mimir|Random")
    FString ResolvedSignature;

    UPROPERTY(Transient, DuplicateTransient, TextExportTransient)
    TArray<TObjectPtr<USceneComponent>> MaterializedComponents;

    TSharedPtr<const UE::MimirComposite::FMHResolvedCompositePlan> ResolvedPlan;
    FString LastRuntimeError;
    bool bUpdating = false;
    bool bBasisRejected = false;
};
