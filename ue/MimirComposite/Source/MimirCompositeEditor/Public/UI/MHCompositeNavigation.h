#pragma once

#include "CoreMinimal.h"

class AMHCompositeActor;
class UMHCompositeEditSession;
class USceneComponent;

namespace UE::MimirComposite
{
class FMHCompositeOutlinerModel;
struct FMHCompositeOutlinerItem;

/** World bounds of one visual; an ISM address never expands to the entire pool. */
MIMIRCOMPOSITEEDITOR_API FBox MHGetVisualFocusBounds(const USceneComponent* Component, int32 InstanceIndex = INDEX_NONE);
MIMIRCOMPOSITEEDITOR_API FBox MHGetPlacementFocusBounds(const AMHCompositeActor& Actor);
MIMIRCOMPOSITEEDITOR_API FBox MHGetOutlinerFocusBounds(FMHCompositeOutlinerModel& Model, const TSharedPtr<FMHCompositeOutlinerItem>& Item);
MIMIRCOMPOSITEEDITOR_API void MHAppendOutlinerAssets(const FMHCompositeOutlinerModel& Model, const FMHCompositeOutlinerItem& Item, TArray<UObject*>& OutAssets);
MIMIRCOMPOSITEEDITOR_API void MHAppendEditSelectionAssets(const UMHCompositeEditSession& Session, TArray<UObject*>& OutAssets);
/** Returns false for ordinary editor selection, leaving the native focus action in charge. */
MIMIRCOMPOSITEEDITOR_API bool MHFocusCompositeSelection();
void MHStartupCompositeNavigation();
void MHShutdownCompositeNavigation();
}
