#include "UI/MHCompositeOutliner.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeSelectionAdapter.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor.h"
#include "Logging/MessageLog.h"
#include "Editor/EditorEngine.h"
#include "EditorModeManager.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Elements/SMInstance/SMInstanceElementData.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "LevelEditor.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Selection.h"
#include "Settings/MHCompositeSettings.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UI/MHCompositeOutlinerModel.h"
#include "UI/MHEditSessionKeys.h"
#include "UI/MHCompositeOutlinerEditActions.h"
#include "UI/MHSourceToolMenus.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "ScopedTransaction.h"
#include "TimerManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STreeView.h"

#define LOCTEXT_NAMESPACE "MHCompositeOutliner"

namespace UE::MimirComposite
{

namespace
{

FString OutlinerKindText(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh: return TEXT("mesh");
    case EMHRandomSemanticKind::Actor: return TEXT("actor");
    case EMHRandomSemanticKind::Composite: return TEXT("composite");
    case EMHRandomSemanticKind::Group: return TEXT("group");
    case EMHRandomSemanticKind::Random: return TEXT("random");
    case EMHRandomSemanticKind::Empty: return TEXT("empty");
    case EMHRandomSemanticKind::GameObj: return TEXT("gameobj");
    }
    return TEXT("unknown");
}

const FSlateBrush* OutlinerKindIcon(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh: return FAppStyle::GetBrush(TEXT("ClassIcon.StaticMesh"));
    case EMHRandomSemanticKind::Actor: return FAppStyle::GetBrush(TEXT("ClassIcon.Actor"));
    case EMHRandomSemanticKind::Composite: return FAppStyle::GetBrush(TEXT("ClassIcon.DataAsset"));
    case EMHRandomSemanticKind::Group: return FAppStyle::GetBrush(TEXT("Icons.FolderClosed"));
    case EMHRandomSemanticKind::Random: return FAppStyle::GetBrush(TEXT("Icons.Refresh"));
    case EMHRandomSemanticKind::GameObj: return FAppStyle::GetBrush(TEXT("ClassIcon.SceneComponent"));
    case EMHRandomSemanticKind::Empty: return FAppStyle::GetBrush(TEXT("Icons.Circle"));
    }
    return FAppStyle::GetBrush(TEXT("Icons.Help"));
}

FString FormatRandomTrs(const FMHRandomTrs& Trs)
{
    return FString::Printf(
        TEXT("T=(%.4g, %.4g, %.4g) cm  R=(%.5g, %.5g, %.5g, %.5g)  S=(%.4g, %.4g, %.4g)"),
        static_cast<double>(Trs.TranslationCm.X),
        static_cast<double>(Trs.TranslationCm.Y),
        static_cast<double>(Trs.TranslationCm.Z),
        static_cast<double>(Trs.RotationQuat.X),
        static_cast<double>(Trs.RotationQuat.Y),
        static_cast<double>(Trs.RotationQuat.Z),
        static_cast<double>(Trs.RotationQuat.W),
        static_cast<double>(Trs.Scale.X),
        static_cast<double>(Trs.Scale.Y),
        static_cast<double>(Trs.Scale.Z));
}

FText OutlinerTooltip(const FMHCompositeOutlinerItem& Item)
{
    FString Text = Item.NodePath;
    Text += TEXT("\nFixed: ") + Item.FixedTransform.ToHumanReadableString();
    if (Item.SampledLocalTrs.IsSet())
        Text += TEXT("\nSampled: ") + FormatRandomTrs(Item.SampledLocalTrs.GetValue());
    if (Item.bMissingEndpoint) Text += TEXT("\nMissing materialized endpoint");
    return FText::FromString(Text);
}

class SMHCompositeOutliner final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMHCompositeOutliner) {}
    SLATE_END_ARGS()

    // R6-UX2a: Esc/Enter inside the panel act on the session like in the viewport.
    virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent& KeyEvent) override
    {
        return !KeyEvent.IsRepeat() && MHHandleEditSessionKey(KeyEvent.GetKey()) ? FReply::Handled() : FReply::Unhandled();
    }

    void Construct(const FArguments&)
    {
        ChildSlot
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
                .Padding(FMargin(8.0f, 5.0f))
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SAssignNew(HeaderText, STextBlock)
                        .Text(LOCTEXT("NoCompositeSelected", "Select one MH Composite actor"))
                        .Font(FAppStyle::GetFontStyle(TEXT("DetailsView.CategoryFontStyle")))
                    ]
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SAssignNew(StatusText, STextBlock)
                        .Text(LOCTEXT("NoOverlay", "Open a composite edit session to work with its nodes"))
                        .AutoWrapText(true)
                    ]
                ]
            ]
            + SVerticalBox::Slot().FillHeight(1.0f)
            [
                SNew(SSplitter)
                .Orientation(Orient_Vertical)
                + SSplitter::Slot().Value(0.64f)
                [
                    SNew(SBorder)
                    .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
                    .Padding(2.0f)
                    [
                        SAssignNew(TreeView, STreeView<TSharedPtr<FMHCompositeOutlinerItem>>)
                        .TreeItemsSource(&RootItems)
                        .SelectionMode_Lambda([]()
                        {
                            return UMHCompositeEditorMode::IsActive()
                                ? ESelectionMode::Multi
                                : ESelectionMode::Single;
                        })
                        .OnGenerateRow(this, &SMHCompositeOutliner::GenerateRow)
                        .OnGetChildren(this, &SMHCompositeOutliner::GetTreeChildren)
                        .OnSelectionChanged(this, &SMHCompositeOutliner::TreeSelectionChanged)
                        .OnContextMenuOpening(this, &SMHCompositeOutliner::OpenContextMenu)
                    ]
                ]
                + SSplitter::Slot().Value(0.36f)
                [
                    SNew(SBorder)
                    .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
                    .Padding(5.0f)
                    [
                        SNew(SScrollBox)
                        + SScrollBox::Slot()
                        [
                            SAssignNew(DetailsBox, SVerticalBox)
                        ]
                    ]
                ]
            ]
        ];

        FLevelEditorModule& LevelEditor =
            FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
        ActorSelectionHandle = LevelEditor.OnActorSelectionChanged().AddSP(
            SharedThis(this), &SMHCompositeOutliner::ActorSelectionChanged);
        ComponentsEditedHandle = LevelEditor.OnComponentsEdited().AddSP(
            SharedThis(this), &SMHCompositeOutliner::ComponentsEdited);
        SelectionChangedHandle = USelection::SelectionChangedEvent.AddSP(
            SharedThis(this), &SMHCompositeOutliner::EditorSelectionChanged);
        SelectObjectHandle = USelection::SelectObjectEvent.AddSP(
            SharedThis(this), &SMHCompositeOutliner::EditorSelectionChanged);
        if (GEditor != nullptr)
        {
            if (UTypedElementSelectionSet* SelectionSet =
                    GLevelEditorModeTools().GetEditorSelectionSet())
            {
                TypedSelectionChangedHandle = SelectionSet->OnChanged().AddSP(
                    SharedThis(this), &SMHCompositeOutliner::TypedSelectionChanged);
            }
        }
        RefreshSelectedActor();
    }

    virtual ~SMHCompositeOutliner() override
    {
        ObserveSession(nullptr);
        if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
        {
            FLevelEditorModule& LevelEditor =
                FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
            LevelEditor.OnActorSelectionChanged().Remove(ActorSelectionHandle);
            LevelEditor.OnComponentsEdited().Remove(ComponentsEditedHandle);
        }
        USelection::SelectionChangedEvent.Remove(SelectionChangedHandle);
        USelection::SelectObjectEvent.Remove(SelectObjectHandle);
        if (GEditor != nullptr)
        {
            if (UTypedElementSelectionSet* SelectionSet =
                    GLevelEditorModeTools().GetEditorSelectionSet())
            {
                SelectionSet->OnChanged().Remove(TypedSelectionChangedHandle);
            }
        }
    }

