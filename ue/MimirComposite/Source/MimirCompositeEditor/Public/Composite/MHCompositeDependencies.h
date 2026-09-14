#pragma once

#include "CoreMinimal.h"

class UMHCompositeAsset;
class UStaticMesh;

namespace UE::MimirComposite
{
struct MIMIRCOMPOSITEEDITOR_API FMHCompositeDependencyUpdateResult
{
    int32 CompositesVisited = 0;
    int32 MeshesVisited = 0;
    int32 MeshesUpdated = 0;
    int32 SlotsUpdated = 0;
    TArray<FString> Errors;
};

/**
 * Restore mesh material bindings throughout the selected applied definitions,
 * including nested composites and every random option. Uses admitted generated
 * MIs named by the mesh slots; no FBX/source import or receipt rewrite. Changes
 * are undoable and leave the modified mesh packages dirty for normal saving.
 * Unresolvable slots/branches are reported and retain their current bindings.
 */
MIMIRCOMPOSITEEDITOR_API bool MHUpdateCompositeDependencies(
    const TArray<UMHCompositeAsset*>& Roots,
    FMHCompositeDependencyUpdateResult& OutResult);

/**
 * Repair the union of composite closures and explicitly selected mesh assets.
 * Direct meshes need no MH import receipt; their slots use the same admitted
 * generated MI lookup. An empty Roots list never widens to a containing actor.
 */
MIMIRCOMPOSITEEDITOR_API bool MHUpdateAssetDependencies(
    const TArray<UMHCompositeAsset*>& Roots,
    const TArray<UStaticMesh*>& Meshes,
    FMHCompositeDependencyUpdateResult& OutResult);
} // namespace UE::MimirComposite
