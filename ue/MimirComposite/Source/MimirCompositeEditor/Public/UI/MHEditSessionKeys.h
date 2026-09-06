#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

/** R6-UX2a: what a key means while a composite edit session is active. */
enum class EMHEditSessionKeyAction : uint8
{
    None,
    /** Enter: publish the session behind the usual source-overwrite confirmation. */
    Apply,
    /** Escape: discard the draft and close the session. */
    Cancel
};

/** Pure decision: Esc -> Cancel, Enter -> Apply while a session is active; None otherwise. */
MIMIRCOMPOSITEEDITOR_API EMHEditSessionKeyAction MHEditSessionKeyAction(const FKey& Key, bool bSessionActive);

/**
 * Runs the key's action on the active session: Cancel immediately, Apply
 * through the interactive commit (confirmation). bDeferApply runs the commit
 * on the next editor tick, outside the input path that delivered the key.
 * Returns true when the key was consumed.
 */
MIMIRCOMPOSITEEDITOR_API bool MHHandleEditSessionKey(const FKey& Key, bool bDeferApply = true);

/**
 * Runs a deferred Apply captured for the session that had CapturedEpoch. Returns
 * false without touching anything when that session is gone or another one
 * has begun since (CE §9: stale queued Apply is a no-op).
 */
MIMIRCOMPOSITEEDITOR_API bool MHRunDeferredEditSessionApply(uint32 CapturedEpoch);

/** Registers/unregisters the Slate input pre-processor that routes viewport Esc/Enter to the session. */
void MHRegisterEditSessionKeys();
void MHUnregisterEditSessionKeys();
