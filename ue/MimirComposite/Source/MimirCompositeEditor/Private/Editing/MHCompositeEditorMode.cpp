#include "Editing/MHCompositeEditorMode.h"

#include "Styling/AppStyle.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditorMode)

#define LOCTEXT_NAMESPACE "MHCompositeEditorMode"

// CE-3a red stub: the mode exists but never activates.

const FEditorModeID UMHCompositeEditorMode::EM_MHCompositeEditModeId(TEXT("EditMode.MHComposite"));

FMHCompositeEditCommands::FMHCompositeEditCommands()
    : TCommands<FMHCompositeEditCommands>(TEXT("MHCompositeEdit"), LOCTEXT("MHCompositeEditCommands", "MH Composite Edit"), NAME_None, FAppStyle::GetAppStyleSetName())
{
}

void FMHCompositeEditCommands::RegisterCommands()
{
    UI_COMMAND(CancelEdit, "Cancel", "Discard the composite draft and leave Edit Contents (asks first when there are changes).", EUserInterfaceActionType::Button, FInputChord(EKeys::Escape));
    UI_COMMAND(SaveEdit, "Save", "Apply the shared definition and leave Edit Contents.", EUserInterfaceActionType::Button, FInputChord());
}

UMHCompositeEditorMode::UMHCompositeEditorMode()
{
    Info = FEditorModeInfo(EM_MHCompositeEditModeId, LOCTEXT("ModeName", "MH Composite Edit"), FSlateIcon(), false);
}

void UMHCompositeEditorMode::ActivateForSession() {}
void UMHCompositeEditorMode::DeactivateForSession() {}
bool UMHCompositeEditorMode::IsActive() { return false; }
UMHCompositeEditorMode* UMHCompositeEditorMode::GetActive() { return nullptr; }
void UMHCompositeEditorMode::RegisterCommands() { FMHCompositeEditCommands::Register(); }
void UMHCompositeEditorMode::UnregisterCommands() { FMHCompositeEditCommands::Unregister(); }
void UMHCompositeEditorMode::RequestSave() {}
bool UMHCompositeEditorMode::RequestCancel() { return false; }
void UMHCompositeEditorMode::Enter() { UEdMode::Enter(); }
void UMHCompositeEditorMode::Exit() { UEdMode::Exit(); }
void UMHCompositeEditorMode::CreateToolkit() {}
bool UMHCompositeEditorMode::UsesToolkits() const { return false; }
bool UMHCompositeEditorMode::IsCompatibleWith(const FEditorModeID OtherModeID) const { static_cast<void>(OtherModeID); return true; }
bool UMHCompositeEditorMode::IsSelectionDisallowed(AActor* InActor, const bool bInSelection) const { static_cast<void>(InActor); static_cast<void>(bInSelection); return false; }
bool UMHCompositeEditorMode::IsEditingDisallowed(AActor* InActor) const { static_cast<void>(InActor); return false; }
bool UMHCompositeEditorMode::OnRequestClose() { return true; }
void UMHCompositeEditorMode::ModeTick(const float DeltaTime) { UEdMode::ModeTick(DeltaTime); }
#if WITH_DEV_AUTOMATION_TESTS
void UMHCompositeEditorMode::SetDiscardConfirmForTests(TFunction<bool()> Confirm) { static_cast<void>(Confirm); }
#endif
void UMHCompositeEditorMode::BindCommands() { UEdMode::BindCommands(); }
void UMHCompositeEditorMode::OnPreBeginPIE(const bool bSimulate) { static_cast<void>(bSimulate); }
void UMHCompositeEditorMode::UpdateEngineShowFlags(const bool bEditing) { static_cast<void>(bEditing); }
UMHCompositeEditSession* UMHCompositeEditorMode::GetSession() const { return nullptr; }
bool UMHCompositeEditorMode::ConfirmDiscard() const { return false; }

#undef LOCTEXT_NAMESPACE
