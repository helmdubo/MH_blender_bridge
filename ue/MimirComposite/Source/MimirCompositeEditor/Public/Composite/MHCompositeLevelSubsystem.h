#pragma once

#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MHCompositeLevelSubsystem.generated.h"

class AActor;
class AMHCompositeActor;
class UMHCompositeAsset;
class USceneComponent;

namespace UE::MimirComposite
{
struct FMHCompositeAdoptTarget;
}

/**
 * Transaction boundary for the Source Protocol v4 composite level operations.
 * Source documents remain authoritative; all scene objects produced here are
 * either managed endpoints or the one persisted AMHCompositeActor row.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHCompositeLevelSubsystem final : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    bool BuildComposite(
        const TArray<AActor*>& Actors,
        const UE::MimirComposite::FMHCompositeAdoptTarget& AdoptTarget,
        AMHCompositeActor*& OutActor,
        TArray<FString>& OutWarnings,
        FString& OutError);

    bool BreakComposites(
        const TArray<AMHCompositeActor*>& Actors,
        TArray<AActor*>& OutActors,
        TArray<FString>& OutWarnings,
        FString& OutError);

    bool BeginEditComposite(AMHCompositeActor* Actor, FString& OutError);
    bool CommitEditComposite(TArray<FString>& OutWarnings, FString& OutError);
    bool CancelEditComposite(FString& OutError);
    bool IsEditingComposite() const { return EditingActor.IsValid(); }

    bool RebuildComposites(
        const TArray<AMHCompositeActor*>& Actors,
        TArray<FString>& OutWarnings,
        FString& OutError);

    bool RebuildAllInstances(
        UMHCompositeAsset* Asset,
        TArray<FString>& OutWarnings,
        FString& OutError);

    bool DeleteCompositeResource(
        UMHCompositeAsset* Asset,
        bool bBreakLoadedInstances,
        TArray<FString>& OutWarnings,
        FString& OutError);

private:
    TWeakObjectPtr<AMHCompositeActor> EditingActor;
    UE::MimirComposite::FMHCompositeDocument EditingDocument;
    TArray<TWeakObjectPtr<USceneComponent>> EditingTopLevelComponents;
};
