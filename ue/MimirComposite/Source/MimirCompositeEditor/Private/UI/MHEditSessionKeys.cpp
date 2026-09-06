#include "UI/MHEditSessionKeys.h"

// R6-UX2a red stub.

EMHEditSessionKeyAction MHEditSessionKeyAction(const FKey& Key, const bool bSessionActive)
{
    static_cast<void>(Key);
    static_cast<void>(bSessionActive);
    return EMHEditSessionKeyAction::None;
}

bool MHHandleEditSessionKey(const FKey& Key, const bool bDeferApply)
{
    static_cast<void>(Key);
    static_cast<void>(bDeferApply);
    return false;
}

void MHRegisterEditSessionKeys()
{
}

void MHUnregisterEditSessionKeys()
{
}
