#pragma once

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Misc/Guid.h"
#include "UObject/Object.h"
#include "MHCompositeEditSession.generated.h"

/** CE-1 (spec §9): where a session is in its life. */
UENUM()
enum class EMHCompositeEditSessionState : uint8
{
    Idle,
    EditingClean,
    EditingDirty,
    Closed
};

/**
 * CE-1 (docs/contracts/composite_edit_ce0.md, spec §5.1): the single owner of
 * one composite edit session — identity, epoch, the edited definition, the
 * invocation it is edited under, the frozen placement context, the immutable
 * original document and the transactional draft. Held by
 * `UMHCompositeLevelSubsystem` through a strong reflected reference; every
 * command checks that the session is still open, so a callback captured for
 * a closed session cannot act on a later one.
 */
UCLASS(Transient)
class MIMIRCOMPOSITEEDITOR_API UMHCompositeEditSession : public UObject
{
    GENERATED_BODY()

public:
    void Open(
        AMHCompositeActor* InRootPlacement,
        UMHCompositeAsset* InEditedAsset,
        const FString& InInvocationPath,
        const UE::MimirComposite::FMHCompositeDocument& InOriginal,
        uint32 InEpoch);
    /** Terminal: the draft stays readable, every command is refused from here on. */
    void Close();

    const FGuid& GetSessionId() const { return SessionId; }
    uint32 GetEpoch() const { return Epoch; }
    EMHCompositeEditSessionState GetState() const;
    bool IsOpen() const { return State != EMHCompositeEditSessionState::Closed && State != EMHCompositeEditSessionState::Idle; }
    /** Authoring changes only: the draft's canonical bytes differ from the original's. */
    bool IsDirty() const;

    AMHCompositeActor* GetRootPlacement() const { return RootPlacement.Get(); }
    UMHCompositeAsset* GetEditedAsset() const { return EditedAsset.Get(); }
    UWorld* GetEditorWorld() const { return EditorWorld.Get(); }
    const FString& GetInvocationPath() const { return InvocationPath; }
    bool IsNested() const { return !InvocationPath.IsEmpty(); }
    int32 GetFrozenSeed() const { return FrozenSeed; }
    int32 GetFrozenAppearanceSeed() const { return FrozenAppearanceSeed; }
    const FMHCompositeCallContext& GetCallContext() const { return CallContext; }
    const UE::MimirComposite::FMHCompositeDocument& GetOriginalDocument() const { return Original; }
    const TArray<uint8>& GetOriginalBytes() const { return OriginalBytes; }
    UMHCompositeEditDocument* GetDraft() const { return Draft; }

    /** Authoring command on the draft; refused when the session is closed. */
    bool SetNodeTransform(const FGuid& NodeId, const FTransform& LocalTransform, FString& OutError);

    /**
     * CE-1 bridge until CE-4a moves the writes here: mirrors the legacy
     * actor-side handle edits (top-level nodes of the edited definition) into
     * the draft, so the draft is the one current document.
     */
    bool SyncDraftFromLegacyEdit(FString& OutError);

private:
    UPROPERTY()
    TObjectPtr<UMHCompositeEditDocument> Draft;
    UPROPERTY()
    FGuid SessionId;
    UPROPERTY()
    uint32 Epoch = 0;
    UPROPERTY()
    EMHCompositeEditSessionState State = EMHCompositeEditSessionState::Idle;
    UPROPERTY()
    FString InvocationPath;
    UPROPERTY()
    int32 FrozenSeed = 0;
    UPROPERTY()
    int32 FrozenAppearanceSeed = 0;
    UPROPERTY()
    FMHCompositeCallContext CallContext;

    TWeakObjectPtr<AMHCompositeActor> RootPlacement;
    TWeakObjectPtr<UMHCompositeAsset> EditedAsset;
    TWeakObjectPtr<UWorld> EditorWorld;
    UE::MimirComposite::FMHCompositeDocument Original;
    TArray<uint8> OriginalBytes;
    mutable uint64 DirtySerial = 0;
    mutable bool bDirtyCached = false;
};
