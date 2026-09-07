#include "Editing/MHCompositeEditorMode.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "ConvexVolume.h"
#include "UnrealWidget.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editing/MHCompositeEditDocument.h"
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
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "Selection.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "TimerManager.h"
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
    UI_COMMAND(CancelEdit, "Cancel", "Discard the composite draft and leave Edit Contents immediately.", EUserInterfaceActionType::Button, FInputChord(EKeys::Escape));
    UI_COMMAND(SaveEdit, "Save", "Apply the shared definition and leave Edit Contents.", EUserInterfaceActionType::Button, FInputChord());
}

namespace
{

#if WITH_DEV_AUTOMATION_TESTS
TFunction<bool()> GDiscardConfirmForTests;
TFunction<EAppReturnType::Type()> GSwitchConfirmForTests;
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

/** The current scope's crumb: "<edited>[ *]". */
FText Breadcrumb()
{
    const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    const FMHCompositeEditContext Context = Subsystem != nullptr ? Subsystem->GetEditContext() : FMHCompositeEditContext();
    const UMHCompositeEditSession* Session = Subsystem != nullptr ? Subsystem->GetEditSession() : nullptr;
    const TCHAR* Dirty = Session != nullptr && Session->IsDirty() ? TEXT(" *") : TEXT("");
    const FString Preview = Session != nullptr && !Session->GetPreviewError().IsEmpty() ? TEXT(" — preview unavailable") : TEXT("");
    return FText::FromString(Context.EditedLogicalName + Dirty + Preview);
}

/**
 * CE-3d: `Composite Edit | root > child > current*` — every crumb but the
 * last is a button that switches to that definition (deferred: the switch
 * closes this toolkit). The chain is fixed for a session's lifetime: a
 * switch re-enters the mode and rebuilds the overlay.
 */
TSharedRef<SWidget> BuildCrumbs(TWeakObjectPtr<UMHCompositeEditorMode> Mode)
{
    const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    const TArray<TPair<FString, FString>> Targets = UMHCompositeEditorMode::BreadcrumbTargets(Subsystem != nullptr ? Subsystem->GetEditContext() : FMHCompositeEditContext());
    TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);
    Box->AddSlot().AutoWidth().VAlign(VAlign_Center)
    [
        SNew(STextBlock).Text(LOCTEXT("OverlayTitle", "Composite Edit  |  "))
    ];
    for (int32 Index = 0; Index + 1 < Targets.Num(); ++Index)
    {
        const FString Path = Targets[Index].Value;
        Box->AddSlot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SButton)
            .ButtonStyle(FAppStyle::Get(), "SimpleButton")
            .ContentPadding(FMargin(2.0f, 0.0f))
            .ToolTipText(LOCTEXT("CrumbTip", "Edit this definition (Save, Discard or stay first when there are changes)."))
            .OnClicked_Lambda([Mode, Path]()
            {
                if (GEditor != nullptr) GEditor->GetTimerManager()->SetTimerForNextTick([Mode, Path]() { if (Mode.IsValid()) Mode->RequestSwitch(Path); });
                return FReply::Handled();
            })
            [
                SNew(STextBlock).Text(FText::FromString(Targets[Index].Key))
            ]
        ];
        Box->AddSlot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(STextBlock).Text(LOCTEXT("CrumbSeparator", " > "))
        ];
    }
    Box->AddSlot().AutoWidth().VAlign(VAlign_Center)
    [
        SNew(STextBlock).Text_Static(&Breadcrumb)
        .ToolTipText_Lambda([]()
        {
            const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
            const UMHCompositeEditSession* Session = Subsystem != nullptr ? Subsystem->GetEditSession() : nullptr;
            return Session != nullptr ? FText::FromString(Session->GetPreviewError()) : FText::GetEmpty();
        })
    ];
    return Box;
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
                    BuildCrumbs(Mode)
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
        // DeactivateMode defers Exit to the manager's tick and a session
        // re-opened in the same tick (CE-3d switch) would resume the pending
        // mode without Enter — no framing, no overlay. DestroyMode exits now;
        // the next session gets a fresh mode object.
        Tools->DestroyMode(EM_MHCompositeEditModeId);
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

