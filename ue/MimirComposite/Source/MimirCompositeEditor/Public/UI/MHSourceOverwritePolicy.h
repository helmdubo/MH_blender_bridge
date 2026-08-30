#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

enum class EMHSourceOverwriteExecution : uint8
{
    Cancelled,
    Attempted
};

/**
 * Runs an editor source overwrite behind the configured confirmation policy.
 * Attempted means that Operation ran; its bool result controls success audit.
 */
MIMIRCOMPOSITEEDITOR_API EMHSourceOverwriteExecution MHExecuteSourceOverwrite(
    const FString& SourceFile,
    const FText& Confirmation,
    const FText& SuccessAudit,
    TFunctionRef<bool()> Operation);

#if WITH_DEV_AUTOMATION_TESTS
struct FMHSourceOverwritePolicyTestHooks
{
    TFunction<bool(const FText&)> Confirm;
    TFunction<void(const FText&)> Notify;
    TFunction<void(const FText&)> MessageLog;
};

MIMIRCOMPOSITEEDITOR_API void MHSetSourceOverwritePolicyTestHooks(
    FMHSourceOverwritePolicyTestHooks Hooks);
#endif

} // namespace UE::MimirComposite
