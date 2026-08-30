#include "UI/MHSourceOverwritePolicy.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "Settings/MHCompositeSettings.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace UE::MimirComposite
{
namespace
{

#if WITH_DEV_AUTOMATION_TESTS
FMHSourceOverwritePolicyTestHooks GTestHooks;
#endif

bool ConfirmOverwrite(const FText& Confirmation)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (GTestHooks.Confirm)
    {
        return GTestHooks.Confirm(Confirmation);
    }
#endif
    return FMessageDialog::Open(EAppMsgType::YesNo, Confirmation) == EAppReturnType::Yes;
}

void NotifyOverwrite(const FText& Audit)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (GTestHooks.Notify)
    {
        GTestHooks.Notify(Audit);
        return;
    }
#endif
    if (FSlateApplication::IsInitialized() && !IsRunningCommandlet())
    {
        FNotificationInfo Info(Audit);
        Info.bFireAndForget = true;
        Info.ExpireDuration = 5.0f;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
}

void LogOverwrite(const FText& Audit)
{
#if WITH_DEV_AUTOMATION_TESTS
    if (GTestHooks.MessageLog)
    {
        GTestHooks.MessageLog(Audit);
        return;
    }
#endif
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(INVTEXT("Source overwrite"));
    Log.Info(Audit);
}

} // namespace

EMHSourceOverwriteExecution MHExecuteSourceOverwrite(
    const FString& SourceFile,
    const FText& Confirmation,
    const FText& SuccessAudit,
    TFunctionRef<bool()> Operation)
{
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const bool bConfirm = Settings == nullptr || Settings->bConfirmSourceOverwrite;
    if (bConfirm && !ConfirmOverwrite(Confirmation))
    {
        return EMHSourceOverwriteExecution::Cancelled;
    }
    if (Operation() && !bConfirm)
    {
        const FText Audit = SuccessAudit.IsEmpty()
            ? FText::Format(
                INVTEXT("{0} overwritten from edited transforms"),
                FText::FromString(SourceFile))
            : SuccessAudit;
        NotifyOverwrite(Audit);
        LogOverwrite(Audit);
    }
    return EMHSourceOverwriteExecution::Attempted;
}

#if WITH_DEV_AUTOMATION_TESTS
void MHSetSourceOverwritePolicyTestHooks(FMHSourceOverwritePolicyTestHooks Hooks)
{
    GTestHooks = MoveTemp(Hooks);
}
#endif

} // namespace UE::MimirComposite
