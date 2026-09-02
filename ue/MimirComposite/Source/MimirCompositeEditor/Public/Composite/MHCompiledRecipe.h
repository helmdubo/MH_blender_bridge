#pragma once

#include "Composite/MHCompositeAsset.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Random/MHRandomStream.h"
#include "Source/MHSourceResolver.h"
#include "UObject/ObjectKey.h"
#include "UObject/WeakObjectPtr.h"
#include "MHCompiledRecipe.generated.h"

class UMHCompositeSettings;

namespace UE::MimirComposite
{

/** How the component's local transform is produced: fixed, or sampled from placement ranges. */
enum class EMHCompiledTransformKind : uint8
{
    Matrix,
    Ranges,
};

/** One weighted option of a random component. Weights stay raw (Recipe Model v2 §2.1). */
struct MIMIRCOMPOSITEEDITOR_API FMHCompiledRecipeOption
{
    EMHRandomSemanticKind Kind = EMHRandomSemanticKind::Empty;
    FString Resource;
    /** Canonical "kind:name" key; empty for the empty option. */
    FString ResourceKey;
    float WeightRaw = 0.0f;
    /** Index into FMHCompiledRecipe::References for composite options. */
    int32 NestedRecipe = INDEX_NONE;
};

/**
 * One node of the flat recipe program, DFS order. The subtree of component i
 * is the half-open interval [BeginInd, EndInd) of the Components array, with
 * BeginInd == i; a linear walk with a parent-matrix stack replaces recursion.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHCompiledRecipeComponent
{
    /** Recipe-local NodePath in the resolver grammar: "<recipe>:nodes[i]/children[j]…". */
    FString NodePath;
    /** §2.7 identity for NodeOverrides; computed from R6 on, zero until then. */
    uint64 NodeFingerprint = 0;
    EMHRandomSemanticKind Kind = EMHRandomSemanticKind::Group;
    FString Resource;
    /** Canonical "kind:name" key; empty for groups and random nodes. */
    FString ResourceKey;
    /** Presentation only; never identity. */
    FString DisplayName;
    /** Canonical TRS exactly as authored in the source; never pre-multiplied. */
    FMHRandomTrs AuthoredTrs;
    EMHCompiledTransformKind TransformKind = EMHCompiledTransformKind::Matrix;
    /** Named inlined profile (FMHCompiledRecipe::Profiles) or empty. */
    FString ProfileName;
    bool bHasInlinePlacement = false;
    FMHRandomPlacementProfile InlinePlacement;
    bool bAppearanceSeedBoundary = false;
    TArray<FMHCompiledRecipeOption> Options;
    int32 ParentIndex = INDEX_NONE;
    int32 BeginInd = INDEX_NONE;
    int32 EndInd = INDEX_NONE;
    /** Index into FMHCompiledRecipe::References for composite components. */
    int32 NestedRecipe = INDEX_NONE;
};

/** Handle to the compiled recipe of a nested composite asset. */
struct MIMIRCOMPOSITEEDITOR_API FMHCompiledRecipeReference
{
    FString LogicalName;
    TWeakObjectPtr<const UMHCompositeAsset> Asset;
    /** RecipeRevision of the child at the time the parent was compiled. */
    uint32 RecipeRevision = 0;
};

/**
 * Flat program of one composite asset (Recipe Model v2 §2.1). Compiled from
 * MHExtractCompositeV5 and resource keys only: no mesh, material or texture
 * is loaded, no receipt is compared with the Source Root.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHCompiledRecipe
{
    TWeakObjectPtr<const UMHCompositeAsset> Asset;
    FString LogicalName;
    uint32 RecipeRevision = 0;
    /** Debug attribute only; never compared with the Source Root in preview. */
    FString AppliedHashDebug;
    TArray<FMHCompiledRecipeComponent> Components;
    /** Inlined profiles referenced by ProfileName. */
    TMap<FString, FMHRandomPlacementProfile> Profiles;
    TArray<FMHCompiledRecipeReference> References;
    /** True when a seed can change anything in this recipe or a nested one. */
    bool bGenerated = false;
};

/**
 * Preview graph assembled from compiled recipes: raw weights, canonical TRS,
 * nested composites by handle. Never carries RawHashes.
 */
MIMIRCOMPOSITEEDITOR_API bool MHBuildRecipeGraph(
    const FMHCompiledRecipe& Root,
    FMHRandomSourceGraph& OutGraph,
    FString& OutError);