void UMHCompositeEditorMode::SetSwitchConfirmForTests(TFunction<EAppReturnType::Type()> Confirm)
{
    GSwitchConfirmForTests = MoveTemp(Confirm);
}
#endif

bool UMHCompositeEditorMode::RequestSwitch(const FString& InvocationPath)
{
    if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()) return false;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    UMHCompositeEditSession* Session = GetSession();
    AMHCompositeActor* Root = Session != nullptr ? Session->GetRootPlacement() : nullptr;
    if (Subsystem == nullptr || Session == nullptr || Root == nullptr) return false;
    if (Session->GetInvocationPath() == InvocationPath) return true;
    // One writable session (spec §5.2, LI EditLevelInstanceInternal): the
    // current one is resolved before the target opens.
    if (Session->IsDirty())
    {
        switch (ConfirmSwitch(InvocationPath))
        {
        case EAppReturnType::Yes:
            // The usual overwrite confirmation; a declined or failed publish keeps the session.
            MHExecuteCommitEditCompositeInteractive();
            if (Subsystem->IsEditingComposite()) return false;
            break;
        case EAppReturnType::No:
        {
            FString CancelError;
            Subsystem->CancelEditComposite(CancelError);
            break;
        }
        default:
            return false;
        }
    }
    else
    {
        FString CancelError;
        Subsystem->CancelEditComposite(CancelError);
    }
    // This object may be recycled by the mode manager from here on: no members.
    FString Error;
    const bool bOpened = InvocationPath.IsEmpty()
        ? Subsystem->BeginEditComposite(Root, Error)
        : Subsystem->BeginEditNestedComposite(Root, InvocationPath, Error);
    if (!bOpened && !Error.IsEmpty()) FMessageLog("Mimir").Error(FText::FromString(Error));
    return bOpened;
}

TArray<TPair<FString, FString>> UMHCompositeEditorMode::BreadcrumbTargets(const FMHCompositeEditContext& Context)
{
    TArray<TPair<FString, FString>> Targets;
    if (Context.EditedLogicalName.IsEmpty()) return Targets;
    // "root:nodes[i]>child:nodes[j]": segment k names the definition that
    // contains node k; the definition it invokes is named by segment k+1, or
    // is the edited one for the last segment.
    TArray<FString> Segments;
    Context.InvocationPath.ParseIntoArray(Segments, TEXT(">"), true);
    auto DefinitionOf = [](const FString& Segment)
    {
        FString Definition, Selector;
        return Segment.Split(TEXT(":"), &Definition, &Selector) ? Definition : Segment;
    };
    const AMHCompositeActor* Root = Context.RootPlacement.Get();
    const UMHCompositeAsset* RootAsset = Root != nullptr ? Root->GetCompositeAsset() : nullptr;
    Targets.Emplace(Segments.Num() > 0 ? DefinitionOf(Segments[0]) : RootAsset != nullptr ? RootAsset->LogicalName : Context.EditedLogicalName, FString());
    FString Path;
    for (int32 Index = 0; Index < Segments.Num(); ++Index)
    {
        Path = Index == 0 ? Segments[0] : Path + TEXT(">") + Segments[Index];
        Targets.Emplace(Index + 1 < Segments.Num() ? DefinitionOf(Segments[Index + 1]) : Context.EditedLogicalName, Path);
    }
    return Targets;
}

