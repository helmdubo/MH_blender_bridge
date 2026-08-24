#include "Material/MHMaterialSourceData.h"

#include "UObject/AssetRegistryTagsContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHMaterialSourceData)

#if WITH_EDITOR
void UMHMaterialSourceData::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
    // This emitter is invoked with the owning material's tag context. Calling
    // Super here would rebroadcast the global extra-object-tag delegates.
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Kind"), TEXT("material"), UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.LogicalName"), LogicalName, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.SourcePath"), SourceRelativePath, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.SourceHash"), SourceHash, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.AppliedHash"), AppliedHash, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Managed"), TEXT("True"), UObject::FAssetRegistryTag::TT_Alphabetical));
}
#endif