/** Preview path (§3.3): Layout + Appearance on the recipe graph; no closure, no signatures. */
MIMIRCOMPOSITEEDITOR_API bool MHResolveRecipePreview(
    const FMHCompiledRecipe& Root,
    int32 Seed,
    int32 AppearanceSeed,
    FMHResolvedCompositePlan& OutPlan,
    FString& OutError);

/**
 * Shadow parity (§2.3): decisions, draws, nodes, leaves, world matrices,
 * appearance channels and selected dependencies of the reference wrapper
 * against the preview path. Returns true with no mismatch text when equal.
 */
MIMIRCOMPOSITEEDITOR_API bool MHCompareRecipeShadowParity(
    const FMHResolvedCompositePlan& Reference,
    const FMHResolvedCompositePlan& Preview,
    TArray<FString>& OutMismatches);

/** Applied-graph reference wrapper vs compiled-recipe preview for one asset and seed pair. */
MIMIRCOMPOSITEEDITOR_API bool MHRunRecipeShadowParity(
    const UMHCompositeAsset& Root,
    const UMHCompositeSettings& Settings,
    int32 Seed,
    int32 AppearanceSeed,
    TArray<FString>& OutMismatches,
    FString& OutError);

} // namespace UE::MimirComposite

/**
 * Compiled recipe registry (Recipe Model v2 §2.1, R2a). Key: asset +
 * RecipeRevision; the revision increments on PostEditChange and reimport of
 * the asset. Nested composites are references to their own entries.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHCompiledRecipeRegistry final : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    static UMHCompiledRecipeRegistry* Get();

    /** Cached recipe for the asset's current revision, compiling (transitively) when stale. */
    const UE::MimirComposite::FMHCompiledRecipe* Compile(const UMHCompositeAsset& Asset, FString& OutError);

    /** Cached recipe only when it matches the current revision; nullptr otherwise. */
    const UE::MimirComposite::FMHCompiledRecipe* Find(const UMHCompositeAsset& Asset) const;

    /** RecipeRevision++; the next Compile rebuilds this recipe (parents keep their references). */
    void Invalidate(const UMHCompositeAsset& Asset);

    uint32 GetRecipeRevision(const UMHCompositeAsset& Asset) const;

    /** Global counter, incremented by every compilation of any recipe. */
    uint32 GetGeneration() const { return Generation; }

    /** Cached seed classification of the compiled recipe, stamped with the generation. */
    EMHCompositeSeedEffect GetSeedAffectsResult(const UMHCompositeAsset& Asset, FString& OutError);

    /** Reverse index for rematerialize localization only; never triggers parent recompilation. */
    TArray<TWeakObjectPtr<const UMHCompositeAsset>> GetDependents(const UE::MimirComposite::FMHResourceKey& Key) const;

private:
    struct FEntry
    {
        uint32 RecipeRevision = 0;
        TSharedPtr<UE::MimirComposite::FMHCompiledRecipe> Recipe;
        bool bSeedEffectValid = false;
        uint32 SeedEffectGeneration = 0;
        EMHCompositeSeedEffect SeedEffect = EMHCompositeSeedEffect::None;
    };

    void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
    void OnAssetReimport(UObject* Object);
    void RunParityCommand(const TArray<FString>& Args);

    /** Compile with the cycle stack of the enclosing compilation (MH_E_COMPOSITE_CYCLE). */
    const UE::MimirComposite::FMHCompiledRecipe* CompileWithStack(
        const UMHCompositeAsset& Asset,
        TArray<FObjectKey>& Stack,
        FString& OutError);
    TSharedPtr<UE::MimirComposite::FMHCompiledRecipe> CompileRecipe(
        const UMHCompositeAsset& Asset,
        uint32 RecipeRevision,
        TArray<FObjectKey>& Stack,
        FString& OutError);
    void IndexDependents(const UE::MimirComposite::FMHCompiledRecipe& Recipe, const FObjectKey& Owner);

    TMap<FObjectKey, FEntry> Entries;
    TMap<UE::MimirComposite::FMHResourceKey, TSet<FObjectKey>> Dependents;
    uint32 Generation = 0;
    FDelegateHandle PropertyChangedHandle;
    FDelegateHandle ReimportHandle;
    struct IConsoleCommand* ParityCommand = nullptr;
};
