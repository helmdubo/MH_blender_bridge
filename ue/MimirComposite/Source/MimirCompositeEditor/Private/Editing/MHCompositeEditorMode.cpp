#include "Editing/MHCompositeEditorMode.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "LevelEditorActions.h"
#include "LevelEditorViewport.h"
#include "Misc/MessageDialog.h"
#include "Selection.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "Toolkits/BaseToolkit.h"
#include "Toolkits/IToolkitHost.h"
#include "UI/MHSourceToolMenus.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditorMode)

#define LOCTEXT_NAMESPACE "MHCompositeEditorMode"

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

namespace
{

#if WITH_DEV_AUTOMATION_TESTS
TFunction<bool()> GDiscardConfirmForTests;
#endif
/** Set while the subsystem itself ends the session: Exit must not cancel it a second time. */
bool GDeactivatingForSession = false;

UMHCompositeLevelSubsystem* LevelSubsystem()
{
    return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
}

FEditorModeTools* LevelModeTools()
{
    return GEditor != nullptr ? &GLevelEditorModeTools() : nullptr;
}

/** "root > child > grandchild" from the session's invocation chain. */
FText Breadcrumb()
{
    const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    const FMHCompositeEditContext Context = Subsystem != nullptr ? Subsystem->GetEditContext() : FMHCompositeEditContext();
    TArray<FString> Trail;
    TArray<FString> Segments;
    Context.InvocationPath.ParseIntoArray(Segments, TEXT(">"), true);
    for (const FString& Segment : Segments)
    {
        FString Definition, Selector;
        Trail.Add(Segment.Split(TEXT(":"), &Definition, &Selector) ? Definition : Segment);
    }
    if (!Context.EditedLogicalName.IsEmpty()) Trail.Add(Context.EditedLogicalName);
    const UMHCompositeEditSession* Session = Subsystem != nullptr ? Subsystem->GetEditSession() : nullptr;
    const TCHAR* Dirty = Session != nullptr && Session->IsDirty() ? TEXT(" *") : TEXT("");
    return FText::FromString(FString::Printf(TEXT("Composite Edit  |  %s%s"), *FString::Join(Trail, TEXT(" > ")), Dirty));
}

/** The viewport overlay: `<icon> <breadcrumb> [Save] [Cancel]`, the Level Instance Edit shape. */
class FMHCompositeEditorModeToolkit final : public FModeToolkit
{
public:
    virtual ~FMHCompositeEditorModeToolkit() override
    {
        if (IsHosted() && Overlay.IsValid()) GetToolkitHost()->RemoveViewportOverlayWidget(Overlay.ToSharedRef());
    }

    virtual void Init(const TSharedPtr<IToolkitHost>& InitToolkitHost, TWeakObjectPtr<UEdMode> InOwningMode) override
    {
        FModeToolkit::Init(InitToolkitHost, InOwningMode);
        TWeakObjectPtr<UMHCompositeEditorMode> Mode = Cast<UMHCompositeEditorMode>(InOwningMode.Get());
        SAssignNew(Overlay, SHorizontalBox)
        + SHorizontalBox::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(FMargin(0.0f, 0.0f, 0.0f, 15.0f))
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("EditorViewport.OverlayBrush"))
            .Padding(8.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
                [
                    SNew(SImage).Image(FSlateIconFinder::FindIconBrushForClass(AMHCompositeActor::StaticClass()))
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
                [
                    SNew(STextBlock).Text_Static(&Breadcrumb)
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
                [
                    SNew(SButton)
                    .ButtonStyle(FAppStyle::Get(), "PrimaryButton")
                    .TextStyle(FAppStyle::Get(), "DialogButtonText")
                    .Text(LOCTEXT("SaveButton", "Save"))
                    .ToolTipText(LOCTEXT("SaveButtonTip", "Apply the shared definition to its .composite source and leave Edit Contents."))
                    .HAlign(HAlign_Center)
                    .OnClicked_Lambda([Mode]() { if (Mode.IsValid()) Mode->RequestSave(); return FReply::Handled(); })
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4.0f, 0.0f, 4.0f, 0.0f))
                [
                    SNew(SButton)
                    .TextStyle(FAppStyle::Get(), "DialogButtonText")
                    .Text(LOCTEXT("CancelButton", "Cancel"))
                    .ToolTipText(LOCTEXT("CancelButtonTip", "Discard the composite draft and leave Edit Contents (Esc)."))
                    .HAlign(HAlign_Center)
                    .OnClicked_Lambda([Mode]() { if (Mode.IsValid()) Mode->RequestCancel(); return FReply::Handled(); })
                ]
            ]
        ];
        GetToolkitHost()->AddViewportOverlayWidget(Overlay.ToSharedRef());
    }

