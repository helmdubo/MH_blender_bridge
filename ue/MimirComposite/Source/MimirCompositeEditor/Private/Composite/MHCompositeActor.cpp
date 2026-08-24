#include "Composite/MHCompositeActor.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeCompiler.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/SceneComponent.h"
#include "Logging/MessageLog.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadScanResolver.h"
#include "UObject/UObjectIterator.h"

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
    CompositeAsset = Asset;
    RebuildComposite();
}

UMHCompositeAsset* AMHCompositeActor::GetCompositeAsset() const
{
    return CompositeAsset.LoadSynchronous();
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
}

void AMHCompositeActor::RebuildComposite()
{
    if (bRebuildInProgress || IsTemplate() || HasAnyFlags(RF_ClassDefaultObject))
    {
        return;
    }
    TGuardValue<bool> RebuildGuard(bRebuildInProgress, true);

    UMHCompositeAsset* Asset = GetCompositeAsset();
    if (Asset == nullptr)
    {
        ClearDerivedComponents();
        return;
    }

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    FString Error;
    if (Settings == nullptr || SourceRoot.IsEmpty())
    {
        Error = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
    }

    UE::MimirComposite::FMHCompositeDocument Document;
    if (Error.IsEmpty() && !UE::MimirComposite::MHExtractCompositeV4(*Asset, Document, Error))
    {
        // Error is already diagnostic-bearing.
    }

    UE::MimirComposite::FMHPayloadScanResolver Resolver(SourceRoot);
    if (Error.IsEmpty() && !Resolver.Initialize(Error))
    {
        // Error is already diagnostic-bearing.
    }

    UE::MimirComposite::FMHCompositeCompileResult Result;
    if (Error.IsEmpty())
    {
        Result = UE::MimirComposite::MHCompileCompositeV4(
            *this,
            Asset->LogicalName,
            Document,
            Resolver,
            *Settings);
        Error = MoveTemp(Result.Error);
    }
    if (!Error.IsEmpty())
    {
        const FString Diagnostic = FString::Printf(
            TEXT("%s: %s"),
            *Asset->GetPathName(),
            *Error);
        UE_LOG(LogMHCompositeActor, Error, TEXT("%s"), *Diagnostic);
        if (!IsRunningCommandlet())
        {
            FMessageLog(TEXT("Mimir")).Error(FText::FromString(Diagnostic));
        }
        return;
    }

    const TArray<TObjectPtr<UActorComponent>> PreviousComponents = MoveTemp(DerivedComponents);
    DerivedComponents = MoveTemp(Result.Components);
    for (int32 Index = PreviousComponents.Num() - 1; Index >= 0; --Index)
    {
        if (UActorComponent* Component = PreviousComponents[Index])
        {
            Component->DestroyComponent();
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

namespace UE::MimirComposite
{

void MHNotifyCompositeAssetChanged(UMHCompositeAsset& Asset)
{
    const FSoftObjectPath ChangedPath(&Asset);
    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        if (Actor == nullptr || Actor->IsTemplate() || Actor->GetWorld() == nullptr)
        {
            continue;
        }
        if (FSoftObjectPath(Actor->GetCompositeAsset()) == ChangedPath)
        {
            Actor->RebuildComposite();
        }
    }
}

} // namespace UE::MimirComposite