EAppReturnType::Type UMHCompositeEditorMode::ConfirmSwitch(const FString& TargetInvocationPath) const
{
#if WITH_DEV_AUTOMATION_TESTS
    if (GSwitchConfirmForTests) return GSwitchConfirmForTests();
#endif
    const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    const FMHCompositeEditContext Context = Subsystem != nullptr ? Subsystem->GetEditContext() : FMHCompositeEditContext();
    FString Target;
    if (const AMHCompositeActor* Root = Context.RootPlacement.Get())
    {
        if (TargetInvocationPath.IsEmpty())
        {
            Target = Root->GetCompositeAsset() != nullptr ? Root->GetCompositeAsset()->LogicalName : FString();
        }
        else if (const UE::MimirComposite::FMHResolvedCompositePlan* Plan = Root->GetResolvedPlan())
        {
            for (const UE::MimirComposite::FMHResolvedCompositeNode& Node : Plan->Nodes)
            {
                if (Node.NodePath == TargetInvocationPath) { Target = Node.Resource; break; }
            }
        }
    }
    return FMessageDialog::Open(EAppMsgType::YesNoCancel,
        FText::Format(LOCTEXT("SwitchPrompt", "Save changes to {0} before editing {1}?"), FText::FromString(Context.EditedLogicalName), FText::FromString(Target)),
        LOCTEXT("SwitchTitle", "Edit Contents"));
}

UMHCompositeEditSession* UMHCompositeEditorMode::GetSession() const
{
    const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    UMHCompositeEditSession* Session = Subsystem != nullptr ? Subsystem->GetEditSession() : nullptr;
    return Session != nullptr && Session->IsOpen() ? Session : nullptr;
}

void UMHCompositeEditorMode::RequestSave()
{
    if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()) return;
    // The subsystem ends the session on success, which deactivates this mode.
    MHExecuteCommitEditCompositeInteractive();
}

bool UMHCompositeEditorMode::RequestCancel()
{
    CancelGesture();
    UMHCompositeEditSession* Session = GetSession();
    if (Session == nullptr)
    {
        DeactivateForSession();
        return true;
    }
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    const bool bCancelled = Subsystem != nullptr && Subsystem->CancelEditComposite(Error);
    if (!Error.IsEmpty()) FMessageLog("Mimir").Error(FText::FromString(Error));
    return bCancelled;
}

void UMHCompositeEditorMode::SelectNodeIds(const TArray<FGuid>& NodeIds, const FGuid& ActiveNodeId)
{
    if (bTracking) return;
    if (UMHCompositeEditSession* Session = GetSession()) Session->SetSelectedNodeIds(NodeIds, ActiveNodeId);
}

bool UMHCompositeEditorMode::SelectComponent(USceneComponent* Component)
{
    const UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    const FGuid Id = Projection != nullptr ? Projection->GetNodeIdForComponent(Component) : FGuid();
    if (!Id.IsValid()) return false;
    SelectNodeIds({Id}, Id);
    return true;
}

void UMHCompositeEditorMode::MirrorSelection()
{
    if (bMirroringSelection || GEditor == nullptr) return;
    UMHCompositeEditSession* Session = GetSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    AActor* Actor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    if (Actor == nullptr) return;
    TGuardValue<bool> Guard(bMirroringSelection, true);
    if (Session->GetSelectedNodeIds().IsEmpty())
    {
        // Selecting only the infrastructure actor lets the default mode draw
        // a widget at world zero even though no authored node can be moved.
        GEditor->SelectNone(false, true, false);
        Projection->UpdateSelection({});
        GEditor->NoteSelectionChange();
        GEditor->RedrawLevelEditingViewports();
        return;
    }
    if (!Actor->IsSelected() || GEditor->GetSelectedActorCount() != 1)
    {
        GEditor->SelectNone(false, true, false);
        GEditor->SelectActor(Actor, true, false, true);
    }
    USelection* Components = GEditor->GetSelectedComponents();
    Components->BeginBatchSelectOperation();
    Components->DeselectAll();
    TSet<USceneComponent*> Selected;
    for (const FGuid& Id : Session->GetSelectedNodeIds())
    {
        for (USceneComponent* Component : Projection->GetComponentsForNodeId(Id, true))
        {
            if (IsValid(Component) && !Selected.Contains(Component))
            {
                Selected.Add(Component);
                GEditor->SelectComponent(Component, true, false, true);
            }
        }
    }
    Projection->UpdateSelection(Session->GetSelectedNodeIds());
    Components->EndBatchSelectOperation(true);
    GEditor->NoteSelectionChange();
    GEditor->RedrawLevelEditingViewports();
}

