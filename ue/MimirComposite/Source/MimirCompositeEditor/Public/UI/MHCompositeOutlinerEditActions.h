#pragma once

#include "Composite/MHCompositeAsset.h"
#include "CoreMinimal.h"
#include "Misc/Guid.h"

class UMHCompositeEditDocument;

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

/**
 * Where a node added "at" a row goes: into the row when it is a group of the
 * draft, next to it (under its parent) for any other draft row, at the root
 * when there is no row or the row is not part of the draft.
 */
MIMIRCOMPOSITEEDITOR_API FGuid MHOutlinerAddParentFor(const UE::MimirComposite::FMHCompositeOutlinerItem* Target, const UMHCompositeEditDocument& Draft);

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
