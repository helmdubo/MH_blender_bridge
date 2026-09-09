#pragma once

#include "Composite/MHCompositeAsset.h"
#include "CoreMinimal.h"
#include "Misc/Guid.h"

class UMHCompositeEditDocument;
class UMHCompositeEditSession;

namespace UE::MimirComposite
{
struct FMHCompositeOutlinerItem;
}

/** CE-4b2: what a Composite Outliner add (menu or drop) asks the session for. */
struct MIMIRCOMPOSITEEDITOR_API FMHOutlinerAddRequest
{
    /** Invalid = a new root of the edited definition. */
    FGuid ParentId;
    EMHCompositeNodeKind Kind = EMHCompositeNodeKind::Group;
    FString Resource;
    FString Name;
};

/** Identity captured by a deferred authoring callback. */
struct MIMIRCOMPOSITEEDITOR_API FMHOutlinerCommandStamp
{
    TWeakObjectPtr<const UMHCompositeEditSession> Session;
    FGuid SessionId;
    uint32 Epoch = 0;
    uint64 DraftChangeSerial = 0;

    static FMHOutlinerCommandStamp Capture(const UMHCompositeEditSession& InSession);
    bool Matches(const UMHCompositeEditSession& InSession) const;
};

/**
 * Resolves the exact destination. Every authored node can own children;
 * locked/variant rows fail, and root requires an explicit root action.
 */
MIMIRCOMPOSITEEDITOR_API bool MHResolveOutlinerAddParent(
    const UE::MimirComposite::FMHCompositeOutlinerItem* Target,
    bool bExplicitRoot,
    const UMHCompositeEditDocument& Draft,
    FGuid& OutParentId,
    FString& OutError);

/** Resolves one atomic sibling-block insertion before an above/below drop. */
MIMIRCOMPOSITEEDITOR_API bool MHResolveOutlinerSiblingInsertion(
    const UE::MimirComposite::FMHCompositeOutlinerItem* Target,
    bool bBelow,
    const UMHCompositeEditDocument& Draft,
    FGuid& OutParentId,
    int32& OutSiblingIndex,
    FString& OutError);

/**
 * The node an asset becomes when dropped on / added at a row: a managed
 * static mesh (with its MH import receipt) is a mesh node, a managed
 * composite a composite node; anything else is refused with the reason.
 */
MIMIRCOMPOSITEEDITOR_API bool MHDescribeOutlinerAssetAdd(
    const UObject* Asset,
    const UE::MimirComposite::FMHCompositeOutlinerItem* Target,
    const UMHCompositeEditDocument& Draft,
    FMHOutlinerAddRequest& OutRequest,
    FString& OutError);

/** Prevalidates a complete Content Browser/drop batch before the session mutates. */
MIMIRCOMPOSITEEDITOR_API bool MHDescribeOutlinerAssetAddBatch(
    TConstArrayView<UObject*> Assets,
    const UE::MimirComposite::FMHCompositeOutlinerItem* Target,
    bool bExplicitRoot,
    const UMHCompositeEditDocument& Draft,
    TArray<FMHOutlinerAddRequest>& OutRequests,
    FString& OutError);

/** Describes one managed asset as a weight-one content variant. */
MIMIRCOMPOSITEEDITOR_API bool MHDescribeOutlinerAssetOption(
    const UObject* Asset,
    FMHCompositeOption& OutOption,
    FString& OutError);