void UMHCompositeEditorMode::ActorSelectionChangeNotify()
{
    if (bMirroringSelection) return;
    UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    const AActor* Actor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    if (Actor != nullptr && !Actor->IsSelected()) Session->SetSelectedNodeIds({});
}

void UMHCompositeEditorMode::SelectNone()
{
    if (!bMirroringSelection) SelectNodeIds({});
}

void UMHCompositeEditorMode::PostUndo()
{
    // Object PostEditUndo rebuilds the projection before the engine finishes
    // restoring its own selection snapshot. Reconcile that snapshot last.
    Super::PostUndo();
    MirrorSelection();
}

bool UMHCompositeEditorMode::HandleHitProxy(HHitProxy* HitProxy)
{
    if (HitProxy == nullptr) return false;
    if (HitProxy->IsA(HActor::StaticGetType()))
    {
        const HActor* Hit = static_cast<const HActor*>(HitProxy);
        SelectComponent(const_cast<UPrimitiveComponent*>(Hit->PrimComponent.Get()));
        return true; // Other actors are locked context.
    }
    return HitProxy->IsA(HInstancedStaticMeshInstance::StaticGetType());
}

bool UMHCompositeEditorMode::HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy, const FViewportClick& Click)
{
    if (Click.IsAltDown()) return false; // Orbit/navigation belongs to the viewport.
    if (Click.GetKey() != EKeys::LeftMouseButton && Click.GetKey() != EKeys::RightMouseButton) return false;
    UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    if (Projection == nullptr) return false;
    if (HitProxy == nullptr)
    {
        if (Click.GetKey() == EKeys::LeftMouseButton && !Click.IsControlDown() && !Click.IsShiftDown()) SelectNodeIds({});
        return false;
    }
    if (HitProxy->IsA(HActor::StaticGetType()))
    {
        const HActor* Hit = static_cast<const HActor*>(HitProxy);
        const FGuid Id = Projection->GetNodeIdForComponent(Hit->PrimComponent.Get());
        if (Id.IsValid())
        {
            TArray<FGuid> Ids = Session->GetSelectedNodeIds();
            if (Click.GetKey() == EKeys::RightMouseButton)
            {
                if (!Ids.Contains(Id)) SelectNodeIds({Id}, Id);
                return true;
            }
            if (Click.IsControlDown())
            {
                if (Ids.Contains(Id)) Ids.Remove(Id); else Ids.Add(Id);
            }
            else if (Click.IsShiftDown()) Ids.AddUnique(Id);
            else Ids = {Id};
            SelectNodeIds(Ids, Ids.Contains(Id) ? Id : FGuid());
        }
        return true;
    }
    return HitProxy->IsA(HInstancedStaticMeshInstance::StaticGetType());
}