private:
    TSharedRef<ITableRow> GenerateRow(
        TSharedPtr<FMHCompositeOutlinerItem> Item,
        const TSharedRef<STableViewBase>& OwnerTable)
    {
        return SNew(STableRow<TSharedPtr<FMHCompositeOutlinerItem>>, OwnerTable)
        .ToolTipText(Item.IsValid() ? OutlinerTooltip(*Item) : FText::GetEmpty())
        .OnCanAcceptDrop(this, &SMHCompositeOutliner::CanAcceptDrop)
        .OnAcceptDrop(this, &SMHCompositeOutliner::AcceptDrop)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 5.0f, 0.0f)
            [
                SNew(SImage)
                .Image(Item.IsValid() ? OutlinerKindIcon(Item->Kind) : nullptr)
                .ColorAndOpacity_Lambda([Item]()
                {
                    if (!Item.IsValid()) return FSlateColor::UseForeground();
                    if (Item->bMissingEndpoint) return FSlateColor(FLinearColor(0.9f, 0.12f, 0.08f));
                    if (Item->bSelectedOption) return FSlateColor(FLinearColor(0.15f, 0.8f, 0.3f));
                    return FSlateColor::UseForeground();
                })
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(Item.IsValid() ? FText::FromString(Item->Label) : FText::GetEmpty())
                .ColorAndOpacity_Lambda([Item, WeakSelf = TWeakPtr<SMHCompositeOutliner>(SharedThis(this))]()
                {
                    if (!Item.IsValid()) return FSlateColor::UseForeground();
                    if (Item->bMissingEndpoint) return FSlateColor(FLinearColor(1.0f, 0.18f, 0.12f));
                    // R6-UX1: the edited subtree stands out; the rest waits for the session to end.
                    const TSharedPtr<SMHCompositeOutliner> Self = WeakSelf.Pin();
                    if (Self.IsValid() && !Self->RowScopePath.IsEmpty())
                    {
                        if (Item->NodePath == Self->RowScopePath) return FSlateColor(FLinearColor(1.0f, 0.75f, 0.2f));
                        if (!Item->NodePath.StartsWith(Self->RowScopePath + TEXT(">")) &&
                            !Item->NodePath.StartsWith(Self->RowScopePath + TEXT("/"))) return FSlateColor::UseSubduedForeground();
                    }
                    if (Item->bSelectedOption) return FSlateColor(FLinearColor(0.2f, 0.9f, 0.35f));
                    return FSlateColor::UseForeground();
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(Item.IsValid() ? FText::FromString(OutlinerKindText(Item->Kind)) : FText::GetEmpty())
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("SelectedOptionMark", "selected"))
                .ColorAndOpacity(FLinearColor(0.2f, 0.9f, 0.35f))
                .Visibility_Lambda([Item]()
                {
                    return Item.IsValid() && Item->bSelectedOption
                        ? EVisibility::Visible
                        : EVisibility::Collapsed;
                })
            ]
        ];
    }

    void GetTreeChildren(
        TSharedPtr<FMHCompositeOutlinerItem> Item,
        TArray<TSharedPtr<FMHCompositeOutlinerItem>>& OutChildren)
    {
        if (!Item.IsValid()) return;
        if (Item->IsCompositeReference() && !Item->bNestedChildrenLoaded)
            Model.ExpandItem(Item);
        OutChildren = Item->Children;
    }

    void TreeSelectionChanged(
        TSharedPtr<FMHCompositeOutlinerItem> Item,
        const ESelectInfo::Type SelectInfo)
    {
        if (SelectInfo == ESelectInfo::Direct || bSyncingTreeSelection) return;
        SelectedItem = Item;
        RebuildDetails();
        if (GEditor == nullptr) return;
        // CE-3b: under the CE backend a row grabs its projection component
        // through the mode (the gizmo sits on the node). Selection is stored
        // as authored GUIDs: option rows and resolved contents of a composite
        // reference climb to the nearest editable authored row.
        if (UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive())
        {
            if (const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get()))
            {
                TArray<TSharedPtr<FMHCompositeOutlinerItem>> SelectedRows;
                if (TreeView.IsValid()) SelectedRows = TreeView->GetSelectedItems();
                TArray<FGuid> RowIds;
                TSet<FGuid> DesiredIds;
                for (const TSharedPtr<FMHCompositeOutlinerItem>& Row : SelectedRows)
                {
                    const FGuid RowId = EditableNodeIdForItem(Row);
                    if (!RowId.IsValid() || DesiredIds.Contains(RowId)) continue;
                    DesiredIds.Add(RowId);
                    RowIds.Add(RowId);
                }

                // Read-only context rows remain browsable for their Edit Contents
                // menu; they do not become transform targets of this definition.
                const FGuid ClickedId = EditableNodeIdForItem(Item);
                if (!ClickedId.IsValid() && Item.IsValid()) return;

                TArray<FGuid> OrderedIds;
                OrderedIds.Reserve(DesiredIds.Num());
                for (const FGuid& ExistingId : Session->GetSelectedNodeIds())
                {
                    if (DesiredIds.Contains(ExistingId)) OrderedIds.Add(ExistingId);
                }
                for (const FGuid& RowId : RowIds)
                {
                    if (!OrderedIds.Contains(RowId)) OrderedIds.Add(RowId);
                }
                const FGuid ActiveId = DesiredIds.Contains(ClickedId)
                    ? ClickedId
                    : DesiredIds.Contains(Session->GetActiveNodeId())
                        ? Session->GetActiveNodeId()
                        : OrderedIds.IsEmpty() ? FGuid() : OrderedIds[0];
                Mode->SelectNodeIds(OrderedIds, ActiveId);
                // SetSelectedNodeIds intentionally does not broadcast a no-op;
                // still canonicalize a clicked nested visual to its authored row.
                SyncTreeSelectionFromSession();
                return;
            }
        }
        if (!Item.IsValid()) return;
        // R6-UX1: in a session a row of the edited subtree grabs its handle —
        // the gizmo moves the node, never the actor. Rows outside the subtree
        // leave the selection alone until the session ends.
        if (CurrentActor.IsValid() && CurrentActor->IsPlacementEditMode())
        {
            if (USceneComponent* Handle = CurrentActor->FindSessionHandleForNodePath(Item->NodePath))
            {
                CurrentActor->SelectPlacementLeafByNodePath(Item->NodePath);
                SelectHandle(Handle);
            }
            return;
        }
        USceneComponent* Component = Item->PlacementComponent.Get();
        if (!IsValid(Component)) return;
        if (Item->PlacementInstanceIndex != INDEX_NONE)
        {
            // Pooled leaf (16 §2.8): the ISM address moves under swap-remove;
            // resolve the current one through the actor's handle row.
            const UE::MimirComposite::FMHCompositeLeafMaterialization* Row =
                CurrentActor->FindLeafMaterializationByNodePath(Item->NodePath);
            UInstancedStaticMeshComponent* Bucket = Row != nullptr
                ? Cast<UInstancedStaticMeshComponent>(Row->Component.Get()) : nullptr;
            const int32 InstanceIndex = Row != nullptr ? Row->InstanceIndex : INDEX_NONE;
            if (Bucket == nullptr || InstanceIndex == INDEX_NONE) return;
            // R6-D1a: a pooled instance is never the selection element (the
            // stock gizmo would edit the ISM behind the model). Select the
            // composite and record the leaf; the pool highlights it (R5b-2a).
            GEditor->SelectNone(false, true, false);
            CurrentActor->SelectPlacementLeafByNodePath(Item->NodePath);
            GEditor->SelectActor(CurrentActor.Get(), true, true, true);
            GEditor->RedrawLevelEditingViewports();
            return;
        }
        SelectHandle(Component);
        CurrentActor->SelectPlacementLeaf(Component);
    }

    // CE-4b2: draft rows take Content Browser assets — onto a group as its
    // child, above/below any row as its sibling. Only managed meshes and
    // composites become nodes; the rest is refused with the reason.
    TOptional<EItemDropZone> CanAcceptDrop(const FDragDropEvent& Event, const EItemDropZone Zone, TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        const TSharedPtr<FAssetDragDropOp> Op = Event.GetOperationAs<FAssetDragDropOp>();
        if (!Op.IsValid() || !Item.IsValid() || !Item->DraftNodeId.IsValid() || !UMHCompositeEditorMode::IsActive()) return TOptional<EItemDropZone>();
        const bool bTakesInto = Item->Kind == EMHRandomSemanticKind::Group || Item->Kind == EMHRandomSemanticKind::Random;
        return Zone == EItemDropZone::OntoItem && !bTakesInto ? EItemDropZone::BelowItem : Zone;
    }

    FReply AcceptDrop(const FDragDropEvent& Event, const EItemDropZone Zone, TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        const TSharedPtr<FAssetDragDropOp> Op = Event.GetOperationAs<FAssetDragDropOp>();
        UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
        if (!Op.IsValid() || Mode == nullptr || Draft == nullptr || !Item.IsValid() || !Item->DraftNodeId.IsValid()) return FReply::Unhandled();
        // CE-4b3: onto a random node the assets become its options.
        if (Zone == EItemDropZone::OntoItem && Item->Kind == EMHRandomSemanticKind::Random)
        {
            const FMHCompositeAssetNode* Node = DraftNodeOf(*Item);
            if (Node == nullptr) return FReply::Unhandled();
            TArray<FMHCompositeOption> Options = Node->Options;
            TArray<FString> RefusedAssets;
            for (const FAssetData& AssetData : Op->GetAssets())
            {
                FMHOutlinerAddRequest Request;
                FString Error;
                if (!MHDescribeOutlinerAssetAdd(AssetData.GetAsset(), nullptr, *Draft, Request, Error)) { RefusedAssets.Add(Error); continue; }
                FMHCompositeOption Option;
                Option.Kind = Request.Kind == EMHCompositeNodeKind::Composite ? EMHCompositeOptionKind::Composite : EMHCompositeOptionKind::Mesh;
                Option.Resource = Request.Resource;
                Option.Weight = 1.0f;
                Options.Add(Option);
            }
            for (const FString& Error : RefusedAssets) FMessageLog("Mimir").Warning(FText::FromString(Error));
            if (!RefusedAssets.IsEmpty()) FMessageLog("Mimir").Open(EMessageSeverity::Warning, true);
            const FGuid Id = Item->DraftNodeId;
            CommitDraftEdit(LOCTEXT("DropOptionsTransaction", "Add Random Options"), [Id, Options](UMHCompositeEditSession& InSession, FString& Error) { return InSession.SetNodeOptions(Id, Options, Error); });
            return FReply::Handled();
        }
        const bool bInto = Zone == EItemDropZone::OntoItem && Item->Kind == EMHRandomSemanticKind::Group;
        const FGuid ParentId = bInto ? Item->DraftNodeId : Draft->GetParentId(Item->DraftNodeId);
        int32 SiblingIndex = INDEX_NONE;
        if (!bInto)
        {
            const TArray<FGuid> Siblings = Draft->GetChildIds(ParentId);
            SiblingIndex = Siblings.IndexOfByKey(Item->DraftNodeId) + (Zone == EItemDropZone::AboveItem ? 0 : 1);
        }
        TArray<FGuid> Added;
        TArray<FString> Refused;
        {
            const FScopedTransaction Transaction(LOCTEXT("DropNodesTransaction", "Add Composite Nodes"));
            for (const FAssetData& AssetData : Op->GetAssets())
            {
                FMHOutlinerAddRequest Request;
                FString Error;
                if (!MHDescribeOutlinerAssetAdd(AssetData.GetAsset(), nullptr, *Draft, Request, Error))
                {
                    Refused.Add(Error);
                    continue;
                }
                AddDraftNode(ParentId, Request.Kind, Request.Resource, Request.Name, SiblingIndex, Added);
                if (SiblingIndex != INDEX_NONE) ++SiblingIndex;
            }
        }
        for (const FString& Error : Refused) FMessageLog("Mimir").Warning(FText::FromString(Error));
        if (!Refused.IsEmpty()) FMessageLog("Mimir").Open(EMessageSeverity::Warning, true);
        FinishDraftCommand(Added.IsEmpty() ? FGuid() : Added.Last());
        return FReply::Handled();
    }

    /** One draft add: the session command, placed among its siblings when asked. */
    void AddDraftNode(const FGuid& ParentId, const EMHCompositeNodeKind Kind, const FString& Resource, const FString& Name, const int32 SiblingIndex, TArray<FGuid>& OutAdded)
    {
        UMHCompositeEditSession* Session = const_cast<UMHCompositeEditSession*>(SessionOf(CurrentActor.Get()));
        if (Session == nullptr) return;
        FString Error;
        const FGuid Id = Session->AddNode(ParentId, Kind, Resource, Name, FTransform::Identity, Error);
        if (!Id.IsValid())
        {
            FMessageLog("Mimir").Error(FText::FromString(Error));
            FMessageLog("Mimir").Open(EMessageSeverity::Error, true);
            return;
        }
        if (SiblingIndex != INDEX_NONE) Session->ReparentNode(Id, ParentId, SiblingIndex, false, Error);
        OutAdded.Add(Id);
    }

    /** After a draft command: rebuild the rows and grab the node the command produced. */
    void FinishDraftCommand(const FGuid& SelectId)
    {
        RefreshModel();
        UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        const UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
        if (Mode != nullptr && Projection != nullptr && SelectId.IsValid()) Mode->SelectComponent(Projection->FindComponentForNodeId(SelectId));
    }

    /** CE-4b3: the draft node a row shows (authoritative for editing). */
    const FMHCompositeAssetNode* DraftNodeOf(const FMHCompositeOutlinerItem& Item) const
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        const UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
        const int32 Index = Draft != nullptr ? Draft->FindNodeIndex(Item.DraftNodeId) : INDEX_NONE;
        return Index != INDEX_NONE ? &Draft->GetNodes()[Index] : nullptr;
    }

    /**
     * CE-4b3: one draft command from a details widget — deferred to the next
     * tick (the rebuild replaces the widget that delivered the event), one
     * transaction, errors to the Message Log, the rows rebuilt after.
     */
    template <typename TCommand>
    void CommitDraftEdit(const FText& Title, TCommand Command)
    {
        if (GEditor == nullptr) return;
        TWeakPtr<SMHCompositeOutliner> Weak = StaticCastSharedRef<SMHCompositeOutliner>(AsShared());
        GEditor->GetTimerManager()->SetTimerForNextTick([Weak, Title, Command]()
        {
            const TSharedPtr<SMHCompositeOutliner> Self = Weak.Pin();
            UMHCompositeEditSession* Session = Self.IsValid() ? const_cast<UMHCompositeEditSession*>(Self->SessionOf(Self->CurrentActor.Get())) : nullptr;
            if (Session == nullptr) return;
            FString Error;
            bool bOk = false;
            {
                const FScopedTransaction Transaction(Title);
                bOk = Command(*Session, Error);
            }
            if (!bOk && !Error.IsEmpty())
            {
                FMessageLog("Mimir").Error(FText::FromString(Error));
                FMessageLog("Mimir").Open(EMessageSeverity::Error, true);
            }
            Self->RefreshModel();
        });
    }

    /** CE-4b3: name, resource and random options of a draft row are edited in place. */
    TSharedRef<SWidget> BuildEditSection(const TSharedPtr<FMHCompositeOutlinerItem>& Item, const FMHCompositeAssetNode& Node)
    {
        const FGuid Id = Item->DraftNodeId;
        TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
        auto AddRow = [&Box](const FText& Label, const TSharedRef<SWidget>& Widget)
        {
            Box->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(STextBlock).Text(Label).ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]
                + SHorizontalBox::Slot().FillWidth(1.0f)
                [
                    Widget
                ]
            ];
        };
        AddRow(LOCTEXT("EditNameLabel", "Name"), SNew(SEditableTextBox)
            .Text(FText::FromString(Node.Name))
            .OnTextCommitted_Lambda([this, Id](const FText& Text, const ETextCommit::Type Commit)
            {
                if (Commit != ETextCommit::OnEnter && Commit != ETextCommit::OnUserMovedFocus) return;
                CommitDraftEdit(LOCTEXT("RenameNodeTransaction", "Rename Composite Node"), [Id, Name = Text.ToString()](UMHCompositeEditSession& InSession, FString& Error) { return InSession.SetNodeName(Id, Name, Error); });
            }));
        const bool bTakesResource = Node.Kind != EMHCompositeNodeKind::Group && Node.Kind != EMHCompositeNodeKind::Random;
        if (bTakesResource)
        {
            AddRow(LOCTEXT("EditResourceLabel", "Resource"), SNew(SEditableTextBox)
                .Text(FText::FromString(Node.Resource))
                .ToolTipText(LOCTEXT("EditResourceTip", "Logical name of a managed mesh, actor, composite or gameobj ([a-z0-9_]+). Dropping an asset on a row adds a node instead."))
                .OnTextCommitted_Lambda([this, Id](const FText& Text, const ETextCommit::Type Commit)
                {
                    if (Commit != ETextCommit::OnEnter && Commit != ETextCommit::OnUserMovedFocus) return;
                    CommitDraftEdit(LOCTEXT("ResourceTransaction", "Change Composite Node Resource"), [Id, Resource = Text.ToString()](UMHCompositeEditSession& InSession, FString& Error) { return InSession.SetNodeResource(Id, Resource, Error); });
                }));
        }
        if (Node.Kind == EMHCompositeNodeKind::Random)
        {
            const TArray<FMHCompositeOption> Options = Node.Options;
            for (int32 Index = 0; Index < Options.Num(); ++Index)
            {
                const FMHCompositeOption& Option = Options[Index];
                const FString Label = Option.Kind == EMHCompositeOptionKind::Empty ? TEXT("-- (empty)") : Option.Resource;
                Box->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock).Text(FText::FromString(Label))
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f)
                    [
                        SNew(SBox).WidthOverride(72.0f)
                        [
                            SNew(SSpinBox<float>)
                            .MinValue(0.0f)
                            .Value(Option.Weight)
                            .ToolTipText(LOCTEXT("OptionWeightTip", "Weight of this option (finite, non-negative; at least one option must be positive)."))
                            .OnValueCommitted_Lambda([this, Id, Options, Index](const float Value, ETextCommit::Type)
                            {
                                TArray<FMHCompositeOption> Next = Options;
                                if (!Next.IsValidIndex(Index)) return;
                                Next[Index].Weight = Value;
                                CommitDraftEdit(LOCTEXT("OptionWeightTransaction", "Change Random Option Weight"), [Id, Next](UMHCompositeEditSession& InSession, FString& Error) { return InSession.SetNodeOptions(Id, Next, Error); });
                            })
                        ]
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("RemoveOption", "Remove"))
                        .ToolTipText(LOCTEXT("RemoveOptionTip", "Remove this option (a random node keeps at least one positive option)."))
                        .OnClicked_Lambda([this, Id, Options, Index]()
                        {
                            TArray<FMHCompositeOption> Next = Options;
                            if (Next.IsValidIndex(Index)) Next.RemoveAt(Index);
                            CommitDraftEdit(LOCTEXT("RemoveOptionTransaction", "Remove Random Option"), [Id, Next](UMHCompositeEditSession& InSession, FString& Error) { return InSession.SetNodeOptions(Id, Next, Error); });
                            return FReply::Handled();
                        })
                    ]
                ];
            }
            Box->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("AddEmptyOption", "Add Empty Option"))
                    .ToolTipText(LOCTEXT("AddEmptyOptionTip", "Add an empty option with weight 1. Drop a managed mesh or composite onto this row to add it as an option."))
                    .OnClicked_Lambda([this, Id, Options]()
                    {
                        TArray<FMHCompositeOption> Next = Options;
                        FMHCompositeOption Empty;
                        Empty.Kind = EMHCompositeOptionKind::Empty;
                        Empty.Weight = 1.0f;
                        Next.Add(Empty);
                        CommitDraftEdit(LOCTEXT("AddEmptyOptionTransaction", "Add Random Option"), [Id, Next](UMHCompositeEditSession& InSession, FString& Error) { return InSession.SetNodeOptions(Id, Next, Error); });
                        return FReply::Handled();
                    })
                ]
            ];
        }
        return Box;
    }

    void AddRandomNodeAt(const TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        UMHCompositeEditSession* Session = const_cast<UMHCompositeEditSession*>(SessionOf(CurrentActor.Get()));
        const UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
        if (Draft == nullptr) return;
        FMHCompositeOption Empty;
        Empty.Kind = EMHCompositeOptionKind::Empty;
        Empty.Weight = 1.0f;
        FGuid Added;
        FString Error;
        {
            const FScopedTransaction Transaction(LOCTEXT("AddRandomNodeTransaction", "Add Random Composite Node"));
            Added = Session->AddRandomNode(MHOutlinerAddParentFor(Item.Get(), *Draft), TEXT("random"), FTransform::Identity, {Empty}, Error);
        }
        if (!Added.IsValid() && !Error.IsEmpty()) FMessageLog("Mimir").Error(FText::FromString(Error));
        FinishDraftCommand(Added);
    }

    void AddEmptyNodeAt(const TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        const UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
        if (Draft == nullptr) return;
        TArray<FGuid> Added;
        {
            const FScopedTransaction Transaction(LOCTEXT("AddEmptyNodeTransaction", "Add Empty Composite Node"));
            AddDraftNode(MHOutlinerAddParentFor(Item.Get(), *Draft), EMHCompositeNodeKind::Group, FString(), TEXT("empty"), INDEX_NONE, Added);
        }
        FinishDraftCommand(Added.IsEmpty() ? FGuid() : Added.Last());
    }

    void DuplicateDraftNode(const TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        UMHCompositeEditSession* Session = const_cast<UMHCompositeEditSession*>(SessionOf(CurrentActor.Get()));
        if (Session == nullptr || !Item.IsValid()) return;
        FGuid Copy;
        FString Error;
        {
            const FScopedTransaction Transaction(LOCTEXT("DuplicateNodeTransaction", "Duplicate Composite Node"));
            Copy = Session->DuplicateNode(Item->DraftNodeId, Error);
        }
        if (!Copy.IsValid() && !Error.IsEmpty()) FMessageLog("Mimir").Error(FText::FromString(Error));
        FinishDraftCommand(Copy);
    }

    void DeleteDraftNode(const TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        UMHCompositeEditSession* Session = const_cast<UMHCompositeEditSession*>(SessionOf(CurrentActor.Get()));
        if (Session == nullptr || !Item.IsValid()) return;
        FString Error;
        {
            const FScopedTransaction Transaction(LOCTEXT("DeleteNodeTransaction", "Delete Composite Node"));
            if (!Session->DeleteNode(Item->DraftNodeId, Error) && !Error.IsEmpty()) FMessageLog("Mimir").Error(FText::FromString(Error));
        }
        FinishDraftCommand(FGuid());
    }

    /** CE-3b: the open CE-backend session of this placement, if any. */
    static const UMHCompositeEditSession* SessionOf(const AMHCompositeActor* Actor)
    {
        const UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
        const UMHCompositeEditSession* Session = Subsystem != nullptr ? Subsystem->GetEditSession() : nullptr;
        return Session != nullptr && Session->IsOpen() && Actor != nullptr && Session->GetRootPlacement() == Actor ? Session : nullptr;
    }

    /** Nearest authored node represented by this row (options and referenced contents normalize upward). */
    static FGuid EditableNodeIdForItem(TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        while (Item.IsValid())
        {
            if (Item->DraftNodeId.IsValid()) return Item->DraftNodeId;
            Item = Item->Parent.Pin();
        }
        return FGuid();
    }

    static TSharedPtr<FMHCompositeOutlinerItem> FindItemByNodeId(
        const TArray<TSharedPtr<FMHCompositeOutlinerItem>>& Items,
        const FGuid& NodeId)
    {
        for (const TSharedPtr<FMHCompositeOutlinerItem>& Item : Items)
        {
            if (!Item.IsValid()) continue;
            if (Item->DraftNodeId == NodeId) return Item;
            if (TSharedPtr<FMHCompositeOutlinerItem> Found = FindItemByNodeId(Item->Children, NodeId)) return Found;
        }
        return nullptr;
    }

    void ObserveSession(UMHCompositeEditSession* Session)
    {
        if (ObservedSession.Get() == Session) return;
        if (UMHCompositeEditSession* Previous = ObservedSession.Get())
        {
            Previous->OnSelectionChanged.Remove(SessionSelectionChangedHandle);
            Previous->OnChanged.Remove(SessionChangedHandle);
        }
        SessionSelectionChangedHandle.Reset();
        SessionChangedHandle.Reset();
        ObservedSession = Session;
        if (Session != nullptr)
        {
            SessionSelectionChangedHandle = Session->OnSelectionChanged.AddSP(
                SharedThis(this), &SMHCompositeOutliner::SessionSelectionChanged);
            SessionChangedHandle = Session->OnChanged.AddSP(
                SharedThis(this), &SMHCompositeOutliner::SessionChanged);
        }
    }

    void SessionSelectionChanged()
    {
        if (UMHCompositeEditorMode::IsActive()) SyncTreeSelectionFromSession();
    }

    void SessionChanged()
    {
        if (UMHCompositeEditorMode::IsActive() && !bRefreshingModel) RefreshModel();
    }

    /** Mirror semantic GUID selection into the current row objects after refresh/reorder/Undo. */
    void SyncTreeSelectionFromSession()
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        if (Session == nullptr || !TreeView.IsValid() || !UMHCompositeEditorMode::IsActive()) return;

        bSyncingTreeSelection = true;
        TreeView->ClearSelection();
        TSharedPtr<FMHCompositeOutlinerItem> FirstItem;
        TSharedPtr<FMHCompositeOutlinerItem> ActiveItem;
        for (const FGuid& NodeId : Session->GetSelectedNodeIds())
        {
            const TSharedPtr<FMHCompositeOutlinerItem> SelectedRow = FindItemByNodeId(RootItems, NodeId);
            if (!SelectedRow.IsValid()) continue;
            if (!FirstItem.IsValid()) FirstItem = SelectedRow;
            if (NodeId == Session->GetActiveNodeId()) ActiveItem = SelectedRow;
            for (TSharedPtr<FMHCompositeOutlinerItem> Parent = SelectedRow->Parent.Pin();
                 Parent.IsValid(); Parent = Parent->Parent.Pin())
            {
                TreeView->SetItemExpansion(Parent, true);
            }
            TreeView->SetItemSelection(SelectedRow, true, ESelectInfo::Direct);
        }
        if (!ActiveItem.IsValid()) ActiveItem = FirstItem;
        SelectedItem = ActiveItem;
        if (ActiveItem.IsValid()) TreeView->RequestScrollIntoView(ActiveItem);
        bSyncingTreeSelection = false;
        RebuildDetails();
    }

    void SelectHandle(USceneComponent* Handle)
    {
        USelection* Components = GEditor != nullptr ? GEditor->GetSelectedComponents() : nullptr;
        if (Components == nullptr || !IsValid(Handle)) return;
        AActor* Owner = Handle->GetOwner();
        if (Owner != nullptr && !Owner->IsSelected()) GEditor->SelectActor(Owner, true, true, true);
        Components->BeginBatchSelectOperation();
        Components->DeselectAll();
        GEditor->SelectComponent(Handle, true, false, true);
        Components->EndBatchSelectOperation(true);
        GEditor->NoteSelectionChange();
        GEditor->RedrawLevelEditingViewports();
    }

    /** R6-UX1: the edited sub-composite stays in view — expanded, revealed, and its first handle grabbed. */
    void FocusEditScope(const FString& InvocationPath)
    {
        AMHCompositeActor* Root = CurrentActor.Get();
        if (Root == nullptr || GEditor == nullptr) return;
        if (TSharedPtr<FMHCompositeOutlinerItem> Item = Model.FindByNodePath(InvocationPath))
        {
            if (Item->IsCompositeReference() && !Item->bNestedChildrenLoaded) Model.ExpandItem(Item);
            RevealItem(Item);
            if (TreeView.IsValid()) TreeView->SetItemExpansion(Item, true);
        }
        // The mode owns logical node selection; entering Edit preserves the camera.
        if (UMHCompositeEditorMode::IsActive() && SessionOf(Root) != nullptr) return;
        const TArray<TObjectPtr<USceneComponent>>& Handles = Root->GetEditScopeHandles();
        if (!Handles.IsEmpty() && IsValid(Handles[0])) SelectHandle(Handles[0]);
    }

    TSharedPtr<SWidget> OpenContextMenu()
    {
        const TSharedPtr<FMHCompositeOutlinerItem> Item = SelectedItem;
        if (!Item.IsValid()) return nullptr;
        const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
        FMHCompositeOutlinerNavigation Navigation;
        const bool bHasNavigation = Model.GetNavigation(
            *Item,
            Settings != nullptr ? Settings->GetSourceRootPath() : FString(),
            Navigation);
        FMenuBuilder Menu(true, nullptr);
        const UMHCompositeLevelSubsystem* EditSubsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
        const bool bSessionActive = EditSubsystem != nullptr && EditSubsystem->IsEditingComposite();
        if (bSessionActive)
        {
            // R6-D2: publish the shared definition from the Outliner as well.
            if (!EditSubsystem->GetEditContext().InvocationPath.IsEmpty() && EditSubsystem->IsEditingComposite(CurrentActor.Get()))
            {
                Menu.AddMenuEntry(
                    LOCTEXT("ApplySharedDefinition", "Apply Shared Definition"),
                    LOCTEXT("ApplySharedDefinitionTip", "Publish the edited nested definition to its .composite source and refresh every placement that invokes it."),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::ApplySharedDefinition)));
                // R6-UX2b: one entry; the dialog explains scope and bake.
                Menu.AddMenuEntry(
                    LOCTEXT("SaveUniqueCopy", "Save As Unique Copy..."),
                    LOCTEXT("SaveUniqueCopyTip", "Save the edited definition as a new unique composite: choose whether it takes effect in this definition or for this placement only, and whether to bake the current result."),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::SaveAsUniqueCopy)));
            }
            // CE-4b2: structural commands on the draft's rows (owner decision:
            // the mode grows the composite — empty/composite/mesh/actor nodes;
            // composite and mesh come from the Content Browser by drag & drop).
            if (UMHCompositeEditorMode::IsActive() && Item->DraftNodeId.IsValid() && EditSubsystem->IsEditingComposite(CurrentActor.Get()))
            {
                Menu.AddMenuEntry(
                    LOCTEXT("AddEmptyNode", "Add Empty Node"),
                    LOCTEXT("AddEmptyNodeTip", "Add an empty group node: into this group, or next to this node. Drag a managed static mesh or composite from the Content Browser onto a row to add it as a node."),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::AddEmptyNodeAt, Item)));
                Menu.AddMenuEntry(
                    LOCTEXT("AddRandomNode", "Add Random Node"),
                    LOCTEXT("AddRandomNodeTip", "Add a random node with one empty option: into this group, or next to this node. Edit its options in the details below; drop managed meshes or composites onto it to add options."),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::AddRandomNodeAt, Item)));
                Menu.AddMenuEntry(
                    LOCTEXT("DuplicateNode", "Duplicate Node"),
                    LOCTEXT("DuplicateNodeTip", "Copy this node with its subtree right after it."),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::DuplicateDraftNode, Item)));
                Menu.AddMenuEntry(
                    LOCTEXT("DeleteNode", "Delete Node"),
                    LOCTEXT("DeleteNodeTip", "Remove this node with its subtree from the draft (Undo restores it)."),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::DeleteDraftNode, Item)));
            }
            // CE-3d: another definition of this placement from the open session
            // (Save / Discard / stay first when there are changes).
            if (UMHCompositeEditorMode::IsActive() && Item->IsCompositeReference() && !Item->NodePath.IsEmpty() &&
                Item->NodePath != EditSubsystem->GetEditContext().InvocationPath && EditSubsystem->IsEditingComposite(CurrentActor.Get()))
            {
                Menu.AddMenuEntry(
                    LOCTEXT("SwitchEditContents", "Edit Contents..."),
                    LOCTEXT("SwitchEditContentsTip", "Leave the definition being edited (Save, Discard or stay) and edit this one in the context of this placement."),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::SwitchEditContents, Item->NodePath)));
            }
            Menu.AddMenuEntry(
                LOCTEXT("CancelEditContents", "Cancel Edit Contents (Esc)"),
                LOCTEXT("CancelEditContentsTip", "Close the active edit session and discard its draft. The source is not changed."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::CancelEditContents)));
        }
        else if (Item->IsCompositeReference() && !Item->NodePath.IsEmpty() && CurrentActor.IsValid())
        {
            // R6-D0 (docs/16 §2.7): open the shared child definition as a draft
            // under this placement; the source stays untouched until published.
            Menu.AddMenuEntry(
                LOCTEXT("EditContents", "Edit Contents..."),
                LOCTEXT("EditContentsTip", "Open the nested composite definition for editing in the context of this placement: its nodes get handles here, Apply Shared Definition publishes, Cancel Edit Contents discards."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::EditContents, Item->NodePath)));
        }
        if (bHasNavigation && Navigation.Asset.IsValid() && Navigation.Asset->IsA<UMHCompositeAsset>())
        {
            Menu.AddMenuEntry(
                LOCTEXT("OpenAsset", "Open Asset"),
                LOCTEXT("OpenAssetTip", "Open the referenced managed composite asset."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateSP(
                    SharedThis(this), &SMHCompositeOutliner::OpenAsset, Navigation.Asset)));
        }
        if (bHasNavigation && Navigation.Asset.IsValid())
        {
            Menu.AddMenuEntry(
                LOCTEXT("BrowseAsset", "Browse to Asset"),
                LOCTEXT("BrowseAssetTip", "Select the referenced generated asset in the Content Browser."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateSP(
                    SharedThis(this), &SMHCompositeOutliner::BrowseAsset, Navigation.Asset)));
        }
        Menu.AddMenuEntry(
            LOCTEXT("CopyName", "Copy Name"),
            LOCTEXT("CopyNameTip", "Copy the source node or resource name."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([Name = Model.GetCopyName(*Item)]()
            {
                FPlatformApplicationMisc::ClipboardCopy(*Name);
            })));
        if (bHasNavigation && !Navigation.SourceFilepath.IsEmpty())
        {
            Menu.AddMenuEntry(
                LOCTEXT("CopySourceFilepath", "Copy Source Filepath"),
                LOCTEXT("CopySourceFilepathTip", "Copy the absolute .composite source filepath."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([Path = Navigation.SourceFilepath]()
                {
                    FPlatformApplicationMisc::ClipboardCopy(*Path);
                })));
            Menu.AddMenuEntry(
                LOCTEXT("RevealExplorer", "Reveal in Explorer"),
                LOCTEXT("RevealExplorerTip", "Open the source document's containing folder."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([Path = Navigation.SourceFilepath]()
                {
                    FPlatformProcess::ExploreFolder(*FPaths::GetPath(Path));
                })));
        }
        return Menu.MakeWidget();
    }

    void EditContents(const FString InvocationPath)
    {
        UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
        AMHCompositeActor* Root = CurrentActor.Get();
        if (Subsystem == nullptr || Root == nullptr) return;
        FString Error;
        const FString PickedLeaf = Root->GetSelectedPlacementLeafPath();
        const bool bEditPicked = !PickedLeaf.IsEmpty() && Root->GetSelectedPlacementOccurrencePath() == InvocationPath;
        const bool bStarted = bEditPicked
            ? UE::MimirComposite::MHBeginEditPickedComposite(*Root, PickedLeaf, Error)
            : Subsystem->BeginEditNestedComposite(Root, InvocationPath, Error);
        if (!bStarted)
        {
            FMessageLog("Mimir").Error(FText::FromString(Error));
            FMessageLog("Mimir").Open(EMessageSeverity::Error, true);
        }
        RefreshModel();
        if (bStarted && !bEditPicked) FocusEditScope(InvocationPath);
    }

    /** CE-3d: switch the open session to another definition of this placement. */
    void SwitchEditContents(const FString InvocationPath)
    {
        UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
        if (Mode == nullptr) return;
        const bool bSwitched = Mode->RequestSwitch(InvocationPath);
        RefreshModel();
        if (bSwitched) FocusEditScope(InvocationPath);
    }

    void CancelEditContents()
    {
        UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
        if (Subsystem == nullptr) return;
        FString Error;
        if (!Subsystem->CancelEditComposite(Error) && !Error.IsEmpty())
        {
            FMessageLog("Mimir").Error(FText::FromString(Error));
            FMessageLog("Mimir").Open(EMessageSeverity::Error, true);
        }
        RefreshModel();
    }

    void ApplySharedDefinition()
    {
        MHExecuteCommitEditCompositeInteractive();
        RefreshModel();
    }

    void SaveAsUniqueCopy()
    {
        MHExecuteSaveUniqueCopyInteractive();
        RefreshModel();
    }

    void OpenAsset(TWeakObjectPtr<UObject> Asset) const
    {
        if (GEditor != nullptr && Asset.IsValid())
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset.Get());
    }

    void BrowseAsset(TWeakObjectPtr<UObject> Asset) const
    {
        if (GEditor != nullptr && Asset.IsValid())
        {
            TArray<UObject*> Objects{Asset.Get()};
            GEditor->SyncBrowserToObjects(Objects);
        }
    }

    void AddSection(const FText& Title, const TSharedRef<SWidget>& Content)
    {
        DetailsBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
            .Padding(6.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(Title)
                    .Font(FAppStyle::GetFontStyle(TEXT("DetailsView.CategoryFontStyle")))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    Content
                ]
            ]
        ];
    }

    static void AddField(
        const TSharedRef<SVerticalBox>& Box,
        const FText& Label,
        const FString& Value)
    {
        Box->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(STextBlock).Text(Label).ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ]
            + SHorizontalBox::Slot().FillWidth(1.0f)
            [
                SNew(STextBlock).Text(FText::FromString(Value)).AutoWrapText(true)
            ]
        ];
    }

    void RebuildDetails()
    {
        if (!DetailsBox.IsValid()) return;
        DetailsBox->ClearChildren();
        if (!SelectedItem.IsValid())
        {
            DetailsBox->AddSlot().AutoHeight()
            [
                SNew(STextBlock)
                .Text(LOCTEXT("SelectNode", "Select a source node to inspect it."))
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ];
            return;
        }

        const TSharedPtr<FMHCompositeOutlinerItem> Item = SelectedItem;
        // CE-4b3: a draft row edits its name, resource and random options here.
        if (Item->DraftNodeId.IsValid() && !Item->IsOption() && UMHCompositeEditorMode::IsActive())
        {
            if (const FMHCompositeAssetNode* DraftNode = DraftNodeOf(*Item)) AddSection(LOCTEXT("EditSection", "Edit"), BuildEditSection(Item, *DraftNode));
        }
        TSharedRef<SVerticalBox> Entities = SNew(SVerticalBox);
        if (Item->Kind == EMHRandomSemanticKind::Random && !Item->IsOption())
        {
            for (const TSharedPtr<FMHCompositeOutlinerItem>& Child : Item->Children)
            {
                if (!Child->IsOption()) continue;
                Entities->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.0f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Child->Label + (Child->bSelectedOption ? TEXT("  [selected]") : TEXT(""))))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(FString::Printf(TEXT("Weight %.9g"), static_cast<double>(Child->Weight))))
                    ]
                ];
            }
        }
        else
        {
            Entities->AddSlot().AutoHeight()
            [SNew(STextBlock).Text(LOCTEXT("NoEntities", "Not a random node."))
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())];
        }
        AddSection(LOCTEXT("EntitiesSection", "Entities"), Entities);

        TSharedRef<SVerticalBox> Children = SNew(SVerticalBox);
        int32 ChildCount = 0;
        for (const TSharedPtr<FMHCompositeOutlinerItem>& Child : Item->Children)
        {
            if (!Child.IsValid() || Child->IsOption()) continue;
            ++ChildCount;
            Children->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
            [SNew(STextBlock).Text(FText::FromString(Child->Label))];
        }
        if (ChildCount == 0)
        {
            Children->AddSlot().AutoHeight()
            [SNew(STextBlock).Text(LOCTEXT("NoChildren", "No child nodes."))
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())];
        }
        AddSection(LOCTEXT("ChildrenSection", "Children"), Children);

        TSharedRef<SVerticalBox> Node = SNew(SVerticalBox);
        AddField(Node, LOCTEXT("KindField", "Kind"), OutlinerKindText(Item->Kind));
        AddField(Node, LOCTEXT("ResourceField", "Resource"), Item->Resource.IsEmpty() ? TEXT("--") : Item->Resource);
        AddField(Node, LOCTEXT("NodePathField", "NodePath"), Item->NodePath);
        AddField(Node, LOCTEXT("FixedTrsField", "Fixed TRS"), Item->FixedTransform.ToHumanReadableString());
        if (Item->PlaceType != INDEX_NONE)
            AddField(Node, LOCTEXT("PlaceTypeField", "place_type"), FString::FromInt(Item->PlaceType));
        if (Item->bAppearanceSeedBoundary)
            AddField(Node, LOCTEXT("AppearanceBoundaryField", "appearance_seed_boundary"), TEXT("true"));
        if (Item->IsOption())
            AddField(Node, LOCTEXT("SelectedField", "Selected"), Item->bSelectedOption ? TEXT("yes") : TEXT("no"));
        if (Item->SampledLocalTrs.IsSet())
            AddField(Node, LOCTEXT("SampledTrsField", "Sampled TRS"), FormatRandomTrs(Item->SampledLocalTrs.GetValue()));
        AddSection(LOCTEXT("NodeSection", "Node"), Node);
    }

    void ActorSelectionChanged(const TArray<UObject*>&, bool)
    {
        RefreshSelectedActor();
    }

    void ComponentsEdited()
    {
        if (!CurrentActor.IsValid() || bRefreshPending) return;
        bRefreshPending = true;
        RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(
            SharedThis(this), &SMHCompositeOutliner::DeferredRefresh));
    }

    EActiveTimerReturnType DeferredRefresh(double, float)
    {
        bRefreshPending = false;
        RefreshModel();
        return EActiveTimerReturnType::Stop;
    }

    void EditorSelectionChanged(UObject*)
    {
        if (!CurrentActor.IsValid() || GEditor == nullptr || !TreeView.IsValid()) return;
        // In Composite Edit Mode the session is the selection authority. A
        // projection leaf may be a descendant visual of its authored node.
        if (UMHCompositeEditorMode::IsActive() && SessionOf(CurrentActor.Get()) != nullptr)
        {
            SyncTreeSelectionFromSession();
            return;
        }
        // A normal viewport hit selects the enclosing composite occurrence.
        // Keep the exact leaf on the actor for preselection after Edit opens.
        if (CurrentActor->IsSelected() && !CurrentActor->GetSelectedPlacementLeafPath().IsEmpty())
        {
            const FString& Occurrence = CurrentActor->GetSelectedPlacementOccurrencePath();
            const FString& RevealPath = Occurrence.IsEmpty() ? CurrentActor->GetSelectedPlacementLeafPath() : Occurrence;
            if (TSharedPtr<FMHCompositeOutlinerItem> Item = Model.FindByNodePath(RevealPath))
            {
                RevealItem(Item);
                return;
            }
        }
        TArray<UObject*> SelectedComponents;
        GEditor->GetSelectedComponents()->GetSelectedObjects(SelectedComponents);
        for (UObject* Object : SelectedComponents)
        {
            const USceneComponent* Component = Cast<USceneComponent>(Object);
            if (Component == nullptr || Component->GetOwner() != CurrentActor.Get()) continue;
            if (TSharedPtr<FMHCompositeOutlinerItem> Item = Model.FindForComponent(Component))
            {
                RevealItem(Item);
            }
            return;
        }
    }

    void TypedSelectionChanged(const UTypedElementSelectionSet* SelectionSet)
    {
        if (!CurrentActor.IsValid() || SelectionSet == nullptr || !TreeView.IsValid()) return;
        for (const FTypedElementHandle& Handle : SelectionSet->GetSelectedElementHandles())
        {
            const FSMInstanceManager Instance =
                SMInstanceElementDataUtil::GetSMInstanceFromHandle(Handle, true);
            if (!Instance) continue;
            UInstancedStaticMeshComponent* Component = Instance.GetISMComponent();
            // Pooled buckets belong to the pool actor; ownership is the pool's answer.
            if (Component == nullptr ||
                CurrentActor->FindLeafMaterialization(Component, Instance.GetISMInstanceIndex()) == nullptr) continue;
            CurrentActor->SelectPlacementLeaf(Component, Instance.GetISMInstanceIndex());
            if (TSharedPtr<FMHCompositeOutlinerItem> Item =
                    Model.FindForInstance(Component, Instance.GetISMInstanceIndex()))
            {
                RevealItem(Item);
            }
            return;
        }
    }

    void RevealItem(const TSharedPtr<FMHCompositeOutlinerItem>& Item)
    {
        if (!Item.IsValid() || !TreeView.IsValid()) return;
        for (TSharedPtr<FMHCompositeOutlinerItem> Parent = Item->Parent.Pin();
             Parent.IsValid(); Parent = Parent->Parent.Pin())
        {
            TreeView->SetItemExpansion(Parent, true);
        }
        TreeView->RequestTreeRefresh();
        TreeView->SetSelection(Item, ESelectInfo::Direct);
        TreeView->RequestScrollIntoView(Item);
        SelectedItem = Item;
        RebuildDetails();
    }

    void RefreshSelectedActor()
    {
        TArray<UObject*> SelectedActors;
        if (GEditor != nullptr && GEditor->GetSelectedActors() != nullptr)
        {
            GEditor->GetSelectedActors()->GetSelectedObjects(SelectedActors);
        }
        TArray<UInstancedStaticMeshComponent*> SelectedInstances;
        if (GEditor != nullptr)
        {
            if (const UTypedElementSelectionSet* SelectionSet =
                    GLevelEditorModeTools().GetEditorSelectionSet())
            {
                for (const FTypedElementHandle& Handle : SelectionSet->GetSelectedElementHandles())
                {
                    const FSMInstanceManager Instance =
                        SMInstanceElementDataUtil::GetSMInstanceFromHandle(Handle, true);
                    if (Instance && Instance.GetISMComponent() != nullptr)
                        SelectedInstances.Add(Instance.GetISMComponent());
                }
            }
        }
        // Composite Edit Mode owns native selection with its transient
        // projection (or nothing before the first node is chosen). Keep the
        // source placement as the Outliner authority for the lifetime of that
        // projection-backed session; after Close, ordinary editor selection
        // becomes authoritative again.
        AMHCompositeActor* NextActor = nullptr;
        const UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
            ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
        const UMHCompositeEditSession* EditSession = Subsystem != nullptr
            ? Subsystem->GetEditSession() : nullptr;
        if (EditSession != nullptr && EditSession->IsOpen() &&
            EditSession->GetProjection() != nullptr && EditSession->GetProjection()->IsOpen() &&
            IsValid(EditSession->GetRootPlacement()))
        {
            NextActor = EditSession->GetRootPlacement();
        }
        else
        {
            NextActor = MHResolveCompositeOutlinerActor(SelectedActors, SelectedInstances);
        }
        const FMHCompositeOutlinerFreshness NextFreshness = NextActor != nullptr
            ? FMHCompositeOutlinerFreshness::Capture(*NextActor)
            : FMHCompositeOutlinerFreshness();
        const bool bNeedsRebuild =
            RefreshState.NeedsRebuild(NextActor, NextFreshness);
        if (CurrentActor.Get() != NextActor)
        {
            CurrentActor = NextActor;
        }
        const UMHCompositeEditSession* NextSession = SessionOf(CurrentActor.Get());
        if (!bNeedsRebuild && ObservedSession.Get() == NextSession) return;
        RefreshModel();
    }

    void RefreshModel()
    {
        TGuardValue<bool> RefreshGuard(bRefreshingModel, true);
        ObserveSession(const_cast<UMHCompositeEditSession*>(SessionOf(CurrentActor.Get())));
        const FString PreviousPath = SelectedItem.IsValid() ? SelectedItem->NodePath : FString();
        // R6-UX1: a refresh never collapses what the user opened.
        TArray<FString> ExpandedPaths;
        if (TreeView.IsValid())
        {
            TSet<TSharedPtr<FMHCompositeOutlinerItem>> Expanded;
            TreeView->GetExpandedItems(Expanded);
            for (const TSharedPtr<FMHCompositeOutlinerItem>& Item : Expanded)
            {
                if (Item.IsValid()) ExpandedPaths.Add(Item->NodePath);
            }
        }
        RowScopePath.Reset();
        SelectedItem.Reset();
        RootItems.Reset();
        const bool bBuilt = CurrentActor.IsValid() && Model.BuildFromActor(*CurrentActor);
        const FMHCompositeOutlinerFreshness BuiltFreshness = CurrentActor.IsValid()
            ? FMHCompositeOutlinerFreshness::Capture(*CurrentActor)
            : FMHCompositeOutlinerFreshness();
        const bool bFreshBuild = bBuilt && CurrentActor.IsValid() &&
            CurrentActor->GetResolvedPlan() != nullptr;
        RefreshState.RecordRebuild(CurrentActor.Get(), BuiltFreshness, bFreshBuild);
        if (!bBuilt)
        {
            if (HeaderText.IsValid()) HeaderText->SetText(LOCTEXT("NoCompositeSelected", "Select one MH Composite actor"));
            if (StatusText.IsValid())
            {
                StatusText->SetText(LOCTEXT("NoOverlay", "Open a composite edit session to work with its nodes"));
                StatusText->SetColorAndOpacity(FSlateColor::UseSubduedForeground());
            }
        }
        else
        {
            RootItems = Model.GetRoots();
            const UMHCompositeAsset* Asset = CurrentActor->GetCompositeAsset();
            if (HeaderText.IsValid()) HeaderText->SetText(FText::FromString(FString::Printf(
                TEXT("%s  |  Layout Seed %d  |  Appearance Seed %d"),
                Asset != nullptr ? *Asset->LogicalName : TEXT("<missing>"),
                CurrentActor->GetSeed(), CurrentActor->GetAppearanceSeed())));
            if (StatusText.IsValid())
            {
                const UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
                const FMHCompositeEditContext EditContext = Subsystem != nullptr ? Subsystem->GetEditContext() : FMHCompositeEditContext();
                if (CurrentActor->IsPreviewLoading())
                {
                    StatusText->SetText(LOCTEXT("OverlayLoading", "Loading selected meshes…"));
                    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.2f)));
                }
                else if (!EditContext.EditedLogicalName.IsEmpty() && EditContext.RootPlacement.Get() == CurrentActor.Get())
                {
                    // R6-D0: what is edited, where it sits, and what saving touches.
                    RowScopePath = EditContext.InvocationPath;
                    const FString Context = EditContext.InvocationPath.IsEmpty()
                        ? Asset != nullptr ? Asset->LogicalName : FString()
                        : FString::Printf(TEXT("%s -> %s"), Asset != nullptr ? *Asset->LogicalName : TEXT("<missing>"), *EditContext.InvocationPath);
                    // CE-3b: under the mode Save and Cancel live in the viewport overlay.
                    const TCHAR* Hint = UMHCompositeEditorMode::IsActive()
                        ? TEXT("Click a node row or its geometry in the viewport to grab it; Save / Cancel are in the viewport; Esc cancels the whole session immediately; right-click for Save As Unique Copy")
                        : TEXT("Click a node row or its sprite in the viewport to grab its handle; Enter applies, Esc discards; right-click for Apply Shared Definition, Save As Unique Copy, Cancel Edit Contents");
                    StatusText->SetText(FText::FromString(FString::Printf(
                        TEXT("Editing: %s  |  Context: %s  |  Saves: shared definition (%d placement%s)  |  %s"),
                        *EditContext.EditedLogicalName, *Context, EditContext.ConsumerPlacements, EditContext.ConsumerPlacements == 1 ? TEXT("") : TEXT("s"), Hint)));
                    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.2f)));
                }
                else if (Model.GetOverlayStatus().IsEmpty())
                {
                    StatusText->SetText(LOCTEXT("OverlayActive", "Resolved overlay active"));
                    StatusText->SetColorAndOpacity(FSlateColor::UseSubduedForeground());
                }
                else
                {
                    StatusText->SetText(FText::FromString(Model.GetOverlayStatus()));
                    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.18f, 0.12f)));
                }
            }
            if (!PreviousPath.IsEmpty()) SelectedItem = Model.FindByNodePath(PreviousPath);
            if (TreeView.IsValid())
            {
                // Parents first: a nested row exists only once its composite row is expanded.
                ExpandedPaths.Sort([](const FString& A, const FString& B) { return A.Len() < B.Len(); });
                for (const FString& Path : ExpandedPaths)
                {
                    if (TSharedPtr<FMHCompositeOutlinerItem> Item = Model.FindByNodePath(Path))
                    {
                        if (Item->IsCompositeReference() && !Item->bNestedChildrenLoaded) Model.ExpandItem(Item);
                        TreeView->SetItemExpansion(Item, true);
                    }
                }
            }
        }
        if (TreeView.IsValid())
        {
            TreeView->RequestTreeRefresh();
            if (UMHCompositeEditorMode::IsActive() && SessionOf(CurrentActor.Get()) != nullptr)
            {
                SyncTreeSelectionFromSession();
            }
            else if (SelectedItem.IsValid()) RevealItem(SelectedItem);
            else TreeView->ClearSelection();
        }
        RebuildDetails();
        EditorSelectionChanged(nullptr);
    }

    FMHCompositeOutlinerModel Model;
    FMHCompositeOutlinerRefreshState RefreshState;
    TWeakObjectPtr<AMHCompositeActor> CurrentActor;
    TArray<TSharedPtr<FMHCompositeOutlinerItem>> RootItems;
    TSharedPtr<FMHCompositeOutlinerItem> SelectedItem;
    /** R6-UX1: invocation path of the session edited on CurrentActor (rows outside it are dimmed). */
    FString RowScopePath;
    TSharedPtr<STreeView<TSharedPtr<FMHCompositeOutlinerItem>>> TreeView;
    TSharedPtr<SVerticalBox> DetailsBox;
    TSharedPtr<STextBlock> HeaderText;
    TSharedPtr<STextBlock> StatusText;
    FDelegateHandle ActorSelectionHandle;
    FDelegateHandle ComponentsEditedHandle;
    FDelegateHandle SelectionChangedHandle;
    FDelegateHandle SelectObjectHandle;
    FDelegateHandle TypedSelectionChangedHandle;
    TWeakObjectPtr<UMHCompositeEditSession> ObservedSession;
    FDelegateHandle SessionSelectionChangedHandle;
    FDelegateHandle SessionChangedHandle;
    bool bRefreshPending = false;
    bool bRefreshingModel = false;
    bool bSyncingTreeSelection = false;
};

} // namespace

TSharedRef<SWidget> MHCreateCompositeOutlinerWidget()
{
    return SNew(SMHCompositeOutliner);
}

} // namespace UE::MimirComposite

#undef LOCTEXT_NAMESPACE
