#include "MimirCompositeEditorModule.h"

#include "CoreGlobals.h"
#include "Engine/Texture.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "Source/MHSourceComposition.h"
#include "Texture/MHTextureSourceData.h"
#include "UObject/AssetRegistryTagsContext.h"

namespace
{

void AddMimirAssetRegistryTags(FAssetRegistryTagsContext Context)
{
    if (const UMaterialInstanceConstant* Material = Cast<UMaterialInstanceConstant>(Context.GetObject()))
    {
        const UMHMaterialSourceData* Data = Cast<UMHMaterialSourceData>(
            const_cast<UMaterialInstanceConstant*>(Material)->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
        if (Data != nullptr)
        {
            Data->GetAssetRegistryTags(Context);
        }
        return;
    }

    if (const UTexture* Texture = Cast<UTexture>(Context.GetObject()))
    {
        const UMHTextureSourceData* Data = Cast<UMHTextureSourceData>(
            const_cast<UTexture*>(Texture)->GetAssetUserDataOfClass(UMHTextureSourceData::StaticClass()));
        if (Data != nullptr)
        {
            Data->GetAssetRegistryTags(Context);
        }
    }
}

} // namespace

void FMimirCompositeEditorModule::StartupModule()
{
    AssetRegistryTagsHandle = UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.AddStatic(
        &AddMimirAssetRegistryTags);
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
    UE::MimirComposite::MHShutdownProjectIndex();
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
