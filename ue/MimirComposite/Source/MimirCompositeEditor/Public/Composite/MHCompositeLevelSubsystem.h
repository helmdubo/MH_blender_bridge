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

/** R6-U (docs/16 §2.7): where a unique copy of the edited definition takes effect. */
UENUM()
enum class EMHCompositeUniqueScope : uint8
{
    /** The invoking (shared) definition is rewired to the copy and published: every placement of that definition follows. */
    InParentDefinition,
    /** Every definition from the edited one up to this placement's root is copied; only this placement switches to the new root. */
    ForThisPlacement
};

/** R6-U: how the edited definition is copied. */
UENUM()
enum class EMHCompositeUniqueVariant : uint8
{
    /** The draft document as authored: random draws inside re-roll under the new name (warned). */
    Procedural,
    /** R6-U2: the resolved subtree under this placement as concrete mesh/actor nodes; nothing re-rolls. */
    BakeCurrentResult
};

/** R6-U: what Save Unique would do for a scope, before any name is chosen. */
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeSaveUniquePlan
{
    /** Definitions that get a unique copy, innermost (the edited one) first. */
    TArray<FString> Copies;
    /** The shared definition rewired and overwritten in place (InParentDefinition), else empty. */
    FString OverwrittenDefinition;
    /** Copies whose random draws re-roll under the new name (streams are keyed by node path, 16 §2.10). */
    TArray<FString> Warnings;
};

namespace UE::MimirComposite
{
/** True when any node of the document draws randomly: random options, an inline placement or a placement profile. */
MIMIRCOMPOSITEEDITOR_API bool MHCompositeDocumentHasRandomization(const FMHCompositeDocument& Document);
}

class UMHCompositeEditSession;

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
    /**
     * Publishes the active session: a root session writes the edited top-level
     * transforms to the placement's source; a nested session (R6-D2, docs/16
     * §2.7 "Apply Shared Definition") writes the edited nested definition to
     * its own source and refreshes every placement that invokes it. Both
     * cross the source boundary: UE Undo is cleared first.
     */
    bool CommitEditComposite(TArray<FString>& OutWarnings, FString& OutError);
    bool CancelEditComposite(FString& OutError);
    /** R6-U: the plan for Scope — copies innermost first, the overwritten shared definition, re-roll warnings. Needs an active Edit Contents session. */
    bool DescribeSaveUnique(EMHCompositeUniqueScope Scope, EMHCompositeUniqueVariant Variant, FMHCompositeSaveUniquePlan& OutPlan, FString& OutError) const;
    /**
     * R6-U (procedural variant): saves the nested draft as unique definitions.
     * Targets align with the plan's Copies. Validated before anything is
     * written; crosses the source boundary (UE Undo cleared) like Commit.
     * ForThisPlacement keeps this placement's root-level streams by giving it
     * the original root as call context when it has none.
     */
    bool SaveEditAsUnique(
        EMHCompositeUniqueScope Scope,
        EMHCompositeUniqueVariant Variant,
        const TArray<UE::MimirComposite::FMHCompositeAdoptTarget>& Targets,
        TArray<FString>& OutWarnings,
        FString& OutError);
    /** Context of the active session; empty EditedLogicalName when none. */
    FMHCompositeEditContext GetEditContext() const;
    /** The draft document of the active session (the root's or the nested definition's), read from the session's draft. */
    const UE::MimirComposite::FMHCompositeDocument& GetEditingDraft() const;
    /** CE-1: the single owner of the active session; null when none. */
    UMHCompositeEditSession* GetEditSession() const { return EditSession; }
    bool IsEditingComposite() const { return EditingActor.IsValid(); }
    /**
     * CE-pre: identity of the current session. Advances on every Begin and on
     * every session end, so a callback captured for one session never acts on
     * a later one (spec CE §9: deferred callbacks carry the epoch).
     */
    uint32 GetEditSessionEpoch() const { return EditSessionEpoch; }
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
    /**
     * Stands in for MHPublishCompositeV5 in Commit: the asset arrives already
     * applied and UE Undo cleared. A publisher that reports success must
     * notify consumers (MHNotifyCompositeAssetChanged) as the real one does.
     */
    void SetCommitPublisherForTests(
        TFunction<bool(UMHCompositeAsset&, FString&)> Publisher)
    {
        CommitPublisherForTests = MoveTemp(Publisher);
    }
    /** Stands in for managed-composite creation (validate, publish, import — shared with Build); must return a resolvable managed asset. */
    void SetDefinitionCreatorForTests(
        TFunction<UMHCompositeAsset*(const UE::MimirComposite::FMHCompositeDocument&, const UE::MimirComposite::FMHCompositeAdoptTarget&, FString&)> Creator)
    {
        DefinitionCreatorForTests = MoveTemp(Creator);
    }
#endif

private:
    TWeakObjectPtr<AMHCompositeActor> EditingActor;
    uint32 EditSessionEpoch = 0;
    /** CE-1: the session object; strong reflected reference. */
    UPROPERTY(Transient)
    TObjectPtr<UMHCompositeEditSession> EditSession;
    mutable UE::MimirComposite::FMHCompositeDocument EditingDocument;
    TArray<TWeakObjectPtr<USceneComponent>> EditingTopLevelComponents;
    /** R6-D0: the definition under edit (the root's asset or a nested child's), its invocation and effective parent. */
    TWeakObjectPtr<UMHCompositeAsset> EditingAsset;
    FString EditingInvocationPath;
    FMatrix EditingParentWorld = FMatrix::Identity;
    void ResetEditSession();
    /** CE-1: creates the session object for the edit just begun (EditingDocument is its original). */
    void OpenEditSession(AMHCompositeActor* Root, UMHCompositeAsset* Asset, const FString& InvocationNodePath);
    /** One managed composite from a document: validated, published to Source Root under Target, imported (Build, R6-U). */
    bool CreateManagedComposite(
        const UE::MimirComposite::FMHCompositeDocument& Document,
        const UE::MimirComposite::FMHCompositeAdoptTarget& Target,
        UMHCompositeAsset*& OutAsset,
        TArray<FString>& OutWarnings,
        FString& OutError);
    /** Applies and publishes Document as Asset's source (seam-aware); a failure restores the definition. */
    bool PublishDefinition(
        UMHCompositeAsset& Asset,
        const UE::MimirComposite::FMHCompositeDocument& Document,
        const FString& SourceRoot,
        TArray<FString>& OutWarnings,
        FString& OutError);
    /** R6-D2: Apply Shared Definition — the nested draft becomes the child's source; consumers follow. */
    bool CommitNestedEditComposite(TArray<FString>& OutWarnings, FString& OutError);
    /** After a failed publish: the authoritative source if present, else the pre-publish document; consumers are notified. */
    static void RestoreDefinition(
        UMHCompositeAsset& Asset,
        const UE::MimirComposite::FMHCompositeDocument& Previous,
        const FString& SourceRelativePath,
        const FString& SourceRoot,
        TArray<FString>& OutWarnings,
        FString& InOutError);
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<bool(UMHCompositeAsset&, FString&)> CommitPublisherForTests;
    TFunction<UMHCompositeAsset*(const UE::MimirComposite::FMHCompositeDocument&, const UE::MimirComposite::FMHCompositeAdoptTarget&, FString&)> DefinitionCreatorForTests;
#endif
};
