#include "UI/MHEditSessionKeys.h"

#include "Composite/MHCompositeLevelSubsystem.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor.h"
#include "TimerManager.h"

EMHEditSessionKeyAction MHEditSessionKeyAction(const FKey& Key, const bool bSessionActive)
{
    return bSessionActive && Key == EKeys::Escape ? EMHEditSessionKeyAction::Cancel : EMHEditSessionKeyAction::None;
}

bool MHHandleEditSessionKey(const FKey& Key, const bool bDeferCancel)
{
    const UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (MHEditSessionKeyAction(Key, Subsystem != nullptr && Subsystem->IsEditingComposite()) != EMHEditSessionKeyAction::Cancel)
        return false;
    const uint32 Epoch = Subsystem->GetEditSessionEpoch();
    if (bDeferCancel)
    {
        // Outliner callbacks must finish before Cancel destroys their toolkit.
        GEditor->GetTimerManager()->SetTimerForNextTick([Epoch]() { MHRunDeferredEditSessionCancel(Epoch); });
        return true;
    }
    return MHRunDeferredEditSessionCancel(Epoch);
}

bool MHRunDeferredEditSessionCancel(const uint32 CapturedEpoch)
{
    const UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    if (Subsystem == nullptr || !Subsystem->IsEditingComposite() || Subsystem->GetEditSessionEpoch() != CapturedEpoch || Mode == nullptr)
        return false;
    return Mode->HandleEscape();
}
