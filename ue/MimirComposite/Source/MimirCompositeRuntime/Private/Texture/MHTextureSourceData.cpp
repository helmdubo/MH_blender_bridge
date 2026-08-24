#include "Texture/MHTextureSourceData.h"

#include "UObject/AssetRegistryTagsContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHTextureSourceData)

#if WITH_EDITOR
void UMHTextureSourceData::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Kind"), TEXT("texture"), UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.LogicalName"), LogicalName, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.SourcePath"), SourceRelativePath, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.SourceHash"), SourceHash, UObject::FAssetRegistryTag::TT_Alphabetical));
    // Binary applied state is the imported source payload by definition.
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.AppliedHash"), SourceHash, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Managed"), TEXT("True"), UObject::FAssetRegistryTag::TT_Alphabetical));
}
#endif
