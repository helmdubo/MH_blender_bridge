#include "MimirCompositeEditorModule.h"

#include "CoreGlobals.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "Source/MHSourceComposition.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "Engine/StaticMeshSocket.h"
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
        return;
    }

    if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Context.GetObject()))
    {
        UMHStaticMeshImportData* Data = Cast<UMHStaticMeshImportData>(StaticMesh->GetAssetImportData());
        if (Data != nullptr)
        {
            Data->AppendAssetRegistryTags(Context);
        }
    }
}

void MarkManagedStaticMeshLocallyModified(UObject* Object)
{
    if (UE::MimirComposite::MHIsStaticMeshImportMutationSuppressed())
    {
        return;
    }

    UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object);
    if (StaticMesh == nullptr)
    {
        if (const UStaticMeshSocket* Socket = Cast<UStaticMeshSocket>(Object))
        {
            StaticMesh = Cast<UStaticMesh>(Socket->GetOuter());
        }
        else if (const UBodySetup* BodySetup = Cast<UBodySetup>(Object))
        {
            StaticMesh = Cast<UStaticMesh>(BodySetup->GetOuter());
        }
    }
    if (StaticMesh == nullptr)
    {
        return;
    }

    UMHStaticMeshImportData* Data = Cast<UMHStaticMeshImportData>(StaticMesh->GetAssetImportData());
    if (Data == nullptr || Data->bLocallyModified)
    {
        return;
    }

    Data->Modify();
    Data->bLocallyModified = true;
    StaticMesh->MarkPackageDirty();
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

    ObjectModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddStatic(
        &MarkManagedStaticMeshLocallyModified);

    FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
    FMessageLogInitializationOptions LogOptions;
    LogOptions.bShowPages = true;
    LogOptions.bAllowClear = true;
    MessageLogModule.RegisterLogListing("Mimir", INVTEXT("Mimir"), LogOptions);
}

void FMimirCompositeEditorModule::ShutdownModule()
{
    UE::MimirComposite::MHShutdownProjectIndex();
    if (ObjectModifiedHandle.IsValid())
    {
        FCoreUObjectDelegates::OnObjectModified.Remove(ObjectModifiedHandle);
        ObjectModifiedHandle.Reset();
    }
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
