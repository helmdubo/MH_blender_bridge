#include "MimirCompositeEditorModule.h"

#include "CoreGlobals.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "UObject/AssetRegistryTagsContext.h"

namespace
{

void AddMimirMaterialAssetRegistryTags(FAssetRegistryTagsContext Context)
{
    const UMaterialInstanceConstant* Material = Cast<UMaterialInstanceConstant>(Context.GetObject());
    if (Material == nullptr)
    {
        return;
    }
    const UMHMaterialSourceData* Data = Cast<UMHMaterialSourceData>(
        const_cast<UMaterialInstanceConstant*>(Material)->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    if (Data != nullptr)
    {
        Data->GetAssetRegistryTags(Context);
    }
}

} // namespace

void FMimirCompositeEditorModule::StartupModule()
{
    AssetRegistryTagsHandle = UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.AddStatic(
        &AddMimirMaterialAssetRegistryTags);
    if (IsRunningCommandlet())
    {
        return;
    }

    FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
    FMessageLogInitializationOptions LogOptions;
    LogOptions.bShowPages = true;
    LogOptions.bAllowClear = true;
    MessageLogModule.RegisterLogListing("Mimir", INVTEXT("Mimir"), LogOptions);
}

void FMimirCompositeEditorModule::ShutdownModule()
{
    if (AssetRegistryTagsHandle.IsValid())
    {
        UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.Remove(AssetRegistryTagsHandle);
        AssetRegistryTagsHandle.Reset();
    }
    if (FModuleManager::Get().IsModuleLoaded("MessageLog"))
    {
        FModuleManager::GetModuleChecked<FMessageLogModule>("MessageLog").UnregisterLogListing("Mimir");
    }
}

IMPLEMENT_MODULE(FMimirCompositeEditorModule, MimirCompositeEditor)
