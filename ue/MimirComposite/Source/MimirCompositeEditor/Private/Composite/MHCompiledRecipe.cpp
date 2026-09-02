#include "Composite/MHCompiledRecipe.h"

#include "Editor.h"

namespace UE::MimirComposite
{

namespace
{
const TCHAR* const RecipeRedError = TEXT("MH_E_NOT_IMPLEMENTED: compiled recipes are not implemented (R2a-2 red)");
}

bool MHBuildRecipeGraph(const FMHCompiledRecipe& Root, FMHRandomSourceGraph& OutGraph, FString& OutError)
{
    OutGraph = FMHRandomSourceGraph();
    OutError = RecipeRedError;
    return false;
}

bool MHResolveRecipePreview(
    const FMHCompiledRecipe& Root,
    const int32 Seed,
    const int32 AppearanceSeed,
    FMHResolvedCompositePlan& OutPlan,
    FString& OutError)
{
    OutPlan = FMHResolvedCompositePlan();
    OutError = RecipeRedError;
    return false;
}

bool MHCompareRecipeShadowParity(
    const FMHResolvedCompositePlan& Reference,
    const FMHResolvedCompositePlan& Preview,
    TArray<FString>& OutMismatches)
{
    OutMismatches.Reset();
    OutMismatches.Add(RecipeRedError);
    return false;
}

bool MHRunRecipeShadowParity(
    const UMHCompositeAsset& Root,
    const UMHCompositeSettings& Settings,
    const int32 Seed,
    const int32 AppearanceSeed,
    TArray<FString>& OutMismatches,
    FString& OutError)
{
    OutMismatches.Reset();
    OutError = RecipeRedError;
    return false;
}

} // namespace UE::MimirComposite

using namespace UE::MimirComposite;

void UMHCompiledRecipeRegistry::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
}

void UMHCompiledRecipeRegistry::Deinitialize()
{
    Entries.Empty();
    Dependents.Empty();
    Super::Deinitialize();
}

UMHCompiledRecipeRegistry* UMHCompiledRecipeRegistry::Get()
{
    return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompiledRecipeRegistry>() : nullptr;
}

const FMHCompiledRecipe* UMHCompiledRecipeRegistry::Compile(const UMHCompositeAsset& Asset, FString& OutError)
{
    OutError = RecipeRedError;
    return nullptr;
}

const FMHCompiledRecipe* UMHCompiledRecipeRegistry::Find(const UMHCompositeAsset& Asset) const
{
    return nullptr;
}

void UMHCompiledRecipeRegistry::Invalidate(const UMHCompositeAsset& Asset)
{
}

uint32 UMHCompiledRecipeRegistry::GetRecipeRevision(const UMHCompositeAsset& Asset) const
{
    return 0;
}

EMHCompositeSeedEffect UMHCompiledRecipeRegistry::GetSeedAffectsResult(const UMHCompositeAsset& Asset, FString& OutError)
{
    OutError = RecipeRedError;
    return EMHCompositeSeedEffect::None;
}

TArray<TWeakObjectPtr<const UMHCompositeAsset>> UMHCompiledRecipeRegistry::GetDependents(const FMHResourceKey& Key) const
{
    return {};
}

void UMHCompiledRecipeRegistry::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
}

void UMHCompiledRecipeRegistry::OnAssetReimport(UObject* Object)
{
}