    virtual FName GetToolkitFName() const override { return FName("MHCompositeEditorModeToolkit"); }
    virtual FText GetBaseToolkitName() const override { return LOCTEXT("ToolkitName", "MH Composite Edit"); }

private:
    TSharedPtr<SWidget> Overlay;
};

} // namespace

UMHCompositeEditorMode::UMHCompositeEditorMode()
{
    // Invisible in the modes toolbar: the subsystem drives activation.
    Info = FEditorModeInfo(EM_MHCompositeEditModeId, LOCTEXT("ModeName", "MH Composite Edit"), FSlateIcon(), false);
}

void UMHCompositeEditorMode::ActivateForSession()
{
    if (FEditorModeTools* Tools = LevelModeTools())
    {
        if (!Tools->IsModeActive(EM_MHCompositeEditModeId)) Tools->ActivateMode(EM_MHCompositeEditModeId);
    }
}

void UMHCompositeEditorMode::DeactivateForSession()
{
    if (FEditorModeTools* Tools = LevelModeTools())
    {
        if (!Tools->IsModeActive(EM_MHCompositeEditModeId)) return;
        TGuardValue<bool> Guard(GDeactivatingForSession, true);
        Tools->DeactivateMode(EM_MHCompositeEditModeId);
    }
}

bool UMHCompositeEditorMode::IsActive()
{
    const FEditorModeTools* Tools = LevelModeTools();
    return Tools != nullptr && Tools->IsModeActive(EM_MHCompositeEditModeId);
}

UMHCompositeEditorMode* UMHCompositeEditorMode::GetActive()
{
    FEditorModeTools* Tools = LevelModeTools();
    return Tools != nullptr ? Cast<UMHCompositeEditorMode>(Tools->GetActiveScriptableMode(EM_MHCompositeEditModeId)) : nullptr;
}

void UMHCompositeEditorMode::RegisterCommands()
{
    FMHCompositeEditCommands::Register();
}

void UMHCompositeEditorMode::UnregisterCommands()
{
    FMHCompositeEditCommands::Unregister();
}

#if WITH_DEV_AUTOMATION_TESTS
void UMHCompositeEditorMode::SetDiscardConfirmForTests(TFunction<bool()> Confirm)
{
    GDiscardConfirmForTests = MoveTemp(Confirm);
}
#endif

UMHCompositeEditSession* UMHCompositeEditorMode::GetSession() const
{
    const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    UMHCompositeEditSession* Session = Subsystem != nullptr ? Subsystem->GetEditSession() : nullptr;
    return Session != nullptr && Session->IsOpen() ? Session : nullptr;
}

bool UMHCompositeEditorMode::ConfirmDiscard() const
{
#if WITH_DEV_AUTOMATION_TESTS
    if (GDiscardConfirmForTests) return GDiscardConfirmForTests();
#endif
    return FMessageDialog::Open(EAppMsgType::YesNo,
        LOCTEXT("DiscardPrompt", "Discard unsaved composite changes?"),
        LOCTEXT("DiscardTitle", "Cancel Edit Contents")) == EAppReturnType::Yes;
}

