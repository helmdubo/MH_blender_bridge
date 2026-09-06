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

    virtual void PostEditUndo() override;

private:
    void MarkChanged();

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
