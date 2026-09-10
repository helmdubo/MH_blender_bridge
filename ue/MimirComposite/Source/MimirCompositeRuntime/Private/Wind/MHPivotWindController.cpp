#include "Wind/MHPivotWindController.h"

#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

namespace
{
const FName DirectionSpeedParameter(TEXT("MH_WindDirectionSpeed"));
const FName NoiseParameter(TEXT("MH_WindNoise"));
const FName TreeParameter(TEXT("MH_WindTree"));

TMap<TWeakObjectPtr<UWorld>, TWeakObjectPtr<AMHPivotWindController>> WorldOwners;

bool CanWriteWorld(const UWorld* World)
{
    return IsValid(World) && !World->bIsTearingDown &&
        !World->IsBeingCleanedUp() && !World->IsCleanedUp();
}

float FiniteOr(const float Value, const float Fallback)
{
    return FMath::IsFinite(Value) ? Value : Fallback;
}

void PruneOwners()
{
    for (auto It = WorldOwners.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid() || !It.Value().IsValid()) It.RemoveCurrent();
    }
}
}

AMHPivotWindController::AMHPivotWindController()
{
    PrimaryActorTick.bCanEverTick = false;
}

float AMHPivotWindController::BeaufortToMetersPerSecond(const float Beaufort)
{
    const float Speed = 0.836f * FMath::Pow(FMath::Max(FiniteOr(Beaufort, 0.0f), 0.0f), 1.5f);
    return FiniteOr(Speed, 0.0f);
}

bool AMHPivotWindController::ApplySettings()
{
    UWorld* World = GetWorld();
    if (!IsInGameThread() || IsTemplate() || IsActorBeingDestroyed() || !CanWriteWorld(World)) return false;
    PruneOwners();
    if (AppliedWorld.IsValid() && AppliedWorld.Get() != World) ReleaseOwnership();
    if (!IsValid(WindParameters) ||
        WindParameters->GetVectorParameterByName(DirectionSpeedParameter) == nullptr ||
        WindParameters->GetVectorParameterByName(NoiseParameter) == nullptr ||
        WindParameters->GetVectorParameterByName(TreeParameter) == nullptr)
    {
        ReleaseOwnership();
        return false;
    }

    const TWeakObjectPtr<UWorld> WorldKey(World);
    if (const TWeakObjectPtr<AMHPivotWindController>* CurrentController = WorldOwners.Find(WorldKey))
    {
        if (CurrentController->IsValid() && CurrentController->Get() != this) return false;
    }
    UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(WindParameters);
    if (Instance == nullptr) return false;
    if (AppliedCollection.IsValid() && AppliedCollection.Get() != WindParameters) ReleaseOwnership();

    const float Radians = FMath::DegreesToRadians(FMath::Fmod(FiniteOr(DirectionDegrees, 0.0f), 360.0f));
    const float Speed = BeaufortToMetersPerSecond(FMath::Clamp(FiniteOr(StrengthBeaufort, 0.0f), 0.0f, 12.0f));
    // Match AmbientWind::setWindParametersToShader: only main wind strength
    // converts from Beaufort. The negatively advected noise uses the raw UI value.
    const float NoiseSpeed = FMath::Clamp(FiniteOr(NoiseSpeedBeaufort, 0.0f), 0.0f, 12.0f);
    const float NoiseScale = FiniteOr(NoiseScaleMeters, 0.0f);
    const float InverseNoiseScale = NoiseScale > 0.000001f ? 1.0f / NoiseScale : 0.0f;
    const FLinearColor DirectionSpeed(FMath::Cos(Radians), -FMath::Sin(Radians), Speed, Enabled ? 1.0f : 0.0f);
    const FLinearColor Noise(NoiseSpeed, InverseNoiseScale, FMath::Clamp(FiniteOr(NoisePerpendicular, 0.0f), 0.0f, 1.0f),
        FMath::Max(FiniteOr(NoiseStrength, 0.0f), 0.0f));
    const FLinearColor Tree(FMath::Max(FiniteOr(BranchAmplitude, 0.0f), 0.0f),
        FMath::Max(FiniteOr(DetailAmplitude, 0.0f), 0.0f), 0.0f, 0.0f);
    // All vector names were validated before any write. Collection defaults
    // remain untouched: each world owns its own transient override instance.
    const bool bDirectionApplied = Instance->SetVectorParameterValue(DirectionSpeedParameter, DirectionSpeed);
    const bool bNoiseApplied = Instance->SetVectorParameterValue(NoiseParameter, Noise);
    const bool bTreeApplied = Instance->SetVectorParameterValue(TreeParameter, Tree);
    if (!bDirectionApplied || !bNoiseApplied || !bTreeApplied) return false;
    WorldOwners.Add(WorldKey, this);
    AppliedWorld = World;
    AppliedCollection = WindParameters;
    return true;
}

void AMHPivotWindController::ReleaseOwnership()
{
    if (!IsInGameThread()) return;
    UWorld* World = AppliedWorld.Get();
    const TWeakObjectPtr<AMHPivotWindController>* CurrentController = WorldOwners.Find(AppliedWorld);
    if (CurrentController != nullptr && CurrentController->Get() == this)
    {
        // Normal actor removal disables its published wind. Teardown must never
        // instantiate or update a collection while the scene is being destroyed.
        if (CanWriteWorld(World) && AppliedCollection.IsValid())
        {
            if (UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(AppliedCollection.Get()))
            {
                FLinearColor DirectionSpeed;
                if (Instance->GetVectorParameterValue(DirectionSpeedParameter, DirectionSpeed))
                {
                    DirectionSpeed.A = 0.0f;
                    Instance->SetVectorParameterValue(DirectionSpeedParameter, DirectionSpeed);
                }
            }
        }
        WorldOwners.Remove(AppliedWorld);
    }
    AppliedWorld.Reset();
    AppliedCollection.Reset();
    PruneOwners();
}

void AMHPivotWindController::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    ApplySettings();
}

void AMHPivotWindController::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    // ReInitWorld replaces MPC instances and registers actors without rerunning
    // construction or BeginPlay. Preserve ownership through temporary component
    // unregistration so registration order cannot promote a rejected duplicate.
    ApplySettings();
}

void AMHPivotWindController::BeginPlay()
{
    Super::BeginPlay();
    ApplySettings();
}

void AMHPivotWindController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseOwnership();
    Super::EndPlay(EndPlayReason);
}

void AMHPivotWindController::Destroyed()
{
    ReleaseOwnership();
    Super::Destroyed();
}

#if WITH_EDITOR
void AMHPivotWindController::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    ApplySettings();
}

void AMHPivotWindController::PostEditUndo()
{
    Super::PostEditUndo();
    ApplySettings();
}
#endif