void UMHCompositeEditorMode::RequestSave()
{
    if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()) return;
    // The subsystem ends the session on success, which deactivates this mode.
    MHExecuteCommitEditCompositeInteractive();
}

bool UMHCompositeEditorMode::RequestCancel()
{
    if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()) return false;
    UMHCompositeEditSession* Session = GetSession();
    if (Session == nullptr)
    {
        DeactivateForSession();
        return true;
    }
    if (Session->IsDirty() && !ConfirmDiscard()) return false;
    FString Error;
    if (UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem()) Subsystem->CancelEditComposite(Error);
    return true;
}

bool UMHCompositeEditorMode::SelectComponent(USceneComponent* Component)
{
    const UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    AActor* ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    if (GEditor == nullptr || ProjectionActor == nullptr || !IsValid(Component) || Component->GetOwner() != ProjectionActor) return false;
    // The projection actor exclusively, then the node's component: the
    // engine's gizmo follows the selected component.
    if (!ProjectionActor->IsSelected() || GEditor->GetSelectedActorCount() != 1)
    {
        GEditor->SelectNone(false, true, false);
        GEditor->SelectActor(ProjectionActor, true, true, true);
    }
    USelection* Components = GEditor->GetSelectedComponents();
    Components->BeginBatchSelectOperation();
    Components->DeselectAll();
    GEditor->SelectComponent(Component, true, false, true);
    Components->EndBatchSelectOperation(true);
    GEditor->NoteSelectionChange();
    GEditor->RedrawLevelEditingViewports();
    return true;
}

bool UMHCompositeEditorMode::HandleHitProxy(HHitProxy* HitProxy)
{
    if (HitProxy == nullptr) return false;
    if (HitProxy->IsA(HActor::StaticGetType()))
    {
        const HActor* ActorHit = static_cast<const HActor*>(HitProxy);
        const UMHCompositeEditSession* Session = GetSession();
        const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
        AActor* ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
        if (ProjectionActor != nullptr && ActorHit->Actor == ProjectionActor)
        {
            // Projection geometry: the node under the cursor.
            UPrimitiveComponent* Hit = const_cast<UPrimitiveComponent*>(ActorHit->PrimComponent.Get());
            if (!SelectComponent(Hit) && GEditor != nullptr && !ProjectionActor->IsSelected())
            {
                GEditor->SelectNone(false, true, false);
                GEditor->SelectActor(ProjectionActor, true, true, true);
            }
            return true;
        }
        // Locked context: any other actor is not a target while editing.
        return true;
    }
    // Pooled instances (shared ISM buckets) are locked too.
    if (HitProxy->IsA(HInstancedStaticMeshInstance::StaticGetType())) return true;
    // Gizmo axes, brush handles, empty space: not ours.
    return false;
}

bool UMHCompositeEditorMode::HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy, const FViewportClick& Click)
{
    static_cast<void>(InViewportClient);
    static_cast<void>(Click);
    return HandleHitProxy(HitProxy);
}

void UMHCompositeEditorMode::Enter()
{
    UEdMode::Enter();
    UpdateEngineShowFlags(true);
    FEditorDelegates::PreBeginPIE.AddUObject(this, &UMHCompositeEditorMode::OnPreBeginPIE);
}

void UMHCompositeEditorMode::Exit()
{
    FEditorDelegates::PreBeginPIE.RemoveAll(this);
    UpdateEngineShowFlags(false);
    UEdMode::Exit();
    // Left by something other than the session's own end (another mode, a
    // level change): the draft cannot stay open without its mode.
    if (!GDeactivatingForSession)
    {
        if (UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem(); Subsystem != nullptr && Subsystem->IsEditingComposite())
        {
            FString Error;
            Subsystem->CancelEditComposite(Error);
        }
    }
}

