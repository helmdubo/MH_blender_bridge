#include "MimirCompositeEditorModule.h"

#include "CoreGlobals.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "UI/MHEditorMenus.h"

void FMimirCompositeEditorModule::StartupModule()
{
    if (IsRunningCommandlet())
    {
        return;
    }

    FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
    FMessageLogInitializationOptions LogOptions;
    LogOptions.bShowPages = true;
    LogOptions.bAllowClear = true;
    MessageLogModule.RegisterLogListing("Mimir", INVTEXT("Mimir"), LogOptions);

    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(
        this, &FMimirCompositeEditorModule::RegisterMenus));
}

void FMimirCompositeEditorModule::ShutdownModule()
{
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
    if (FModuleManager::Get().IsModuleLoaded("MessageLog"))
    {
        FModuleManager::GetModuleChecked<FMessageLogModule>("MessageLog").UnregisterLogListing("Mimir");
    }
}

void FMimirCompositeEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    MH::EditorMenus::Populate();
}

IMPLEMENT_MODULE(FMimirCompositeEditorModule, MimirCompositeEditor)