bool UMHCompositeEditorMode::StartTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport)
{
    // Camera and marquee drags must never open an authoring transaction.
    if (InViewportClient != nullptr && InViewportClient->GetCurrentWidgetAxis() == EAxisList::None) return false;
    if (bTracking || bCancelledTracking || bNavigationTracking) return true;
    if (InViewportClient != nullptr && InViewport != nullptr)
    {
        if (!InViewport->KeyState(EKeys::LeftMouseButton) || InViewport->KeyState(EKeys::RightMouseButton) || InViewport->KeyState(EKeys::MiddleMouseButton))
        {
            // A hovered axis must not turn RMB navigation into an object drag.
            InViewportClient->SetCurrentWidgetAxis(EAxisList::None);
            bNavigationTracking = true;
            return true;
        }
        if (InViewportClient->IsAltPressed())
        {
            // Alt-drag duplication needs a draft command; never duplicate or
            // silently move the transient projection through native fallback.
            bCancelledTracking = true;
            return true;
        }
    }
    UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    if (GEditor == nullptr || Projection == nullptr || !Projection->GetProjectionActor()->IsSelected()) return false;
    if (!Session->GetPreviewError().IsEmpty()) return true;
    GestureNodes.Reset();
    const UMHCompositeEditDocument* Draft = Session->GetDraft();
    const TArray<FGuid>& Ids = Session->GetSelectedNodeIds();
    for (const FGuid& Id : Ids)
    {
        bool bCoveredByParent = false;
        for (FGuid Parent = Draft->GetParentId(Id); Parent.IsValid(); Parent = Draft->GetParentId(Parent))
        {
            if (Ids.Contains(Parent)) { bCoveredByParent = true; break; }
        }
        if (bCoveredByParent) continue;
        FMHCompositeEditNodeFrame Frame;
        if (!Projection->GetNodeFrame(Id, Frame) || Frame.bGeneratedTransform || !UE::MimirComposite::MHIsRepresentableTransformMatrix(Frame.WorldMatrix))
        {
            GestureNodes.Reset();
            return true; // Cannot route unsupported targets to default component editing.
        }
        FGestureNode& Node = GestureNodes.AddDefaulted_GetRef();
        Node.NodeId = Id;
        Node.AuthoredLocal = Frame.AuthoredLocal;
        Node.ParentWorld = Frame.ParentWorldMatrix;
        Node.World = FTransform(Frame.WorldMatrix);
    }
    if (GestureNodes.IsEmpty()) return false;
    GesturePivot = GetWidgetLocation();
    bTracking = true;
    bGestureChanged = false;
    GestureTransaction = GEditor->BeginTransaction(LOCTEXT("MoveNodeTransaction", "Transform Composite Nodes"));
    return true;
}

bool UMHCompositeEditorMode::InputDelta(FEditorViewportClient* InViewportClient, FViewport* InViewport, FVector& InDrag, FRotator& InRot, FVector& InScale)
{
    if (bCancelledTracking) return true;
    if (bNavigationTracking) return false;
    if (!bTracking) return InViewportClient != nullptr && InViewportClient->GetCurrentWidgetAxis() != EAxisList::None;
    UMHCompositeEditSession* Session = GetSession();
    if (Session == nullptr || (InDrag.IsNearlyZero() && InRot.IsNearlyZero() && InScale.IsNearlyZero())) return true;
    TArray<FGuid> Ids;
    TArray<FTransform> Locals, Worlds, PreviousLocals;
    for (const FGestureNode& Node : GestureNodes)
    {
        FTransform World = Node.World;
        FVector Position = World.GetLocation();
        if (!InRot.IsNearlyZero())
        {
            const FQuat Rotation = InRot.Quaternion();
            Position = GesturePivot + Rotation.RotateVector(Position - GesturePivot);
            World.SetRotation((Rotation * World.GetRotation()).GetNormalized());
        }
        if (!InScale.IsNearlyZero())
        {
            const FVector OldScale = World.GetScale3D();
            const FVector NewScale = AActor::bUsePercentageBasedScaling ? OldScale * (FVector::OneVector + InScale) : OldScale + InScale;
            Position = GesturePivot + World.GetRotation().RotateVector(World.GetRotation().UnrotateVector(Position - GesturePivot) * (NewScale / OldScale));
            World.SetScale3D(NewScale);
        }
        World.SetLocation(Position + InDrag);
        // Test the full matrix BEFORE decomposition: silently discarding shear changes the authored pose.
        const FMatrix LocalMatrix = World.ToMatrixWithScale() * Node.ParentWorld.Inverse();
        if (!UE::MimirComposite::MHIsRepresentableTransformMatrix(LocalMatrix)) return true;
        Ids.Add(Node.NodeId);
        const int32 DraftIndex = Session->GetDraft()->FindNodeIndex(Node.NodeId);
        if (DraftIndex == INDEX_NONE) return true;
        PreviousLocals.Add(Session->GetDraft()->GetNodes()[DraftIndex].Transform);
        Locals.Emplace(LocalMatrix);
        Worlds.Add(World);
    }
    FString Error;
    if (Session->SetNodeTransforms(Ids, Locals, Error))
    {
        if (!Session->GetPreviewError().IsEmpty())
        {
            // A parent edit may make a descendant unrepresentable. Restore
            // this delta as a batch before accepting any gesture state.
            const FString PreviewFailure = Session->GetPreviewError();
            Session->SetNodeTransforms(Ids, PreviousLocals, Error);
            FMessageLog("Mimir").Warning(FText::FromString(PreviewFailure));
            return true;
        }
        for (int32 Index = 0; Index < GestureNodes.Num(); ++Index) GestureNodes[Index].World = Worlds[Index];
        GesturePivot += InDrag;
        bGestureChanged = true;
    }
    else if (!Error.IsEmpty()) FMessageLog("Mimir").Warning(FText::FromString(Error));
    return true;
}

