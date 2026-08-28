#pragma once

#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"
#include "Source/MHSourceResolver.h"

class AMHCompositeActor;
class UMHCompositeAsset;
class UMHCompositeSettings;
class UStaticMesh;

namespace UE::MimirComposite
{
/** Session-only successful applied inputs, not an index or a second authority. */
MIMIRCOMPOSITEEDITOR_API void MHStartupCompositePreviewCache();
MIMIRCOMPOSITEEDITOR_API void MHShutdownCompositePreviewCache();
MIMIRCOMPOSITEEDITOR_API void MHInvalidateCompositePreviewCache(const FMHResourceKey* ChangedKey = nullptr);
MIMIRCOMPOSITEEDITOR_API uint64 MHCompositePreviewRevision(const TSet<FMHResourceKey>& Dependencies);
MIMIRCOMPOSITEEDITOR_API TSharedPtr<const FMHRandomSourceGraph> MHGetCompositePreviewGraph(
    const UMHCompositeAsset& Root, const UMHCompositeSettings& Settings,
    TSet<FMHResourceKey>& Dependencies, FString& Error, UStaticMesh*& PendingMesh);
MIMIRCOMPOSITEEDITOR_API void MHDeferCompositePreview(AMHCompositeActor& Actor, UStaticMesh& Mesh);
}
