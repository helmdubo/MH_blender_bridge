#include "UI/MHSourceOverwritePolicy.h"

#include "Misc/MessageDialog.h"

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

} // namespace

EMHSourceOverwriteExecution MHExecuteSourceOverwrite(
    const FString& SourceFile,
    const FText& Confirmation,
    const FText& SuccessAudit,
    TFunctionRef<bool()> Operation)
{
    static_cast<void>(SourceFile);
    static_cast<void>(SuccessAudit);
    if (!ConfirmOverwrite(Confirmation))
    {
        return EMHSourceOverwriteExecution::Cancelled;
    }
    Operation();
    return EMHSourceOverwriteExecution::Attempted;
}

#if WITH_DEV_AUTOMATION_TESTS
void MHSetSourceOverwritePolicyTestHooks(FMHSourceOverwritePolicyTestHooks Hooks)
{
    GTestHooks = MoveTemp(Hooks);
}
#endif

} // namespace UE::MimirComposite