bool UMHCompositeEditorMode::EndTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport)
{
    if (bNavigationTracking) { bNavigationTracking = false; return true; }
    if (bCancelledTracking) { bCancelledTracking = false; return true; }
    if (!bTracking) return false;
    bTracking = false;
    if (GestureTransaction != INDEX_NONE && GEditor != nullptr)
    {
        bool bDifferent = false;
        if (const UMHCompositeEditSession* Session = GetSession())
        {
            const UMHCompositeEditDocument* Draft = Session->GetDraft();
            for (const FGestureNode& Node : GestureNodes)
            {
                const int32 Index = Draft->FindNodeIndex(Node.NodeId);
                bDifferent |= Index != INDEX_NONE && !Draft->GetNodes()[Index].Transform.Equals(Node.AuthoredLocal, 0.0);
            }
        }
        if (bGestureChanged && bDifferent) GEditor->EndTransaction();
        else GEditor->CancelTransaction(GestureTransaction);
    }
    GestureTransaction = INDEX_NONE;
    GestureNodes.Reset();
    bGestureChanged = false;
    return true;
}

void UMHCompositeEditorMode::CancelGesture()
{
    if (!bTracking) return;
    if (UMHCompositeEditSession* Session = GetSession())
    {
        TArray<FGuid> Ids;
        TArray<FTransform> Locals;
        for (const FGestureNode& Node : GestureNodes) { Ids.Add(Node.NodeId); Locals.Add(Node.AuthoredLocal); }
        FString Error;
        if (!Session->SetNodeTransforms(Ids, Locals, Error)) FMessageLog("Mimir").Error(FText::FromString(Error));
    }
    // CancelTransaction removes the record; it does not restore object state.
    if (GestureTransaction != INDEX_NONE && GEditor != nullptr) GEditor->CancelTransaction(GestureTransaction);
    GestureTransaction = INDEX_NONE;
    GestureNodes.Reset();
    bTracking = false;
    bCancelledTracking = true;
    bGestureChanged = false;
}

bool UMHCompositeEditorMode::HandleEscape()
{
    return RequestCancel();
}

