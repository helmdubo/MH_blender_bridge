#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{
struct FMHCompositeAdoptTarget;
}

/** Shared modal used by Build/Publish and external .composite file-drop Adopt. */
bool MHPromptCompositeAdoptTarget(
    UE::MimirComposite::FMHCompositeAdoptTarget& OutTarget,
    const FString& SuggestedName,
    const FText& WindowTitle,
    const FText& AcceptLabel);

/** Registers the S6 project, placement and managed-asset editor commands. */
void MHRegisterS6ToolMenus();

/** Publishes the active edit session directly; Save is the explicit overwrite action. */
void MHExecuteCommitEditCompositeInteractive();

/** R6-UX2b: one entry point — a dialog chooses the scope and the bake, then the copies are named and saved. */
void MHExecuteSaveUniqueCopyInteractive();
