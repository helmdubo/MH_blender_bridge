#pragma once

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "UObject/Object.h"
#include "MHCompositeEditDocument.generated.h"

/**
 * CE-1 (docs/contracts/composite_edit_ce0.md, spec §5.2): the transactional
 * authoring draft of one composite definition. The tree lives in the same
 * reflected flat form the managed asset uses (`FMHCompositeAssetNode`,
 * pre-order with parent indices), so `Modify()` puts the whole draft into the
 * native transaction and Undo/Redo restore it exactly — options, empties,
 * zero weights, profiles, order and the session-local node ids alike.
 *
 * Node ids are session-local: never written to v5, never part of the RNG.
 * The typed `FMHCompositeDocument` view is rebuilt on demand and cached by a
 * change serial that also advances on Undo.
 */
UCLASS(Transient)
class MIMIRCOMPOSITEEDITOR_API UMHCompositeEditDocument : public UObject
{
    GENERATED_BODY()

public:
    /** Replaces the draft with Document; fresh ids, revision 0. */
    void Load(const UE::MimirComposite::FMHCompositeDocument& Document, TConstArrayView<FMHPlacementProfile> InlinedProfiles = TConstArrayView<FMHPlacementProfile>());
    /** The typed document of the current draft (cached until the next change or Undo). */
    bool Extract(UE::MimirComposite::FMHCompositeDocument& OutDocument, FString& OutError) const;
    /** Canonical v5 bytes of the current draft. */
    bool CanonicalBytes(TArray<uint8>& OutBytes, FString& OutError) const;

    int32 Num() const { return Nodes.Num(); }
    const TArray<FMHCompositeAssetNode>& GetNodes() const { return Nodes; }
    const TArray<FMHPlacementProfile>& GetInlinedProfiles() const { return InlinedProfiles; }
    FGuid GetNodeId(int32 Index) const { return NodeIds.IsValidIndex(Index) ? NodeIds[Index] : FGuid(); }
    int32 FindNodeIndex(const FGuid& Id) const { return Id.IsValid() ? NodeIds.IndexOfByKey(Id) : INDEX_NONE; }
    /** Pre-order index of the node a v5 selector names ("nodes[1]/children[0]"); options are not nodes. */
    int32 FindNodeIndexBySelector(const FString& Selector) const;
    /** The v5 selector of a node ("nodes[1]/children[0]"); empty for an invalid index. */
    FString GetSelector(int32 Index) const;
    /** Advances on every authoring change and on Undo/Redo; a cache key, not a version. */
    uint64 GetChangeSerial() const { return ChangeSerial; }
    /** Authoring revision: advances on every command; restored by Undo. */
    uint32 GetRevision() const { return Revision; }

    /** Authoring command: the node's authored local transform. Modify()s the draft first. */
    bool SetNodeTransform(const FGuid& Id, const FTransform& LocalTransform, FString& OutError);

    /**
     * CE-4b1 structural commands. Every command validates first (grammar of
     * the kind, tree shape), then Modify()s the draft and keeps the pre-order
     * node array, parent indices and session ids consistent — Undo restores
     * order, ids and metadata. Random nodes need options and are not added
     * here (CE-4b3).
     */
    /** Appends a node as the last child of ParentId (invalid = a new root); returns its id, invalid on refusal. */
    FGuid AddNode(const FGuid& ParentId, EMHCompositeNodeKind Kind, const FString& Resource, const FString& Name, const FTransform& LocalTransform, FString& OutError);
    /** Removes the node with its subtree. */
    bool DeleteNode(const FGuid& Id, FString& OutError);
    /** Copies the node's subtree right after it under the same parent, with fresh ids; returns the copy's id. */
    FGuid DuplicateNode(const FGuid& Id, FString& OutError);
    /** Moves the node's subtree under NewParentId (invalid = root) at SiblingIndex (INDEX_NONE = last); the local transform is kept as is. */
    bool ReparentNode(const FGuid& Id, const FGuid& NewParentId, int32 SiblingIndex, FString& OutError);
    /** CE-4b3 metadata and random commands (same rules: validate, Modify(), Undo restores). */
    bool SetNodeName(const FGuid& Id, const FString& Name, FString& OutError);
    /** Resource kinds only (mesh/actor/composite/gameobj), canonical [a-z0-9_]+. */
    bool SetNodeResource(const FGuid& Id, const FString& Resource, FString& OutError);
    /** A random node with validated options as the last child of ParentId (invalid = root). */
    FGuid AddRandomNode(const FGuid& ParentId, const FString& Name, const FTransform& LocalTransform, const TArray<FMHCompositeOption>& Options, FString& OutError);
    /** Replaces a random node's options (validated as a whole). */
    bool SetNodeOptions(const FGuid& Id, const TArray<FMHCompositeOption>& Options, FString& OutError);
    /** The writer's rules for options: non-empty, finite non-negative weights with one positive, empty options without and other options with a canonical resource. */
    static bool ValidateOptions(const TArray<FMHCompositeOption>& Options, FString& OutError);
    /** Parent id of a node; invalid for roots and unknown ids. */
    FGuid GetParentId(const FGuid& Id) const;
    /** Child ids of a node in order; the roots for an invalid id. */
    TArray<FGuid> GetChildIds(const FGuid& ParentId) const;

    virtual void PostEditUndo() override;
    /** CE-4a: fired after Undo/Redo replaced the reflected state (the session refreshes its projection). */
    FSimpleDelegate OnRestored;

private:
    void MarkChanged();
    /** Index after the last descendant of Index (pre-order). */
    int32 SubtreeEnd(int32 Index) const;
    /** Inserts a block (relative parents, INDEX_NONE = the block's root) at At under ParentIndex, shifting later parents. */
    void InsertBlock(int32 At, int32 ParentIndex, TArray<FMHCompositeAssetNode> Block, TArray<FGuid> Ids);
    /** Removes the subtree at Index into a block with relative parents, shifting later parents. */
    void ExtractBlock(int32 Index, TArray<FMHCompositeAssetNode>& OutBlock, TArray<FGuid>& OutIds);

    UPROPERTY()
    TArray<FMHCompositeAssetNode> Nodes;
    UPROPERTY()
    TArray<FGuid> NodeIds;
    UPROPERTY()
    TArray<FMHPlacementProfile> InlinedProfiles;
    UPROPERTY()
    uint32 Revision = 0;

    uint64 ChangeSerial = 1;
    mutable uint64 CacheSerial = 0;
    mutable UE::MimirComposite::FMHCompositeDocument CachedDocument;
    mutable FString CachedError;
};
