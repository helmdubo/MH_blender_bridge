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

/** R6-D2: publishes the active edit session (root transforms or Apply Shared Definition) behind the source-overwrite confirmation. */
void MHExecuteCommitEditCompositeInteractive();

enum class EMHCompositeUniqueScope : uint8;
enum class EMHCompositeUniqueVariant : uint8;
/** R6-U1/U2: saves the active Edit Contents draft as unique definitions for Scope (procedural or baked); one name is prompted per copy. */
void MHExecuteSaveUniqueInteractive(EMHCompositeUniqueScope Scope, EMHCompositeUniqueVariant Variant);
/** R6-UX2b: one entry point — a dialog chooses the scope and the bake, then the copies are named and saved. */
void MHExecuteSaveUniqueCopyInteractive();
