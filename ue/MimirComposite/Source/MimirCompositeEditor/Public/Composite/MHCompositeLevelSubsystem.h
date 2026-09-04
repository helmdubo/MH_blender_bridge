#pragma once

class UMHCompositeSettings;
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
 * Transaction boundary for Source Protocol v5 parent-local composite operations.
 * Source documents remain authoritative; all scene objects produced here are
 * either managed endpoints or the one persisted AMHCompositeActor row.
 */
namespace UE::MimirComposite
{
struct FMHCompositeDocument;

/**
 * Pure Build preflight (R4-pre-2, owner decision 2026-09-04): assembles the
 * recipe document for the selection exactly as BuildComposite would (pivot =
 * selection AABB centre, one node per actor) and reports, one line per actor
 * and per item, every piece of selected state the recipe grammar cannot carry:
 * a child composite's Seed/AppearanceSeed (its random subtree re-rolls under
 * the new parent), a StaticMeshActor's material overrides and custom primitive
 * data, an Actor leaf's instance properties. Warnings never refuse; only
 * unrepresentable objects (transform, unmanaged mesh) return false with
 * MH_E_UNREPRESENTABLE_SCENE_OBJECT. Touches neither source root nor the scene.
 */
MIMIRCOMPOSITEEDITOR_API bool MHPreflightBuildComposite(
    const TArray<AActor*>& Actors,
    const UMHCompositeSettings& Settings,
    FMHCompositeDocument& OutDocument,
    TArray<FString>& OutWarnings,
    FString& OutError);
} // namespace UE::MimirComposite

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
    bool IsEditingComposite(const AMHCompositeActor* Actor) const
    {
        return EditingActor.IsValid() && EditingActor.Get() == Actor;
    }
    FString GetEditingCompositeLogicalName() const;
    FString GetEditingCompositeSourceRelativePath() const;

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

#if WITH_DEV_AUTOMATION_TESTS
    void SetCommitPublisherForTests(
        TFunction<bool(UMHCompositeAsset&, FString&)> Publisher)
    {
        CommitPublisherForTests = MoveTemp(Publisher);
    }
#endif

private:
    TWeakObjectPtr<AMHCompositeActor> EditingActor;
    UE::MimirComposite::FMHCompositeDocument EditingDocument;
    TArray<TWeakObjectPtr<USceneComponent>> EditingTopLevelComponents;
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<bool(UMHCompositeAsset&, FString&)> CommitPublisherForTests;
#endif
};
