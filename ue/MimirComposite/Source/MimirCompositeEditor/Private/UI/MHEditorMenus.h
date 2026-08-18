#pragma once

#include "CoreMinimal.h"

namespace MH::EditorMenus
{

/**
 * Adds the "Mimir" section to the level editor Tools menu: interactive FBX
 * and composite hierarchy dumps for field testing without commandlets.
 * Must be called inside an FToolMenuOwnerScoped scope.
 */
void Populate();

} // namespace MH::EditorMenus
