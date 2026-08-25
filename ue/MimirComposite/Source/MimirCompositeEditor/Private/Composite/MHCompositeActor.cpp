#include "Composite/MHCompositeActor.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Components/SceneComponent.h"
#include "LevelEditor.h"
#include "Logging/MessageLog.h"
#include "Modules/ModuleManager.h"
#include "Settings/MHCompositeSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeActor)

DEFINE_LOG_CATEGORY_STATIC(LogMHCompositeActor, Display, All);

namespace
{
void BroadcastMHCompositeComponentsEdited()
{
#if WITH_EDITOR
    if (!IsRunningCommandlet() && FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
    {
        FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"))
            .BroadcastComponentsEdited();
    }
#endif
}
}

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
    BroadcastMHCompositeComponentsEdited();
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
    BroadcastMHCompositeComponentsEdited();

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
    // Moving the actor already moves the attached derived tree. Rebuilding it
    // here replaces every component during a transform drag and leaves the
    // Details component tree temporarily pointing at destroyed objects.
    if (DerivedComponents.IsEmpty())
    {
        RebuildComposite();
    }
}

void AMHCompositeActor::PostLoad()
{
    Super::PostLoad();
    RebuildComposite();
}

void AMHCompositeActor::Destroyed()
{
    // EditorDestroyActor keeps the actor UObject alive for undo, but its
    // transient placement components must stop contributing render hit proxies
    // immediately. Otherwise a composite built from another composite can
    // still click through to the destroyed source actor.
    ClearDerivedComponents();
    Super::Destroyed();
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
    const FName PropertyName = PropertyChangedEvent.GetPropertyName();
    if (PropertyName == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, CompositeAsset))
    {
        RebuildComposite();
    }
}
#endif
