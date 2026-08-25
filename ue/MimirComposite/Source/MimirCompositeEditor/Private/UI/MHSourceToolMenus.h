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
