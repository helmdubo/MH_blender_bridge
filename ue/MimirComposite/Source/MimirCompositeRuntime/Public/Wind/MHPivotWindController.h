#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MHPivotWindController.generated.h"

class UMaterialParameterCollection;
struct FPropertyChangedEvent;

/**
 * Publishes global pivot-wind controls to this world's MPC instance.
 * The first valid controller to apply owns the world until it is removed or
 * clears its collection. Other controllers fail ApplySettings without writing.
 * Disabling wind retains ownership; animation uses shader time, never actor Tick.
 */
UCLASS(BlueprintType, Blueprintable)
class MIMIRCOMPOSITERUNTIME_API AMHPivotWindController : public AActor
{
    GENERATED_BODY()

public:
    AMHPivotWindController();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind")
    TObjectPtr<UMaterialParameterCollection> WindParameters;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind")
    bool Enabled = true;

    /** Dagor azimuth mapped to UE: 0 degrees is +X, 90 degrees is -Y. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind", meta=(Units="deg"))
    float DirectionDegrees = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind", meta=(ClampMin="0", ClampMax="12"))
    float StrengthBeaufort = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind|Noise", meta=(ClampMin="0"))
    float NoiseStrength = 1.0f;

    /** AssetViewer labels this Beaufort, but Dagor passes the number directly to noise advection. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind|Noise", meta=(ClampMin="0", ClampMax="12"))
    float NoiseSpeedBeaufort = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind|Noise", meta=(ClampMin="0", Units="m"))
    float NoiseScaleMeters = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind|Noise", meta=(ClampMin="0", ClampMax="1"))
    float NoisePerpendicular = 0.5f;

    /** Dagor tree_wind_branch_amp; default from the supplied CDK environments/global.blk. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind|Leaf response", meta=(ClampMin="0", Units="m"))
    float BranchAmplitude = 0.1f;

    /** Dagor tree_wind_detail_amp; independent of the pivot hierarchy's rotation limits. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MH Wind|Leaf response", meta=(ClampMin="0", Units="m"))
    float DetailAmplitude = 0.1f;

    /** Returns false for missing parameters, teardown, or another controller owning this world. */
    UFUNCTION(BlueprintCallable, Category="MH Wind")
    bool ApplySettings();

    /** Dagor conversion: 0.836 * max(Beaufort, 0)^1.5. Invalid results become zero. */
    static float BeaufortToMetersPerSecond(float Beaufort);

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostRegisterAllComponents() override;
    virtual void Destroyed() override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual void PostEditUndo() override;
#endif

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    void ReleaseOwnership();

    TWeakObjectPtr<UWorld> AppliedWorld;
    TWeakObjectPtr<UMaterialParameterCollection> AppliedCollection;
};
