#include "StaticMesh/MHStaticMeshImportData.h"

#include "UObject/AssetRegistryTagsContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHStaticMeshImportData)

namespace
{

int32 GStaticMeshImportMutationSuppressionDepth = 0;

} // namespace

namespace UE::MimirComposite
{

FMHScopedStaticMeshImportMutation::FMHScopedStaticMeshImportMutation()
{
    ++GStaticMeshImportMutationSuppressionDepth;
}

FMHScopedStaticMeshImportMutation::~FMHScopedStaticMeshImportMutation()
{
    check(GStaticMeshImportMutationSuppressionDepth > 0);
    --GStaticMeshImportMutationSuppressionDepth;
}

bool MHIsStaticMeshImportMutationSuppressed()
{
    return GStaticMeshImportMutationSuppressionDepth > 0;
}

} // namespace UE::MimirComposite

#if WITH_EDITOR
void UMHStaticMeshImportData::AppendAssetRegistryTags(FAssetRegistryTagsContext Context)
{
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Kind"), TEXT("static_mesh"), UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.LogicalName"), LogicalName, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.SourcePath"), SourceRelativePath, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.SourceHash"), SourceHash, UObject::FAssetRegistryTag::TT_Alphabetical));
    // Binary applied state is the imported source payload by definition.
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.AppliedHash"), SourceHash, UObject::FAssetRegistryTag::TT_Alphabetical));
    Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Managed"), TEXT("True"), UObject::FAssetRegistryTag::TT_Alphabetical));
}
#endif
