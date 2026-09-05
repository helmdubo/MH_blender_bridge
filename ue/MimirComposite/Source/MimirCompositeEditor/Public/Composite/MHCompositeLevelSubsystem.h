#pragma once

class UMHCompositeSettings;
#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MHCompositeLevelSubsystem.generated.h"

class AActor;
class AMHCompositeActor;
class UMHCompositeAsset;

/** Where a composite edit session saves (docs/16 §2.7 R6-D0/R6-U). */
UENUM()
enum class EMHCompositeEditSaveScope : uint8
{
    None,
    /** The shared definition: every placement and every parent invoking it. */
    SharedDefinition,
};

/** Read-only description of the active composite edit session (docs/16 §2.7 R6-D0). */
USTRUCT()
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeEditContext
{
    GENERATED_BODY()

    /** Logical name of the definition under edit (root or nested child). */
    UPROPERTY() FString EditedLogicalName;
    UPROPERTY() FString EditedSourceRelativePath;
    /** NodePath of the invocation inside the root placement; empty for a root session. */
    UPROPERTY() FString InvocationPath;
    /** World matrix of the invocation under the placement basis (identity-based root basis for a root session). */
    FMatrix EffectiveParentWorld = FMatrix::Identity;
    /** Root placement the session was opened from. */
    TWeakObjectPtr<AMHCompositeActor> RootPlacement;
    /** Live placements whose recipe graph contains the edited definition. */
    UPROPERTY() int32 ConsumerPlacements = 0;
    UPROPERTY() EMHCompositeEditSaveScope SaveScope = EMHCompositeEditSaveScope::None;
};
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
    /**
     * R6-D0 (docs/16 §2.7): opens the shared definition invoked at
     * InvocationNodePath of Root's resident plan as a draft, with the root
     * placement and the invocation's effective world transform as context.
     * The source is untouched until an explicit publish; Cancel discards.
     */
    bool BeginEditNestedComposite(AMHCompositeActor* Root, const FString& InvocationNodePath, FString& OutError);
    bool CommitEditComposite(TArray<FString>& OutWarnings, FString& OutError);
    bool CancelEditComposite(FString& OutError);
    /** Context of the active session; empty EditedLogicalName when none. */
    FMHCompositeEditContext GetEditContext() const;
    /** The draft document of the active session (the root's or the nested definition's). */
    const UE::MimirComposite::FMHCompositeDocument& GetEditingDraft() const { return EditingDocument; }
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
    /** R6-D0: the definition under edit (the root's asset or a nested child's), its invocation and effective parent. */
    TWeakObjectPtr<UMHCompositeAsset> EditingAsset;
    FString EditingInvocationPath;
    FMatrix EditingParentWorld = FMatrix::Identity;
    void ResetEditSession();
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<bool(UMHCompositeAsset&, FString&)> CommitPublisherForTests;
#endif
};
