#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Random/MHRandomStream.h"
#include "Source/MHSourceResolver.h"
#include "UObject/ObjectKey.h"
#include "MHProofCache.generated.h"

class AMHCompositeActor;
class FObjectPreSaveContext;
class UWorld;

namespace UE::MimirComposite
{

/**
 * Proof state of one placement (Recipe Model v2 §2.6). The preview plane never
 * produces any of these; only the proof cache and the exit points do.
 */
enum class EMHProofState : uint8
{
    /** No proof was ever requested for the placement's current key. */
    Unknown,
    /** A deferred proof build is scheduled and has not run yet. */
    ProofPending,
    /** Full closure admitted; every receipt matches the source payload hash known to the index. */
    Fresh,
    /** Full closure admitted, but at least one receipt differs from the source payload hash. */
    Stale,
    /** The full closure cannot be admitted (missing endpoint, receipt, class, duplicate claim). */
    Missing,
};

MIMIRCOMPOSITEEDITOR_API const TCHAR* MHProofStateLabel(EMHProofState State);

/** Result of a proof build or a cache read. */
struct MIMIRCOMPOSITEEDITOR_API FMHProofResult
{
    EMHProofState State = EMHProofState::Unknown;
    /** Diagnostic of a Stale or Missing state (MH_E_* code first). */
    FString Diagnostic;
    /**
     * Full proof plan (closure, ClosureHash, ResolvedSignature, AppearanceSignature,
     * PlacementSignature) when the closure was admitted (Fresh or Stale).
     */
    TSharedPtr<const FMHResolvedCompositePlan> Plan;
};

/** One row of a world audit (PreSaveWorld warning source). */
struct MIMIRCOMPOSITEEDITOR_API FMHProofAuditRow
{
    TWeakObjectPtr<const AMHCompositeActor> Placement;
    EMHProofState State = EMHProofState::Unknown;
    FString Diagnostic;
};

} // namespace UE::MimirComposite

/**
 * Background proof cache (Recipe Model v2 §2.6, R2c). Key: root asset +
 * RecipeRevision + Seed + AppearanceSeed + ProjectIndex generation +
 * ImporterVersion + endpoint registry revision. PreSaveWorld reads it and
 * warns; build preflight, runtime snapshot admission, export and Break build
 * the proof synchronously. The preview plane never touches it.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHProofCacheSubsystem final : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    static UMHProofCacheSubsystem* Get();

    /** Read-only cache lookup for the placement's current key; never builds anything. */
    UE::MimirComposite::FMHProofResult GetProofState(const AMHCompositeActor& Placement) const;

    /**
     * Schedule a deferred proof build (game thread, outside the caller's
     * action). Returns the cached state, or ProofPending when scheduled.
     */
    UE::MimirComposite::EMHProofState RequestProof(const AMHCompositeActor& Placement);

    /** Run every scheduled proof build now (explicit user actions, tests). */
    void FlushPendingProofs();

    /**
     * Synchronous proof for exit points: applied graph (receipts, identity,
     * duplicate claims), full resolve, transform admission, source freshness.
     * Returns true only for Fresh; the result carries the state either way.
     */
    bool BuildProofNow(
        const AMHCompositeActor& Placement,
        UE::MimirComposite::FMHProofResult& OutResult,
        FString& OutError);

    /** Cached states of every placement in the world; builds nothing (PreSaveWorld). */
    TArray<UE::MimirComposite::FMHProofAuditRow> AuditWorld(const UWorld& World) const;

    /** Forget every cached proof (reimport notifications, index rebuild, tests). */
    void InvalidateAll();

    /** Warnings the last non-cook PreSaveWorld audit emitted (one per non-Fresh placement). */
    int32 GetLastSaveAuditWarningCount() const { return LastSaveAuditWarningCount; }

    /**
     * Test seam for the freshness check: current source payload hash of a
     * resource key. Production reads the ProjectIndex; a provider returning
     * false means "unknown to the index", which never makes a proof Stale.
     */
    void SetSourceHashProviderForTests(TFunction<bool(const UE::MimirComposite::FMHResourceKey&, FString&)> Provider);

private:
    struct FImpl;

    bool TickPendingProofs(float DeltaTime);
    void HandlePreSaveWorld(UWorld* World, FObjectPreSaveContext Context);

    TSharedPtr<FImpl> Impl;
    TFunction<bool(const UE::MimirComposite::FMHResourceKey&, FString&)> SourceHashProviderForTests;
    int32 LastSaveAuditWarningCount = 0;
};
