#include "Composite/MHCompositeAsset.h"

#include "UObject/AssetRegistryTagsContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeAsset)

#if WITH_EDITOR
void UMHCompositeAsset::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
    Super::GetAssetRegistryTags(Context);
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Kind"), TEXT("composite"), UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.LogicalName"), LogicalName, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.SourcePath"), SourceRelativePath, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.AppliedHash"), AppliedHash, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Managed"), TEXT("True"), UObject::FAssetRegistryTag::TT_Alphabetical));
}
#endif
