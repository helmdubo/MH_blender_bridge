#include "Composite/MHCompositeAsset.h"

#if WITH_EDITORONLY_DATA
#include "EditorFramework/AssetImportData.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeAsset)

#if WITH_EDITORONLY_DATA
void UMHCompositeAsset::PostInitProperties()
{
	Super::PostInitProperties();
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad))
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}
}
#endif
