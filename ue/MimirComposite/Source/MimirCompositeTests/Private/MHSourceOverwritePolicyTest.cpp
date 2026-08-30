#include "Settings/MHCompositeSettings.h"
#include "UI/MHSourceOverwritePolicy.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FOverwritePolicyTestScope
{
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    bool bPreviousConfirm = Settings->bConfirmSourceOverwrite;

    ~FOverwritePolicyTestScope()
    {
        Settings->bConfirmSourceOverwrite = bPreviousConfirm;
        MHSetSourceOverwritePolicyTestHooks({});
    }
};

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceOverwriteConfirmEnabledTest,
    "Mimir.V4.SourceOverwritePolicy.ConfirmEnabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceOverwriteConfirmEnabledTest::RunTest(const FString& Parameters)
{
    FOverwritePolicyTestScope Scope;
    const UMHCompositeSettings* Defaults = NewObject<UMHCompositeSettings>();
    bool bPassed = TestTrue(
        TEXT("source overwrite confirmation defaults to enabled"),
        Defaults->bConfirmSourceOverwrite);
    Scope.Settings->bConfirmSourceOverwrite = true;

    int32 ConfirmationCalls = 0;
    int32 WriteCalls = 0;
    int32 NotificationCalls = 0;
    int32 MessageLogCalls = 0;
    FMHSourceOverwritePolicyTestHooks Hooks;
    Hooks.Confirm = [&ConfirmationCalls](const FText&)
    {
        ++ConfirmationCalls;
        return false;
    };
    Hooks.Notify = [&NotificationCalls](const FText&) { ++NotificationCalls; };
    Hooks.MessageLog = [&MessageLogCalls](const FText&) { ++MessageLogCalls; };
    MHSetSourceOverwritePolicyTestHooks(MoveTemp(Hooks));

    const EMHSourceOverwriteExecution Cancelled = MHExecuteSourceOverwrite(
        TEXT("vehicles/truck.composite"),
        INVTEXT("Confirm overwrite"),
        INVTEXT("vehicles/truck.composite overwritten from edited transforms"),
        [&WriteCalls]()
        {
            ++WriteCalls;
            return true;
        });
    bPassed &= TestEqual(
        TEXT("declined confirmation cancels overwrite"),
        Cancelled,
        EMHSourceOverwriteExecution::Cancelled);
    bPassed &= TestEqual(TEXT("enabled policy opens confirmation"), ConfirmationCalls, 1);
    bPassed &= TestEqual(TEXT("cancelled overwrite does not write"), WriteCalls, 0);
    bPassed &= TestEqual(TEXT("cancelled overwrite has no success notification"), NotificationCalls, 0);
    bPassed &= TestEqual(TEXT("cancelled overwrite has no success log"), MessageLogCalls, 0);

    FMHSourceOverwritePolicyTestHooks AcceptHooks;
    AcceptHooks.Confirm = [&ConfirmationCalls](const FText&)
    {
        ++ConfirmationCalls;
        return true;
    };
    AcceptHooks.Notify = [&NotificationCalls](const FText&) { ++NotificationCalls; };
    AcceptHooks.MessageLog = [&MessageLogCalls](const FText&) { ++MessageLogCalls; };
    MHSetSourceOverwritePolicyTestHooks(MoveTemp(AcceptHooks));
    const EMHSourceOverwriteExecution Accepted = MHExecuteSourceOverwrite(
        TEXT("vehicles/truck.composite"),
        INVTEXT("Confirm overwrite"),
        INVTEXT("vehicles/truck.composite overwritten from edited transforms"),
        [&WriteCalls]()
        {
            ++WriteCalls;
            return true;
        });
    bPassed &= TestEqual(
        TEXT("accepted confirmation attempts overwrite"),
        Accepted,
        EMHSourceOverwriteExecution::Attempted);
    bPassed &= TestEqual(TEXT("enabled policy asks before every attempt"), ConfirmationCalls, 2);
    bPassed &= TestEqual(TEXT("accepted confirmation runs one write"), WriteCalls, 1);
    bPassed &= TestEqual(TEXT("enabled policy keeps existing notification path"), NotificationCalls, 0);
    bPassed &= TestEqual(TEXT("enabled policy keeps existing Message Log path"), MessageLogCalls, 0);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceOverwriteConfirmDisabledTest,
    "Mimir.V4.SourceOverwritePolicy.ConfirmDisabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceOverwriteConfirmDisabledTest::RunTest(const FString& Parameters)
{
    FOverwritePolicyTestScope Scope;
    Scope.Settings->bConfirmSourceOverwrite = false;
    int32 ConfirmationCalls = 0;
    int32 WriteCalls = 0;
    TArray<FString> Notifications;
    TArray<FString> MessageLogLines;
    FMHSourceOverwritePolicyTestHooks Hooks;
    Hooks.Confirm = [&ConfirmationCalls](const FText&)
    {
        ++ConfirmationCalls;
        return false;
    };
    Hooks.Notify = [&Notifications](const FText& Text)
    {
        Notifications.Add(Text.ToString());
    };
    Hooks.MessageLog = [&MessageLogLines](const FText& Text)
    {
        MessageLogLines.Add(Text.ToString());
    };
    MHSetSourceOverwritePolicyTestHooks(MoveTemp(Hooks));

    const FString Target = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".composite"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*Target, false, true, true);
    };
    const FString Audit = Target + TEXT(" overwritten from edited transforms");
    const EMHSourceOverwriteExecution Executed = MHExecuteSourceOverwrite(
        Target,
        INVTEXT("This confirmation must be suppressed"),
        FText::FromString(Audit),
        [&Target, &WriteCalls]()
        {
            ++WriteCalls;
            return FFileHelper::SaveStringToFile(TEXT("edited\n"), *Target);
        });

    FString Contents;
    bool bPassed = TestEqual(
        TEXT("disabled policy attempts overwrite without modal"),
        Executed,
        EMHSourceOverwriteExecution::Attempted);
    bPassed &= TestEqual(TEXT("disabled policy never invokes confirmation UI"), ConfirmationCalls, 0);
    bPassed &= TestEqual(TEXT("disabled policy runs one write"), WriteCalls, 1);
    bPassed &= TestTrue(TEXT("disabled policy writes the source file"), FFileHelper::LoadFileToString(Contents, *Target));
    bPassed &= TestEqual(TEXT("disabled policy writes expected contents"), Contents, FString(TEXT("edited\n")));
    bPassed &= TestEqual(TEXT("disabled policy emits one non-modal notification"), Notifications.Num(), 1);
    bPassed &= TestEqual(TEXT("disabled policy emits one Message Log line"), MessageLogLines.Num(), 1);
    if (Notifications.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("notification text identifies overwritten file"), Notifications[0], Audit);
    }
    if (MessageLogLines.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("Message Log text identifies overwritten file"), MessageLogLines[0], Audit);
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
