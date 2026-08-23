#include "MimirCompositeEditorModule.h"

#include "CoreGlobals.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"

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
}

void FMimirCompositeEditorModule::ShutdownModule()
{
    if (FModuleManager::Get().IsModuleLoaded("MessageLog"))
    {
        FModuleManager::GetModuleChecked<FMessageLogModule>("MessageLog").UnregisterLogListing("Mimir");
    }
}

IMPLEMENT_MODULE(FMimirCompositeEditorModule, MimirCompositeEditor)