bool UMHCompositeEditorMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event)
{
    if (Key != EKeys::Escape || Event != IE_Pressed) return false;
    // Stop authoring now; defer mode teardown until UE finishes dispatching
    // this key through its active mode collection.
    CancelGesture();
    if (GEditor != nullptr)
    {
        TWeakObjectPtr<UMHCompositeEditorMode> WeakThis(this);
        const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
        const uint32 Epoch = Subsystem != nullptr ? Subsystem->GetEditSessionEpoch() : 0;
        GEditor->GetTimerManager()->SetTimerForNextTick([WeakThis, Epoch]()
        {
            const UMHCompositeLevelSubsystem* Current = LevelSubsystem();
            if (WeakThis.IsValid() && GetActive() == WeakThis.Get() && Current != nullptr && Current->GetEditSessionEpoch() == Epoch)
                WeakThis->RequestCancel();
        });
    }
    return true;
}

bool UMHCompositeEditorMode::UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const
{
    return CheckMode == UE::Widget::WM_Translate || CheckMode == UE::Widget::WM_Rotate || CheckMode == UE::Widget::WM_Scale;
}

bool UMHCompositeEditorMode::ShouldDrawWidget() const
{
    const UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    if (Projection == nullptr || !Session->GetPreviewError().IsEmpty() || Session->GetSelectedNodeIds().IsEmpty()) return false;
    for (const FGuid& Id : Session->GetSelectedNodeIds())
    {
        bool bCoveredByParent = false;
        for (FGuid Parent = Session->GetDraft()->GetParentId(Id); Parent.IsValid(); Parent = Session->GetDraft()->GetParentId(Parent))
        {
            if (Session->GetSelectedNodeIds().Contains(Parent)) { bCoveredByParent = true; break; }
        }
        if (bCoveredByParent) continue;
        FMHCompositeEditNodeFrame Frame;
        if (!Projection->GetNodeFrame(Id, Frame) || Frame.bGeneratedTransform) return false;
    }
    return true;
}

FVector UMHCompositeEditorMode::GetWidgetLocation() const
{
    if (bTracking) return GesturePivot;
    const UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    FMHCompositeEditNodeFrame Frame;
    return Projection != nullptr && Projection->GetNodeFrame(Session->GetActiveNodeId(), Frame) ? Frame.WorldMatrix.GetOrigin() : FVector::ZeroVector;
}

bool UMHCompositeEditorMode::GetCustomDrawingCoordinateSystem(FMatrix& InMatrix, void* InData)
{
    const UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    FMHCompositeEditNodeFrame Frame;
    if (Projection == nullptr || !Projection->GetNodeFrame(Session->GetActiveNodeId(), Frame)) return false;
    InMatrix = FQuatRotationMatrix(FTransform(Frame.WorldMatrix).GetRotation());
    return true;
}

bool UMHCompositeEditorMode::GetCustomInputCoordinateSystem(FMatrix& InMatrix, void* InData)
{
    return GetCustomDrawingCoordinateSystem(InMatrix, InData);
}

FVector UMHCompositeEditorMode::GetWidgetNormalFromCurrentAxis(void* InData)
{
    FMatrix Matrix = FMatrix::Identity;
    if (GetModeManager() != nullptr && GetModeManager()->GetCoordSystem() == COORD_Local) GetCustomDrawingCoordinateSystem(Matrix, InData);
    FVector Axis = FVector::ZeroVector;
    if ((CurrentWidgetAxis & EAxisList::X) != 0) Axis += Matrix.GetUnitAxis(EAxis::X);
    if ((CurrentWidgetAxis & EAxisList::Y) != 0) Axis += Matrix.GetUnitAxis(EAxis::Y);
    if ((CurrentWidgetAxis & EAxisList::Z) != 0) Axis += Matrix.GetUnitAxis(EAxis::Z);
    return Axis.GetSafeNormal();
}

FBox UMHCompositeEditorMode::ComputeCustomViewportFocus() const
{
    FBox Bounds(ForceInit);
    const UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    if (Projection != nullptr)
    {
        for (const FGuid& Id : Session->GetSelectedNodeIds())
        {
            FBox NodeBounds(ForceInit);
            if (Projection->GetNodeBounds(Id, NodeBounds)) Bounds += NodeBounds;
        }
    }
    return Bounds;
}

