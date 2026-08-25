#include "Composite/MHCompositeActor.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Components/SceneComponent.h"
#include "Logging/MessageLog.h"
#include "Settings/MHCompositeSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeActor)

DEFINE_LOG_CATEGORY_STATIC(LogMHCompositeActor, Display, All);

AMHCompositeActor::AMHCompositeActor()
{
    CompositeRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MHCompositeRoot"));
    SetRootComponent(CompositeRoot);
}

void AMHCompositeActor::SetCompositeAsset(UMHCompositeAsset* Asset)
{
    Modify();
    bPlacementEditMode = false;
    CompositeAsset = Asset;
    RebuildComposite();
}

UMHCompositeAsset* AMHCompositeActor::GetCompositeAsset() const
{
    return CompositeAsset.LoadSynchronous();
}

void AMHCompositeActor::SetPlacementEditMode(const bool bEnabled)
{
    bPlacementEditMode = bEnabled;
}

bool AMHCompositeActor::DependsOnResource(
    const UE::MimirComposite::FMHResourceKey& Key) const
{
    return PlacementDependencies.Contains(Key);
}

void AMHCompositeActor::ClearDerivedComponents()
{
    for (int32 Index = DerivedComponents.Num() - 1; Index >= 0; --Index)
    {
        if (UActorComponent* Component = DerivedComponents[Index])
        {
            Component->DestroyComponent();
        }
    }
    DerivedComponents.Reset();
    TopLevelPlacementComponents.Reset();
    PlacementDependencies.Reset();
    LastPlacementWarnings.Reset();
}

void AMHCompositeActor::RebuildComposite()
{
    if (bRebuildInProgress || bPlacementEditMode || IsTemplate() ||
        HasAnyFlags(RF_ClassDefaultObject))
    {
        return;
    }
    TGuardValue<bool> RebuildGuard(bRebuildInProgress, true);

    const FSoftObjectPath AssetPath = CompositeAsset.ToSoftObjectPath();
    if (AssetPath.IsNull())
    {
        ClearDerivedComponents();
        return;
    }

    UMHCompositeAsset* Asset = GetCompositeAsset();
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr)
    {
        ClearDerivedComponents();
        return;
    }

    const FString ExpectedLogicalName = Asset != nullptr && !Asset->LogicalName.IsEmpty()
        ? Asset->LogicalName
        : AssetPath.GetAssetName();
    UE::MimirComposite::FMHCompositePlacementCompileResult Result =
        UE::MimirComposite::MHCompileCompositePlacementV4(
            *this,
            Asset,
            ExpectedLogicalName,
            *Settings);

    const TArray<TObjectPtr<UActorComponent>> PreviousComponents = MoveTemp(DerivedComponents);
    DerivedComponents = MoveTemp(Result.Components);
    TopLevelPlacementComponents = MoveTemp(Result.TopLevelComponents);
    PlacementDependencies = MoveTemp(Result.Dependencies);
    LastPlacementWarnings = MoveTemp(Result.Warnings);
    for (int32 Index = PreviousComponents.Num() - 1; Index >= 0; --Index)
    {
        if (UActorComponent* Component = PreviousComponents[Index])
        {
            Component->DestroyComponent();
        }
    }

    for (const FString& Warning : LastPlacementWarnings)
    {
        const FString Diagnostic = FString::Printf(
            TEXT("%s: %s"),
            *AssetPath.ToString(),
            *Warning);
        UE_LOG(LogMHCompositeActor, Warning, TEXT("%s"), *Diagnostic);
        if (!IsRunningCommandlet())
        {
            FMessageLog(TEXT("Mimir")).Warning(FText::FromString(Diagnostic));
        }
    }
    if (!Result.Error.IsEmpty())
    {
        const FString Diagnostic = FString::Printf(
            TEXT("%s: %s"),
            *AssetPath.ToString(),
            *Result.Error);
        UE_LOG(LogMHCompositeActor, Error, TEXT("%s"), *Diagnostic);
        if (!IsRunningCommandlet())
        {
            FMessageLog(TEXT("Mimir")).Error(FText::FromString(Diagnostic));
        }
    }
}

void AMHCompositeActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RebuildComposite();
}

void AMHCompositeActor::PostLoad()
{
    Super::PostLoad();
    RebuildComposite();
}

#if WITH_EDITOR
void AMHCompositeActor::PostEditUndo()
{
    Super::PostEditUndo();
    RebuildComposite();
}

void AMHCompositeActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    RebuildComposite();
}
#endif
