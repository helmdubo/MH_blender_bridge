#include "MimirCompositeEditorModule.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserMenuContexts.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Logging/MessageLog.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "Source/MHSourceComposition.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "Engine/StaticMeshSocket.h"
#include "Source/MHSourceImporter.h"
#include "Texture/MHTextureSourceData.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "UI/MHSourceToolMenus.h"
#include "UObject/AssetRegistryTagsContext.h"

#define LOCTEXT_NAMESPACE "MimirCompositeEditor"

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

void ExecuteReimportManagedMaterials(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    if (Context == nullptr)
    {
        return;
    }

    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(LOCTEXT("ReimportManagedMaterialsPage", "Reimport managed materials"));
    UMHSourceImporter* Importer = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>()
        : nullptr;
    if (Importer == nullptr)
    {
        Log.Error(LOCTEXT(
            "MissingSourceImporter",
            "MH_E_IMPORT_THREAD_INVALID: MH Source Importer subsystem is unavailable"));
        Log.Notify(
            LOCTEXT("ManagedMaterialReimportFailed", "MH material reimport failed"),
            EMessageSeverity::Error,
            true);
        return;
    }

    int32 Succeeded = 0;
    int32 Failed = 0;
    const TArray<UMaterialInstanceConstant*> Materials =
        Context->LoadSelectedObjects<UMaterialInstanceConstant>();
    for (UMaterialInstanceConstant* Material : Materials)
    {
        if (Material == nullptr)
        {
            continue;
        }
        TArray<FString> Warnings;
        FString Error;
        if (!Importer->ReimportMaterial(Material, Warnings, Error))
        {
            ++Failed;
            Log.Error(FText::FromString(FString::Printf(
                TEXT("%s: %s"),
                *Material->GetPathName(),
                *Error)));
            continue;
        }

        ++Succeeded;
        Log.Info(FText::FromString(FString::Printf(
            TEXT("Reimported %s from the current project source document"),
            *Material->GetPathName())));
        for (const FString& Warning : Warnings)
        {
            Log.Warning(FText::FromString(FString::Printf(
                TEXT("%s: %s"),
                *Material->GetPathName(),
                *Warning)));
        }
    }

    if (Materials.IsEmpty())
    {
        Log.Error(LOCTEXT(
            "NoMaterialInstancesSelected",
            "MH_E_INVALID_RESOURCE_SOURCE: no Material Instance assets were selected"));
        ++Failed;
    }
    Log.Notify(
        FText::Format(
            LOCTEXT(
                "ManagedMaterialReimportSummary",
                "MH material reimport: {0} succeeded, {1} failed"),
            FText::AsNumber(Succeeded),
            FText::AsNumber(Failed)),
        Failed > 0 ? EMessageSeverity::Error : EMessageSeverity::Info,
        true);
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

    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMimirCompositeEditorModule::RegisterMenus));
}

void FMimirCompositeEditorModule::ShutdownModule()
{
    UE::MimirComposite::MHShutdownProjectIndex();
    if (!IsRunningCommandlet())
    {
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
    }
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

void FMimirCompositeEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenu* Menu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(
        UMaterialInstanceConstant::StaticClass());
    if (Menu != nullptr)
    {
        FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("GetAssetActions"));
        Section.AddDynamicEntry(
            TEXT("MHManagedMaterialActions"),
            FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& DynamicSection)
            {
                const UContentBrowserAssetContextMenuContext* Context =
                    UContentBrowserAssetContextMenuContext::FindContextWithAssets(DynamicSection);
                if (Context == nullptr)
                {
                    return;
                }

                const bool bHasMaterialInstance = Context->SelectedAssets.ContainsByPredicate(
                    [](const FAssetData& Asset)
                    {
                        return Asset.IsInstanceOf(UMaterialInstanceConstant::StaticClass());
                    });
                if (!bHasMaterialInstance)
                {
                    return;
                }

                FToolUIAction Action;
                Action.ExecuteAction = FToolMenuExecuteAction::CreateStatic(
                    &ExecuteReimportManagedMaterials);
                DynamicSection.AddMenuEntry(
                    TEXT("MHReimportManagedMaterials"),
                    LOCTEXT("ReimportManagedMaterials", "Reimport from MH Source"),
                    LOCTEXT(
                        "ReimportManagedMaterialsTooltip",
                        "Force a full source-wins reimport of the selected managed Material Instances. "
                        "Parent, textures, parameters and base overrides are replaced from the current .material files."),
                    FSlateIcon(),
                    Action);
            }));
    }
    MHRegisterS6ToolMenus();
}

IMPLEMENT_MODULE(FMimirCompositeEditorModule, MimirCompositeEditor)

#undef LOCTEXT_NAMESPACE