bool UMHCompositeEditorMode::UsesToolkits() const
{
    // Headless (automation, commandlets): no host, no overlay, the mode logic stays.
    return GetModeManager() != nullptr && GetModeManager()->HasToolkitHost();
}

void UMHCompositeEditorMode::CreateToolkit()
{
    if (!UsesToolkits()) return;
    Toolkit = MakeShared<FMHCompositeEditorModeToolkit>();
}

bool UMHCompositeEditorMode::IsCompatibleWith(const FEditorModeID OtherModeID) const
{
    return OtherModeID != FBuiltinEditorModes::EM_Foliage && OtherModeID != FBuiltinEditorModes::EM_Landscape;
}

bool UMHCompositeEditorMode::IsSelectionDisallowed(AActor* InActor, const bool bInSelection) const
{
    if (!bInSelection) return false;
    // Locked context: only the session's projection can be selected or edited.
    const UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    const AActor* ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    return ProjectionActor != nullptr && InActor != ProjectionActor;
}

bool UMHCompositeEditorMode::IsEditingDisallowed(AActor* InActor) const
{
    return IsSelectionDisallowed(InActor, true);
}

bool UMHCompositeEditorMode::OnRequestClose()
{
    // Save/Discard/Stay: staying keeps the mode (and the session) alive.
    return RequestCancel();
}

void UMHCompositeEditorMode::ModeTick(const float DeltaTime)
{
    UEdMode::ModeTick(DeltaTime);
    // Viewports created after Enter (new windows) pick up the dimming too.
    UpdateEngineShowFlags(true);
    // CE-3b: a re-created proxy (mesh, material, visibility) is tinted again.
    if (UMHCompositeEditSession* Session = GetSession())
    {
        if (UMHCompositeEditProjection* Projection = Session->GetProjection()) Projection->PushEditingTint();
    }
}

void UMHCompositeEditorMode::BindCommands()
{
    UEdMode::BindCommands();
    if (!Toolkit.IsValid()) return;
    const TSharedRef<FUICommandList>& CommandList = Toolkit->GetToolkitCommands();
    const FMHCompositeEditCommands& Commands = FMHCompositeEditCommands::Get();
    CommandList->MapAction(
        Commands.CancelEdit,
        FExecuteAction::CreateLambda([this]() { RequestCancel(); }),
        FCanExecuteAction::CreateLambda([]()
        {
            // Escape first clears a selection (the engine's SelectNone chord), only then leaves.
            if (GEditor != nullptr && GEditor->GetSelectedActors()->Num() > 0)
            {
                const FMHCompositeEditCommands& Own = FMHCompositeEditCommands::Get();
                for (const EMultipleKeyBindingIndex Index : {EMultipleKeyBindingIndex::Primary, EMultipleKeyBindingIndex::Secondary})
                {
                    const FInputChord& SelectNone = FLevelEditorCommands::Get().SelectNone->GetActiveChord(Index).Get();
                    if (SelectNone.IsValidChord() && Own.CancelEdit->HasActiveChord(SelectNone)) return false;
                }
            }
            return true;
        }));
    CommandList->MapAction(Commands.SaveEdit, FExecuteAction::CreateLambda([this]() { RequestSave(); }));
}

void UMHCompositeEditorMode::OnPreBeginPIE(const bool bSimulate)
{
    static_cast<void>(bSimulate);
    // PIE never runs a draft: the user saves or discards first (or stays and
    // keeps the editor-only projection, which PIE does not duplicate).
    RequestCancel();
}

void UMHCompositeEditorMode::UpdateEngineShowFlags(const bool bEditing)
{
    if (GEditor == nullptr) return;
    for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
    {
        if (Client == nullptr) continue;
        // The engine's Level Instance dimming: everything but proxies marked
        // as editing (the projection's) renders desaturated.
        Client->EngineShowFlags.EditingLevelInstance = bEditing;
        Client->LastEngineShowFlags.EditingLevelInstance = bEditing;
    }
}

#undef LOCTEXT_NAMESPACE
