#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

enum class EMHEditSessionKeyAction : uint8
{
    None,
    Cancel
};

/** Escape cancels the active session. Enter retains its ordinary widget meaning. */
MIMIRCOMPOSITEEDITOR_API EMHEditSessionKeyAction MHEditSessionKeyAction(const FKey& Key, bool bSessionActive);

/** Outliner key routing; defer teardown until the Slate callback finishes. */
MIMIRCOMPOSITEEDITOR_API bool MHHandleEditSessionKey(const FKey& Key, bool bDeferCancel = true);

/** A queued Cancel cannot affect a different session opened in the meantime. */
MIMIRCOMPOSITEEDITOR_API bool MHRunDeferredEditSessionCancel(uint32 CapturedEpoch);
