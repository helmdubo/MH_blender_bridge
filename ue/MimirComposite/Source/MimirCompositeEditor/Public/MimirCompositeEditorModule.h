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
    FDelegateHandle LevelEditorCreatedHandle;
    /** R5b-2b: the pool-instance selection seam lives on the level editor's element selection set. */
    void RegisterPoolInstanceSelection();
    bool bOwnsToolMenusRegistration = false;
};
