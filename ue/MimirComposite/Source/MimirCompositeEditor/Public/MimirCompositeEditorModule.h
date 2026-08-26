#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FMimirCompositeEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    void UnregisterMenusBeforeExit();

    FDelegateHandle AssetRegistryTagsHandle;
    FDelegateHandle ObjectModifiedHandle;
    FDelegateHandle EnginePreExitHandle;
    bool bOwnsToolMenusRegistration = false;
};
