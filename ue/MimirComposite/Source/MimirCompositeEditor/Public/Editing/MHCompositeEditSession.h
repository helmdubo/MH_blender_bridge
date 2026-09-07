#pragma once

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Misc/Guid.h"
#include "UObject/Object.h"
#include "MHCompositeEditSession.generated.h"

class UMHCompositeEditProjection;

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
    /** CE-5a: after a source-committed failure the committed document is the new original — dirty and Cancel measure against the file. */
    void RebaseOriginal(const UE::MimirComposite::FMHCompositeDocument& Committed);

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

    /** CE-I2 semantic selection: ordered session node identities, independent of component lifetime. */
    const TArray<FGuid>& GetSelectedNodeIds() const { return SelectedNodeIds; }
    FGuid GetActiveNodeId() const { return ActiveNodeId; }
    void SetSelectedNodeIds(const TArray<FGuid>& NodeIds, const FGuid& ActiveNodeId = FGuid());
    FSimpleMulticastDelegate OnSelectionChanged;
    /** Successful authoring changes, including Undo/Redo restoration. */
    FSimpleMulticastDelegate OnChanged;
    /** Most recent projection refresh failure; cleared by the next successful refresh. */
    const FString& GetPreviewError() const { return PreviewError; }

    /** CE-2b: the edit projection of the selected occurrence; null for root sessions or the legacy backend. */
    UMHCompositeEditProjection* GetProjection() const { return Projection; }
    bool OpenProjection(FString& OutError);
    void CloseProjection();

    /** Atomic authoring command on the draft; refused when any target is invalid or procedural. */
    bool SetNodeTransforms(const TArray<FGuid>& NodeIds, const TArray<FTransform>& LocalTransforms, FString& OutError);
    /** Single-target compatibility route. */
    bool SetNodeTransform(const FGuid& NodeId, const FTransform& LocalTransform, FString& OutError);
    /** CE-4b1 structural commands on the draft (see UMHCompositeEditDocument); the projection follows each one. */
    FGuid AddNode(const FGuid& ParentId, EMHCompositeNodeKind Kind, const FString& Resource, const FString& Name, const FTransform& LocalTransform, FString& OutError);
    bool DeleteNode(const FGuid& NodeId, FString& OutError);
    FGuid DuplicateNode(const FGuid& NodeId, FString& OutError);
    /** bKeepWorld: the node keeps where it renders — its local transform is re-authored under the new parent (from the projection). */
    bool ReparentNode(const FGuid& NodeId, const FGuid& NewParentId, int32 SiblingIndex, bool bKeepWorld, FString& OutError);
    /** CE-4b3 metadata and random commands (see UMHCompositeEditDocument); the projection follows. */
    bool SetNodeName(const FGuid& NodeId, const FString& Name, FString& OutError);
    bool SetNodeResource(const FGuid& NodeId, const FString& Resource, FString& OutError);
    FGuid AddRandomNode(const FGuid& ParentId, const FString& Name, const FTransform& LocalTransform, const TArray<FMHCompositeOption>& Options, FString& OutError);
    bool SetNodeOptions(const FGuid& NodeId, const TArray<FMHCompositeOption>& Options, FString& OutError);
    /** The projection follows the draft after a command (a refresh failure is a preview problem, not an authoring one). */
    void RefreshProjection();

    /**
     * CE-1 bridge until CE-4a moves the writes here: mirrors the legacy
     * actor-side handle edits (top-level nodes of the edited definition) into
     * the draft, so the draft is the one current document.
     */
    bool SyncDraftFromLegacyEdit(FString& OutError);

private:
    /** Removes identities that no longer exist and preserves the active id when possible. */
    void PruneSelection();
    /** One derived refresh and one authoring notification per command. */
    void FinishAuthoringCommand(bool bStructureChanged = false);

    UPROPERTY()
    TObjectPtr<UMHCompositeEditDocument> Draft;
    UPROPERTY()
    TObjectPtr<UMHCompositeEditProjection> Projection;
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
    TArray<FGuid> SelectedNodeIds;
    FGuid ActiveNodeId;
    FString PreviewError;
    mutable uint64 DirtySerial = 0;
    mutable bool bDirtyCached = false;
    mutable bool bDirtyCacheValid = false;
};