bool UMHCompositeEditorMode::SelectBounds(TFunctionRef<bool(const FBox&)> Intersects, bool bSelect)
{
    UMHCompositeEditSession* Session = GetSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    if (Projection == nullptr) return false;
    TArray<FGuid> Ids = Session->GetSelectedNodeIds();
    TSet<FGuid> Hits;
    // Any visual leaf selects its authoring owner, exactly like a click.
    for (USceneComponent* Component : Projection->GetComponents())
    {
        const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
        const FGuid Id = Projection->GetNodeIdForComponent(Component);
        if (Primitive != nullptr && Id.IsValid() && Intersects(Primitive->Bounds.GetBox())) Hits.Add(Id);
    }
    // Draft order keeps the active node deterministic, independent of component/hash ordering.
    for (int32 Index = 0; Index < Session->GetDraft()->Num(); ++Index)
    {
        const FGuid Id = Session->GetDraft()->GetNodeId(Index);
        if (Hits.Contains(Id)) { if (bSelect) Ids.AddUnique(Id); else Ids.Remove(Id); }
    }
    SelectNodeIds(Ids);
    return true;
}

bool UMHCompositeEditorMode::BoxSelect(FBox& InBox, bool InSelect)
{
    return SelectBounds([&InBox](const FBox& Box) { return InBox.Intersect(Box); }, InSelect);
}

bool UMHCompositeEditorMode::FrustumSelect(const FConvexVolume& InFrustum, FEditorViewportClient* InViewportClient, bool InSelect)
{
    return SelectBounds([&InFrustum](const FBox& Box) { return InFrustum.IntersectBox(Box.GetCenter(), Box.GetExtent()); }, InSelect);
}

void UMHCompositeEditorMode::Enter()
{
    UEdMode::Enter();
    // BPP policy (contract §2 "Undo"): the history restarts at the session
    // boundary — inside, every gesture is one step; nothing before the entry
    // can be undone into the session.
    if (GEditor != nullptr) GEditor->ResetTransaction(LOCTEXT("EnterResetTransaction", "Composite Edit Contents started"));
    UpdateEngineShowFlags(true);
    FEditorDelegates::PreBeginPIE.AddUObject(this, &UMHCompositeEditorMode::OnPreBeginPIE);
    // The actor is infrastructure; node selection owns the gizmo and outlines.
    // Entering changes no viewport camera or focus.
    BoundSession = GetSession();
    if (BoundSession.IsValid())
    {
        BoundSession->OnSelectionChanged.AddUObject(this, &UMHCompositeEditorMode::MirrorSelection);
        BoundSession->OnChanged.AddUObject(this, &UMHCompositeEditorMode::MirrorSelection);
    }
    MirrorSelection();
}

void UMHCompositeEditorMode::Exit()
{
    FEditorDelegates::PreBeginPIE.RemoveAll(this);
    if (BoundSession.IsValid())
    {
        BoundSession->OnSelectionChanged.RemoveAll(this);
        BoundSession->OnChanged.RemoveAll(this);
    }
    BoundSession.Reset();
    UpdateEngineShowFlags(false);
    if (GestureTransaction != INDEX_NONE && GEditor != nullptr) GEditor->CancelTransaction(GestureTransaction);
    GestureTransaction = INDEX_NONE;
    bTracking = false;
    bCancelledTracking = false;
    bNavigationTracking = false;
    // The session's steps cannot outlive it (the draft is gone).
    if (GEditor != nullptr) GEditor->ResetTransaction(LOCTEXT("ExitResetTransaction", "Composite Edit Contents ended"));
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
    CommandList->MapAction(Commands.CancelEdit, FExecuteAction::CreateLambda([this]() { HandleEscape(); }));
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
