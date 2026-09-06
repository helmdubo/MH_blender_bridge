#include "UI/MHEditSessionKeys.h"

#include "Composite/MHCompositeLevelSubsystem.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Logging/MessageLog.h"
#include "TimerManager.h"
#include "UI/MHSourceToolMenus.h"
#include "Widgets/SWidget.h"

namespace
{

UMHCompositeLevelSubsystem* EditSessionSubsystem()
{
    return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
}

/**
 * R6-UX2a: while a session is active, Esc and Enter in the level viewport
 * belong to the session. Anywhere else (text boxes, other panels) the keys
 * keep their meaning; the Composite Outliner routes its own keys.
 */
class FMHEditSessionInputProcessor final : public IInputProcessor
{
public:
    virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
    virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& KeyEvent) override
    {
        if (KeyEvent.IsRepeat()) return false;
        const UMHCompositeLevelSubsystem* Subsystem = EditSessionSubsystem();
        const EMHEditSessionKeyAction Action = MHEditSessionKeyAction(KeyEvent.GetKey(), Subsystem != nullptr && Subsystem->IsEditingComposite());
        if (Action == EMHEditSessionKeyAction::None) return false;
        // CE-3b: under the Composite Edit Mode Enter means nothing (Save is a button).
        if (Action == EMHEditSessionKeyAction::Apply && UMHCompositeEditorMode::IsActive()) return false;
        const TSharedPtr<SWidget> Focused = SlateApp.GetKeyboardFocusedWidget();
        if (!Focused.IsValid() || Focused->GetType() != TEXT("SViewport")) return false;
        return MHHandleEditSessionKey(KeyEvent.GetKey());
    }
    virtual const TCHAR* GetDebugName() const override { return TEXT("MHEditSessionKeys"); }
};

TSharedPtr<FMHEditSessionInputProcessor> GProcessor;

} // namespace

EMHEditSessionKeyAction MHEditSessionKeyAction(const FKey& Key, const bool bSessionActive)
{
    if (!bSessionActive) return EMHEditSessionKeyAction::None;
    if (Key == EKeys::Escape) return EMHEditSessionKeyAction::Cancel;
    if (Key == EKeys::Enter) return EMHEditSessionKeyAction::Apply;
    return EMHEditSessionKeyAction::None;
}

bool MHHandleEditSessionKey(const FKey& Key, const bool bDeferApply)
{
    UMHCompositeLevelSubsystem* Subsystem = EditSessionSubsystem();
    switch (MHEditSessionKeyAction(Key, Subsystem != nullptr && Subsystem->IsEditingComposite()))
    {
    case EMHEditSessionKeyAction::Cancel:
    {
        // CE-3b: under the mode Escape is the mode's Cancel (asks when the
        // draft is dirty). The question is modal: never open it from inside
        // the input path that delivered the key.
        if (UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive())
        {
            if (bDeferApply && GEditor != nullptr)
            {
                const uint32 Epoch = Subsystem->GetEditSessionEpoch();
                GEditor->GetTimerManager()->SetTimerForNextTick([Epoch]()
                {
                    const UMHCompositeLevelSubsystem* Current = EditSessionSubsystem();
                    UMHCompositeEditorMode* CurrentMode = UMHCompositeEditorMode::GetActive();
                    if (Current != nullptr && Current->IsEditingComposite() && Current->GetEditSessionEpoch() == Epoch && CurrentMode != nullptr) CurrentMode->RequestCancel();
                });
            }
            else
            {
                Mode->RequestCancel();
            }
            return true;
        }
        FString Error;
        if (!Subsystem->CancelEditComposite(Error) && !Error.IsEmpty())
        {
            FMessageLog("Mimir").Error(FText::FromString(Error));
        }
        if (GEditor != nullptr) GEditor->RedrawLevelEditingViewports();
        return true;
    }
    case EMHEditSessionKeyAction::Apply:
        // CE-3b: Enter never publishes under the mode.
        if (UMHCompositeEditorMode::IsActive()) return false;
        // The confirmation is modal: never open it from inside the input
        // path that delivered the key.
        if (bDeferApply && GEditor != nullptr)
        {
            const uint32 Epoch = Subsystem->GetEditSessionEpoch();
            GEditor->GetTimerManager()->SetTimerForNextTick([Epoch]() { MHRunDeferredEditSessionApply(Epoch); });
        }
        else
        {
            MHExecuteCommitEditCompositeInteractive();
        }
        return true;
    default:
        return false;
    }
}

bool MHRunDeferredEditSessionApply(const uint32 CapturedEpoch)
{
    // CE §9: the callback belongs to the session it was queued for. A closed
    // session or a newer one leaves the current state untouched.
    const UMHCompositeLevelSubsystem* Subsystem = EditSessionSubsystem();
    if (Subsystem == nullptr || !Subsystem->IsEditingComposite() || Subsystem->GetEditSessionEpoch() != CapturedEpoch) return false;
    MHExecuteCommitEditCompositeInteractive();
    return true;
}

void MHRegisterEditSessionKeys()
{
    if (GProcessor.IsValid() || !FSlateApplication::IsInitialized()) return;
    GProcessor = MakeShared<FMHEditSessionInputProcessor>();
    FSlateApplication::Get().RegisterInputPreProcessor(GProcessor);
}

void MHUnregisterEditSessionKeys()
{
    if (!GProcessor.IsValid()) return;
    if (FSlateApplication::IsInitialized()) FSlateApplication::Get().UnregisterInputPreProcessor(GProcessor);
    GProcessor.Reset();
}
