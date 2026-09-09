#include "UI/MHCompositeOutliner.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "ContentBrowserModule.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor.h"
#include "Logging/MessageLog.h"
#include "Editor/EditorEngine.h"
#include "EditorModeManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "LevelEditor.h"
#include "IContentBrowserSingleton.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
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
                        .Text(LOCTEXT("NoEditSession", "No composite edit session"))
                        .Font(FAppStyle::GetFontStyle(TEXT("DetailsView.CategoryFontStyle")))
                    ]
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SAssignNew(StatusText, STextBlock)
                        .Text(LOCTEXT("NoOverlay", "Open a composite edit session to work with its nodes"))
                        .AutoWrapText(true)
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
                        [
                            SNew(SButton)
                            .Text(LOCTEXT("AddSelectedNodes", "Add Nodes"))
                            .ToolTipText(LOCTEXT("AddSelectedNodesTip", "Add every selected managed MH mesh/composite as a child of the selected authored node. With no tree selection, add roots to the current edited definition."))
                            .OnClicked(this, &SMHCompositeOutliner::AddSelectedAssetsAsNodes)
                            .IsEnabled(this, &SMHCompositeOutliner::CanAddSelectedAssetsAsNodes)
                        ]
                        + SHorizontalBox::Slot().AutoWidth()
                        [
                            SNew(SButton)
                            .Text(LOCTEXT("AddSelectedEntities", "Add Entities"))
                            .ToolTipText(LOCTEXT("AddSelectedEntitiesTip", "Add every selected managed MH mesh/composite as a weight-one content variant of the selected authored node."))
                            .OnClicked(this, &SMHCompositeOutliner::AddSelectedAssetsAsOptions)
                            .IsEnabled(this, &SMHCompositeOutliner::CanAddSelectedAssetsAsOptions)
                        ]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                        .Text(this, &SMHCompositeOutliner::GetAuthoringTargetText)
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
                    [
                        SAssignNew(NavigationBox, SHorizontalBox)
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
                        .SelectionMode(ESelectionMode::Multi)
                        .OnGenerateRow(this, &SMHCompositeOutliner::GenerateRow)
                        .OnGetChildren(this, &SMHCompositeOutliner::GetTreeChildren)
                        .OnSelectionChanged(this, &SMHCompositeOutliner::TreeSelectionChanged)
                        .OnMouseButtonDoubleClick(this, &SMHCompositeOutliner::TreeItemDoubleClicked)
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
        if (GEditor != nullptr)
        {
            ModeChangedHandle = GLevelEditorModeTools().OnEditorModeIDChanged().AddSP(
                SharedThis(this), &SMHCompositeOutliner::EditorModeChanged);
        }
        ComponentsEditedHandle = LevelEditor.OnComponentsEdited().AddSP(
            SharedThis(this), &SMHCompositeOutliner::ComponentsEdited);
        RefreshSelectedActor();
    }

    virtual ~SMHCompositeOutliner() override
    {
        ObserveSession(nullptr);
        if (GEditor != nullptr)
        {
            GLevelEditorModeTools().OnEditorModeIDChanged().Remove(ModeChangedHandle);
        }
        if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
        {
            FLevelEditorModule& LevelEditor =
                FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
            LevelEditor.OnComponentsEdited().Remove(ComponentsEditedHandle);
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
                .Text_Lambda([Item]()
                {
                    if (!Item.IsValid()) return FText::GetEmpty();
                    return FText::FromString(Item->Label);
                })
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
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("EditContentsButton", "Edit"))
                .ToolTipText(LOCTEXT("EditContentsButtonTip", "Switch the current session to this materialized composite reference. Dirty edits keep the Save / Discard / Stay policy."))
                .Visibility_Lambda([Item, WeakSelf = TWeakPtr<SMHCompositeOutliner>(SharedThis(this))]()
                {
                    const TSharedPtr<SMHCompositeOutliner> Self = WeakSelf.Pin();
                    return Self.IsValid() && Self->CanSwitchToItem(Item) ? EVisibility::Visible : EVisibility::Collapsed;
                })
                .OnClicked_Lambda([Item, WeakSelf = TWeakPtr<SMHCompositeOutliner>(SharedThis(this))]()
                {
                    if (const TSharedPtr<SMHCompositeOutliner> Self = WeakSelf.Pin()) Self->QueueSwitchEditContents(Item->NodePath);
                    return FReply::Handled();
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
        // A row selects its projection component
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
                // A variant is a distinct presentation row even though the
                // authored selection belongs to its owner node. Keep the
                // clicked variant as the primary inspector row.
                if (Item.IsValid() && Item->IsOption()) RevealItem(Item);
                return;
            }
        }
    }

    // Managed Content Browser assets use the same prevalidated atomic command
    // whether they arrive from the toolbar or drag/drop.
    TOptional<EItemDropZone> CanAcceptDrop(const FDragDropEvent& Event, const EItemDropZone Zone, TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        const TSharedPtr<FAssetDragDropOp> Op = Event.GetOperationAs<FAssetDragDropOp>();
        if (!Op.IsValid() || !Item.IsValid() ||
            Item->IsOption() || !Item->DraftNodeId.IsValid() || !UMHCompositeEditorMode::IsActive())
            return TOptional<EItemDropZone>();
        return Zone;
    }

    FReply AcceptDrop(const FDragDropEvent& Event, const EItemDropZone Zone, TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        const TSharedPtr<FAssetDragDropOp> Op = Event.GetOperationAs<FAssetDragDropOp>();
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        if (!Op.IsValid() || Session == nullptr ||
            Session->GetDraft() == nullptr || !Item.IsValid() || Item->IsOption() || !Item->DraftNodeId.IsValid())
            return FReply::Unhandled();
        TArray<UObject*> Assets;
        for (const FAssetData& AssetData : Op->GetAssets()) Assets.Add(AssetData.GetAsset());
        if (Zone == EItemDropZone::OntoItem && Item->Kind == EMHRandomSemanticKind::Random)
        {
            ExecuteAssetOptionBatch(Assets, Item, LOCTEXT("DropOptionsTransaction", "Add Composite Content Variants"));
            return FReply::Handled();
        }
        if (Zone == EItemDropZone::AboveItem || Zone == EItemDropZone::BelowItem)
        {
            const UMHCompositeEditDocument* Draft = Session->GetDraft();
            FGuid ParentId;
            int32 SiblingIndex = INDEX_NONE;
            FString Error;
            if (!MHResolveOutlinerSiblingInsertion(Item.Get(), Zone == EItemDropZone::BelowItem,
                    *Draft, ParentId, SiblingIndex, Error))
            {
                ReportAuthoringError(Error);
                return FReply::Handled();
            }
            ExecuteAssetNodeBatch(Assets, Item, false, LOCTEXT("DropSiblingNodesTransaction", "Insert Composite Nodes"),
                TOptional<FGuid>(ParentId), SiblingIndex);
        }
        else
        {
            ExecuteAssetNodeBatch(Assets, Item, false, LOCTEXT("DropNodesTransaction", "Add Composite Nodes"));
        }
        return FReply::Handled();
    }

    TSharedPtr<FMHCompositeOutlinerItem> GetTreeAuthoringTarget() const
    {
        if (!TreeView.IsValid()) return nullptr;
        const TArray<TSharedPtr<FMHCompositeOutlinerItem>> Selected = TreeView->GetSelectedItems();
        if (SelectedItem.IsValid() && Selected.Contains(SelectedItem)) return SelectedItem;
        return Selected.IsEmpty() ? nullptr : Selected[0];
    }

    static void ReportAuthoringError(const FString& Error)
    {
        if (Error.IsEmpty()) return;
        FMessageLog("Mimir").Error(FText::FromString(Error));
        FMessageLog("Mimir").Open(EMessageSeverity::Error, true);
    }

    static TArray<UObject*> GetSelectedContentBrowserAssets()
    {
        TArray<FAssetData> Selected;
        FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"))
            .Get().GetSelectedAssets(Selected);
        TArray<UObject*> Assets;
        Assets.Reserve(Selected.Num());
        for (const FAssetData& Asset : Selected) Assets.Add(Asset.GetAsset());
        return Assets;
    }

    bool CanAddSelectedAssetsAsNodes() const
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        if (Session == nullptr || Session->GetDraft() == nullptr || !UMHCompositeEditorMode::IsActive()) return false;
        const TSharedPtr<FMHCompositeOutlinerItem> Target = GetTreeAuthoringTarget();
        if (!Target.IsValid()) return true; // explicit Current Composite / Root destination
        return !Target->IsOption() && Target->DraftNodeId.IsValid() &&
            Session->GetDraft()->FindNodeIndex(Target->DraftNodeId) != INDEX_NONE;
    }

    bool CanAddSelectedAssetsAsOptions() const
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        const TSharedPtr<FMHCompositeOutlinerItem> Target = GetTreeAuthoringTarget();
        return Session != nullptr && Session->GetDraft() != nullptr && UMHCompositeEditorMode::IsActive() &&
            Target.IsValid() && !Target->IsOption() && Target->DraftNodeId.IsValid() &&
            Session->GetDraft()->FindNodeIndex(Target->DraftNodeId) != INDEX_NONE;
    }

    FText GetAuthoringTargetText() const
    {
        const TSharedPtr<FMHCompositeOutlinerItem> Target = GetTreeAuthoringTarget();
        if (!Target.IsValid()) return LOCTEXT("RootAuthoringTarget", "Destination: Current Composite / Root");
        if (Target->IsOption()) return LOCTEXT("VariantAuthoringTarget", "Destination: variant row (select its authored node)");
        if (!Target->DraftNodeId.IsValid()) return LOCTEXT("LockedAuthoringTarget", "Destination: locked context (use Edit Contents)");
        return FText::FromString(TEXT("Destination: child of ") + Target->Label);
    }

    FReply AddSelectedAssetsAsNodes()
    {
        const TSharedPtr<FMHCompositeOutlinerItem> Target = GetTreeAuthoringTarget();
        ExecuteAssetNodeBatch(GetSelectedContentBrowserAssets(), Target, !Target.IsValid(),
            LOCTEXT("AddSelectedNodesTransaction", "Add Composite Nodes"));
        return FReply::Handled();
    }

    FReply AddSelectedAssetsAsOptions()
    {
        ExecuteAssetOptionBatch(GetSelectedContentBrowserAssets(), GetTreeAuthoringTarget(),
            LOCTEXT("AddSelectedOptionsTransaction", "Add Composite Content Variants"));
        return FReply::Handled();
    }

    void ExecuteAssetNodeBatch(
        const TArray<UObject*>& Assets,
        const TSharedPtr<FMHCompositeOutlinerItem>& Target,
        const bool bExplicitRoot,
        const FText& TransactionTitle,
        const TOptional<FGuid>& ParentOverride = TOptional<FGuid>(),
        const int32 SiblingIndex = INDEX_NONE)
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        const UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
        if (Draft == nullptr) return;
        TArray<FMHOutlinerAddRequest> Described;
        FString Error;
        if (!MHDescribeOutlinerAssetAddBatch(Assets, Target.Get(), bExplicitRoot, *Draft, Described, Error))
        {
            ReportAuthoringError(Error);
            return;
        }
        const FGuid ParentId = ParentOverride.IsSet() ? ParentOverride.GetValue() : Described[0].ParentId;
        TArray<FMHCompositeNodeAdd> Requests;
        Requests.Reserve(Described.Num());
        for (const FMHOutlinerAddRequest& Source : Described)
        {
            FMHCompositeNodeAdd& Request = Requests.AddDefaulted_GetRef();
            Request.Kind = Source.Kind;
            Request.Resource = Source.Resource;
            Request.Name = Source.Name;
            Request.LocalTransform = FTransform::Identity;
        }
        CommitDraftEdit(TransactionTitle,
            [ParentId, Requests, SiblingIndex](UMHCompositeEditSession& InSession, FString& OutError)
            {
                TArray<FGuid> Added;
                return InSession.AddNodes(ParentId, Requests, Added, OutError, SiblingIndex);
            });
    }

    void ExecuteAssetOptionBatch(
        const TArray<UObject*>& Assets,
        const TSharedPtr<FMHCompositeOutlinerItem>& Target,
        const FText& TransactionTitle)
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        const UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
        if (Draft == nullptr || !Target.IsValid() || Target->IsOption() ||
            !Target->DraftNodeId.IsValid() || Draft->FindNodeIndex(Target->DraftNodeId) == INDEX_NONE)
        {
            ReportAuthoringError(TEXT("MH_E_INVALID_AUTHORING_TARGET: Add Entities needs one selected authored node"));
            return;
        }
        if (Assets.IsEmpty())
        {
            ReportAuthoringError(TEXT("MH_E_INVALID_RESOURCE_SOURCE: select one or more managed assets in the Content Browser"));
            return;
        }
        TArray<FMHCompositeOption> Options;
        Options.Reserve(Assets.Num());
        FString Error;
        for (const UObject* Asset : Assets)
        {
            FMHCompositeOption Option;
            if (!MHDescribeOutlinerAssetOption(Asset, Option, Error))
            {
                ReportAuthoringError(Error);
                return;
            }
            Options.Add(MoveTemp(Option));
        }
        const FGuid NodeId = Target->DraftNodeId;
        CommitDraftEdit(TransactionTitle,
            [NodeId, Options](UMHCompositeEditSession& InSession, FString& OutError)
            {
                return InSession.AddNodeOptions(NodeId, Options, OutError);
            });
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
        const UMHCompositeEditSession* CapturedSession = SessionOf(CurrentActor.Get());
        if (GEditor == nullptr || CapturedSession == nullptr) return;
        const FMHOutlinerCommandStamp Stamp = FMHOutlinerCommandStamp::Capture(*CapturedSession);
        TWeakPtr<SMHCompositeOutliner> Weak = StaticCastSharedRef<SMHCompositeOutliner>(AsShared());
        GEditor->GetTimerManager()->SetTimerForNextTick([Weak, Title, Command, Stamp]()
        {
            const TSharedPtr<SMHCompositeOutliner> Self = Weak.Pin();
            UMHCompositeEditSession* Session = Self.IsValid() ? const_cast<UMHCompositeEditSession*>(Self->SessionOf(Self->CurrentActor.Get())) : nullptr;
            if (Session == nullptr || !Stamp.Matches(*Session)) return;
            FString Error;
            bool bOk = false;
            {
                TGuardValue<bool> CommandGuard(Self->bExecutingCommand, true);
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

    /** Editable content controls live in the Content section that owns them. */
    TSharedRef<SWidget> BuildEditableContent(const TSharedPtr<FMHCompositeOutlinerItem>& Item, const FMHCompositeAssetNode& Node)
    {
        const FGuid Id = Item->DraftNodeId;
        TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
        const bool bTakesResource = Node.Kind != EMHCompositeNodeKind::Group && Node.Kind != EMHCompositeNodeKind::Random;
        if (bTakesResource)
        {
            AddWidgetField(Box, LOCTEXT("EditResourceLabel", "Single asset"), SNew(SEditableTextBox)
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
            double TotalWeight = 0.0;
            for (const FMHCompositeOption& Candidate : Node.Options) TotalWeight += Candidate.Weight;
            for (int32 Index = 0; Index < Node.Options.Num(); ++Index)
            {
                const FMHCompositeOption& Option = Node.Options[Index];
                const FString Label = FString::Printf(TEXT("%s  (%.2f%%)"),
                    Option.Kind == EMHCompositeOptionKind::Empty ? TEXT("-- (empty)") : *Option.Resource,
                    TotalWeight > 0.0 ? 100.0 * static_cast<double>(Option.Weight) / TotalWeight : 0.0);
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
                            .OnValueCommitted_Lambda([this, Id, Index](const float Value, ETextCommit::Type)
                            {
                                CommitDraftEdit(LOCTEXT("OptionWeightTransaction", "Change Random Option Weight"),
                                    [Id, Index, Value](UMHCompositeEditSession& InSession, FString& Error)
                                    { return InSession.SetNodeOptionWeight(Id, Index, Value, Error); });
                            })
                        ]
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("RemoveOption", "Remove"))
                        .ToolTipText(LOCTEXT("RemoveOptionTip", "Remove this content variant. Removing the final variant clears node content and preserves its transform and children."))
                        .OnClicked_Lambda([this, Id, Index]()
                        {
                            CommitDraftEdit(LOCTEXT("RemoveOptionTransaction", "Remove Content Variant"),
                                [Id, Index](UMHCompositeEditSession& InSession, FString& Error)
                                { return InSession.RemoveNodeOption(Id, Index, Error); });
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
                    .OnClicked_Lambda([this, Id]()
                    {
                        FMHCompositeOption Empty;
                        Empty.Kind = EMHCompositeOptionKind::Empty;
                        Empty.Weight = 1.0f;
                        CommitDraftEdit(LOCTEXT("AddEmptyOptionTransaction", "Add Content Variant"),
                            [Id, Empty](UMHCompositeEditSession& InSession, FString& Error)
                            {
                                const TArray<FMHCompositeOption> Added{Empty};
                                return InSession.AddNodeOptions(Id, Added, Error);
                            });
                        return FReply::Handled();
                    })
                ]
            ];
        }
        if (Node.Kind == EMHCompositeNodeKind::Group)
            AddField(Box, LOCTEXT("EmptyContentField", "Content"), TEXT("Empty"));
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
        FGuid ParentId;
        if (!MHResolveOutlinerAddParent(Item.Get(), false, *Draft, ParentId, Error))
        {
            ReportAuthoringError(Error);
            return;
        }
        {
            const FScopedTransaction Transaction(LOCTEXT("AddRandomNodeTransaction", "Add Random Composite Node"));
            Added = Session->AddRandomNode(ParentId, TEXT("random"), FTransform::Identity, {Empty}, Error);
        }
        if (!Added.IsValid() && !Error.IsEmpty()) FMessageLog("Mimir").Error(FText::FromString(Error));
        FinishDraftCommand(Added);
    }

    void AddEmptyNodeAt(const TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        UMHCompositeEditSession* Session = const_cast<UMHCompositeEditSession*>(SessionOf(CurrentActor.Get()));
        const UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
        if (Draft == nullptr) return;
        FGuid ParentId;
        FString Error;
        if (!MHResolveOutlinerAddParent(Item.Get(), false, *Draft, ParentId, Error))
        {
            ReportAuthoringError(Error);
            return;
        }
        FMHCompositeNodeAdd Request;
        Request.Kind = EMHCompositeNodeKind::Group;
        Request.Name = TEXT("empty");
        TArray<FGuid> Added;
        {
            const FScopedTransaction Transaction(LOCTEXT("AddEmptyNodeTransaction", "Add Empty Composite Node"));
            const TArray<FMHCompositeNodeAdd> Requests{Request};
            Session->AddNodes(ParentId, Requests, Added, Error);
        }
        if (Added.IsEmpty()) ReportAuthoringError(Error);
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

    /** The open edit session of this placement, if any. */
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
        if (UMHCompositeEditorMode::IsActive() && !bRefreshingModel && !bExecutingCommand && !bSwitchingScope) RefreshModel();
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

    /** Load and expand only the active occurrence; do not change selection or camera. */
    TSharedPtr<FMHCompositeOutlinerItem> PrepareEditScope(const FString& InvocationPath)
    {
        if (CurrentActor.Get() == nullptr || GEditor == nullptr) return nullptr;
        if (TSharedPtr<FMHCompositeOutlinerItem> Item = Model.FindByNodePath(InvocationPath))
        {
            if (Item->IsCompositeReference() && !Item->bNestedChildrenLoaded) Model.ExpandItem(Item);
            if (TreeView.IsValid())
            {
                for (TSharedPtr<FMHCompositeOutlinerItem> Parent = Item->Parent.Pin();
                     Parent.IsValid(); Parent = Parent->Parent.Pin())
                    TreeView->SetItemExpansion(Parent, true);
                TreeView->SetItemExpansion(Item, true);
            }
            return Item;
        }
        return nullptr;
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
                    LOCTEXT("AddEmptyNodeTip", "Add an empty child node under this authored node. Drag a managed static mesh or composite onto a row to add content, or above/below it to insert sibling nodes."),
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
            if (CanSwitchToItem(Item))
            {
                Menu.AddMenuEntry(
                    LOCTEXT("SwitchEditContents", "Edit Contents..."),
                    LOCTEXT("SwitchEditContentsTip", "Switch this session to the referenced definition. Choose Save or Discard first when the current draft has changes."),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::QueueSwitchEditContents, Item->NodePath)));
            }
            Menu.AddMenuEntry(
                LOCTEXT("CancelEditContents", "Cancel Edit Contents (Esc)"),
                LOCTEXT("CancelEditContentsTip", "Close the active edit session and discard its draft. The source is not changed."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateSP(SharedThis(this), &SMHCompositeOutliner::CancelEditContents)));
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

    bool CanSwitchToItem(const TSharedPtr<FMHCompositeOutlinerItem>& Item) const
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        if (Session == nullptr || !UMHCompositeEditorMode::IsActive() || !Item.IsValid() ||
            !Item->IsCompositeReference() || Item->NodePath.IsEmpty() || Item->bMissingEndpoint ||
            Item->NodePath == Session->GetInvocationPath()) return false;
        // A random option invokes only when it is the resolved choice. For a
        // source-node reference the plan overlay proves this occurrence exists.
        return Item->IsOption() ? Item->bSelectedOption : Item->bHasResolvedOverlay;
    }

    void TreeItemDoubleClicked(TSharedPtr<FMHCompositeOutlinerItem> Item)
    {
        if (CanSwitchToItem(Item)) QueueSwitchEditContents(Item->NodePath);
    }

    /** Switch is deferred because RequestSwitch may show Save / Discard / Stay and retarget the live session. */
    void QueueSwitchEditContents(const FString InvocationPath)
    {
        const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
        if (GEditor == nullptr || Session == nullptr || InvocationPath == Session->GetInvocationPath()) return;
        const FMHOutlinerCommandStamp Stamp = FMHOutlinerCommandStamp::Capture(*Session);
        const TWeakPtr<SMHCompositeOutliner> Weak = StaticCastSharedRef<SMHCompositeOutliner>(AsShared());
        GEditor->GetTimerManager()->SetTimerForNextTick([Weak, Stamp, InvocationPath]()
        {
            const TSharedPtr<SMHCompositeOutliner> Self = Weak.Pin();
            UMHCompositeEditSession* Current = Self.IsValid()
                ? const_cast<UMHCompositeEditSession*>(Self->SessionOf(Self->CurrentActor.Get())) : nullptr;
            UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
            if (Current == nullptr || Mode == nullptr || !Stamp.Matches(*Current)) return;
            bool bSwitched = false;
            {
                TGuardValue<bool> SwitchGuard(Self->bSwitchingScope, true);
                bSwitched = Mode->RequestSwitch(InvocationPath);
            }
            if (bSwitched) Self->RefreshModel();
        });
    }

    void RebuildNavigation()
    {
        if (!NavigationBox.IsValid()) return;
        NavigationBox->ClearChildren();
        const UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
            ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
        const FMHCompositeEditContext Context = Subsystem != nullptr
            ? Subsystem->GetEditContext() : FMHCompositeEditContext();
        const TArray<TPair<FString, FString>> Targets = UMHCompositeEditorMode::BreadcrumbTargets(Context);
        if (Targets.Num() <= 1) return;
        NavigationBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 5.0f, 0.0f)
        [
            SNew(STextBlock).Text(LOCTEXT("NavigateDefinition", "Navigate:"))
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
        ];
        for (int32 Index = 0; Index + 1 < Targets.Num(); ++Index)
        {
            const FString Label = Index == 0 ? TEXT("Root: ") + Targets[Index].Key : TEXT("Parent: ") + Targets[Index].Key;
            const FString Path = Targets[Index].Value;
            NavigationBox->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SNew(SButton)
                .Text(FText::FromString(Label))
                .ToolTipText(LOCTEXT("NavigateDefinitionTip", "Switch through the current session; camera and Game View do not change."))
                .OnClicked_Lambda([Weak = TWeakPtr<SMHCompositeOutliner>(SharedThis(this)), Path]()
                {
                    if (const TSharedPtr<SMHCompositeOutliner> Self = Weak.Pin()) Self->QueueSwitchEditContents(Path);
                    return FReply::Handled();
                })
            ];
        }
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

    static void AddWidgetField(
        const TSharedRef<SVerticalBox>& Box,
        const FText& Label,
        const TSharedRef<SWidget>& Widget)
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
        const FMHCompositeAssetNode* EditableNode = Item->DraftNodeId.IsValid() && !Item->IsOption()
            ? DraftNodeOf(*Item) : nullptr;
        TSharedRef<SVerticalBox> NodeBox = SNew(SVerticalBox);
        if (EditableNode != nullptr)
        {
            const FGuid NodeId = Item->DraftNodeId;
            AddWidgetField(NodeBox, LOCTEXT("EditNameLabel", "Name"), SNew(SEditableTextBox)
                .Text(FText::FromString(EditableNode->Name))
                .OnTextCommitted_Lambda([this, NodeId](const FText& Text, const ETextCommit::Type Commit)
                {
                    if (Commit != ETextCommit::OnEnter && Commit != ETextCommit::OnUserMovedFocus) return;
                    CommitDraftEdit(LOCTEXT("RenameNodeTransaction", "Rename Composite Node"),
                        [NodeId, Name = Text.ToString()](UMHCompositeEditSession& InSession, FString& Error)
                        { return InSession.SetNodeName(NodeId, Name, Error); });
                }));
        }
        AddField(NodeBox, LOCTEXT("KindField", "Kind"), Item->IsOption() ? TEXT("content variant") : OutlinerKindText(Item->Kind));
        NodeBox->SetToolTipText(FText::FromString(Item->NodePath));
        FString Access = TEXT("Locked context — use Edit Contents to author this definition");
        const TSharedPtr<FMHCompositeOutlinerItem> OptionOwner = Item->IsOption() ? Item->Parent.Pin() : nullptr;
        if (OptionOwner.IsValid() && OptionOwner->DraftNodeId.IsValid()) Access = TEXT("Content variant of the active authored node");
        else if (Item->DraftNodeId.IsValid()) Access = TEXT("Editable authored node");
        else if (!RowScopePath.IsEmpty() && Item->NodePath == RowScopePath) Access = TEXT("Active edited definition");
        AddField(NodeBox, LOCTEXT("AccessField", "Authoring"), Access);
        AddSection(LOCTEXT("NodeSection", "Node"), NodeBox);

        TSharedRef<SVerticalBox> TransformBox = SNew(SVerticalBox);
        if (Item->IsOption())
        {
            AddField(TransformBox, LOCTEXT("VariantTransformField", "Transform"), TEXT("Owned by the parent node"));
        }
        else
        {
            AddField(TransformBox, LOCTEXT("FixedTrsField", "Authored TRS"), Item->FixedTransform.ToHumanReadableString());
            const UMHCompositeAsset* SourceAsset = Item->SourceAsset.Get();
            const FMHCompositeAssetNode* SourceNode = EditableNode != nullptr ? EditableNode
                : SourceAsset != nullptr && SourceAsset->Nodes.IsValidIndex(Item->SourceNodeIndex)
                    ? &SourceAsset->Nodes[Item->SourceNodeIndex] : nullptr;
            const bool bProcedural = !Item->Profile.IsEmpty() || (SourceNode != nullptr && SourceNode->bHasInlinePlacement);
            const FString PlacementStatus = bProcedural
                ? TEXT("Procedural placement; ordinary transform editing is disabled")
                : TEXT("Authored placement");
            AddField(TransformBox, LOCTEXT("PlacementStatusField", "Status"), PlacementStatus);
            if (Item->PlaceType != INDEX_NONE)
                AddField(TransformBox, LOCTEXT("PlaceTypeField", "place_type"), FString::FromInt(Item->PlaceType));
            if (Item->SampledLocalTrs.IsSet())
                AddField(TransformBox, LOCTEXT("SampledTrsField", "Sampled TRS"), FormatRandomTrs(Item->SampledLocalTrs.GetValue()));
        }
        AddSection(LOCTEXT("TransformSection", "Transform / Placement"), TransformBox);

        TSharedRef<SVerticalBox> ContentBox = SNew(SVerticalBox);
        TSharedRef<SWidget> ContentWidget = ContentBox;
        if (Item->IsOption())
        {
            double Total = 0.0;
            const TSharedPtr<FMHCompositeOutlinerItem> Parent = Item->Parent.Pin();
            if (Parent.IsValid())
                for (const TSharedPtr<FMHCompositeOutlinerItem>& Child : Parent->Children)
                    if (Child.IsValid() && Child->IsOption()) Total += Child->Weight;
            AddField(ContentBox, LOCTEXT("VariantResourceField", "Variant"), Item->Resource.IsEmpty() ? TEXT("Empty") : Item->Resource);
            if (Parent.IsValid() && Parent->DraftNodeId.IsValid())
            {
                const FGuid NodeId = Parent->DraftNodeId;
                const int32 OptionIndex = Item->OptionIndex;
                AddWidgetField(ContentBox, LOCTEXT("VariantWeightField", "Weight"),
                    SNew(SSpinBox<float>)
                    .MinValue(0.0f)
                    .Value(Item->Weight)
                    .OnValueCommitted_Lambda([this, NodeId, OptionIndex](const float Value, ETextCommit::Type)
                    {
                        CommitDraftEdit(LOCTEXT("SelectedOptionWeightTransaction", "Change Content Variant Weight"),
                            [NodeId, OptionIndex, Value](UMHCompositeEditSession& InSession, FString& Error)
                            { return InSession.SetNodeOptionWeight(NodeId, OptionIndex, Value, Error); });
                    }));
                AddWidgetField(ContentBox, LOCTEXT("VariantRemoveField", "Action"),
                    SNew(SButton)
                    .Text(LOCTEXT("RemoveSelectedOption", "Remove Variant"))
                    .OnClicked_Lambda([this, NodeId, OptionIndex]()
                    {
                        CommitDraftEdit(LOCTEXT("RemoveSelectedOptionTransaction", "Remove Content Variant"),
                            [NodeId, OptionIndex](UMHCompositeEditSession& InSession, FString& Error)
                            { return InSession.RemoveNodeOption(NodeId, OptionIndex, Error); });
                        return FReply::Handled();
                    }));
            }
            else AddField(ContentBox, LOCTEXT("VariantWeightReadOnlyField", "Weight"), FString::Printf(TEXT("%.9g"), static_cast<double>(Item->Weight)));
            AddField(ContentBox, LOCTEXT("VariantChanceField", "Computed chance"), FString::Printf(TEXT("%.2f%%"), Total > 0.0 ? 100.0 * Item->Weight / Total : 0.0));
            AddField(ContentBox, LOCTEXT("VariantSelectedField", "Preview"), Item->bSelectedOption ? TEXT("Selected") : TEXT("Not selected"));
        }
        else if (EditableNode != nullptr)
        {
            ContentWidget = BuildEditableContent(Item, *EditableNode);
        }
        else if (Item->Kind == EMHRandomSemanticKind::Random)
        {
            double Total = 0.0;
            for (const TSharedPtr<FMHCompositeOutlinerItem>& Child : Item->Children)
                if (Child.IsValid() && Child->IsOption()) Total += Child->Weight;
            for (const TSharedPtr<FMHCompositeOutlinerItem>& Child : Item->Children)
            {
                if (!Child.IsValid() || !Child->IsOption()) continue;
                ContentBox->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.0f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("Variant: ") + Child->Label + (Child->bSelectedOption ? TEXT("  [selected]") : TEXT(""))))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(FString::Printf(TEXT("Weight %.9g  |  %.2f%%"),
                            static_cast<double>(Child->Weight), Total > 0.0 ? 100.0 * Child->Weight / Total : 0.0)))
                    ]
                ];
            }
        }
        else if (!Item->Resource.IsEmpty())
        {
            AddField(ContentBox, LOCTEXT("SingleContentField", "Single asset"), Item->Resource);
        }
        else
        {
            AddField(ContentBox, LOCTEXT("EmptyContentField", "Content"), TEXT("Empty"));
        }
        AddSection(LOCTEXT("ContentSection", "Content"), ContentWidget);
    }

    void EditorModeChanged(const FEditorModeID& ModeId, const bool bEnteringMode)
    {
        if (ModeId != UMHCompositeEditorMode::EM_MHCompositeEditModeId) return;
        if (bEnteringMode)
        {
            RefreshSelectedActor();
            return;
        }
        // Closing a session need not change native selection, and an empty
        // session has no selection notification to broadcast. Mode lifetime
        // owns the panel even when a caller retains its widget after Exit.
        CurrentActor.Reset();
        RefreshModel();
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
        const UMHCompositeEditSession* ActiveSession = SessionOf(CurrentActor.Get());
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
        TSharedPtr<FMHCompositeOutlinerItem> ScopeItem;
        bool bScopeChanged = false;
        SelectedItem.Reset();
        RootItems.Reset();
        const bool bBuilt = SessionOf(CurrentActor.Get()) != nullptr && Model.BuildFromActor(*CurrentActor);
        const FMHCompositeOutlinerFreshness BuiltFreshness = CurrentActor.IsValid()
            ? FMHCompositeOutlinerFreshness::Capture(*CurrentActor)
            : FMHCompositeOutlinerFreshness();
        const bool bFreshBuild = bBuilt && CurrentActor.IsValid() &&
            CurrentActor->GetResolvedPlan() != nullptr;
        RefreshState.RecordRebuild(CurrentActor.Get(), BuiltFreshness, bFreshBuild);
        if (!bBuilt)
        {
            if (HeaderText.IsValid()) HeaderText->SetText(LOCTEXT("NoEditSession", "No composite edit session"));
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
            const UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
                ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
            const FMHCompositeEditContext EditContext = Subsystem != nullptr
                ? Subsystem->GetEditContext() : FMHCompositeEditContext();
            const bool bHasEditContext = !EditContext.EditedLogicalName.IsEmpty() &&
                EditContext.RootPlacement.Get() == CurrentActor.Get();
            if (bHasEditContext)
            {
                RowScopePath = EditContext.InvocationPath;
                if (ActiveSession != nullptr)
                {
                    bScopeChanged = FocusedSessionId != ActiveSession->GetSessionId() ||
                        FocusedEpoch != ActiveSession->GetEpoch() ||
                        FocusedInvocationPath != EditContext.InvocationPath;
                }
            }
            if (HeaderText.IsValid())
            {
                HeaderText->SetText(FText::FromString(FString::Printf(
                    TEXT("Composite Edit — %s"),
                    bHasEditContext ? *EditContext.EditedLogicalName
                        : Asset != nullptr ? *Asset->LogicalName : TEXT("<missing>"))));
                HeaderText->SetToolTipText(FText::FromString(FString::Printf(
                    TEXT("Placement: %s\nLayout seed: %d\nAppearance seed: %d"),
                    Asset != nullptr ? *Asset->LogicalName : TEXT("<missing>"),
                    CurrentActor->GetSeed(), CurrentActor->GetAppearanceSeed())));
            }
            if (StatusText.IsValid())
            {
                const UMHCompositeEditSession* Session = SessionOf(CurrentActor.Get());
                if (Session != nullptr && !Session->GetPreviewError().IsEmpty())
                {
                    StatusText->SetText(LOCTEXT("PreviewError", "Preview error — authored changes are still in the draft"));
                    StatusText->SetToolTipText(FText::FromString(Session->GetPreviewError()));
                    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.18f, 0.12f)));
                }
                else if (CurrentActor->IsPreviewLoading())
                {
                    StatusText->SetText(FText::FromString(FString::Printf(TEXT("%s  •  Loading preview…"),
                        Session != nullptr && Session->IsDirty() ? TEXT("Unsaved changes") : TEXT("Saved"))));
                    StatusText->SetToolTipText(FText::GetEmpty());
                    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.2f)));
                }
                else if (bHasEditContext)
                {
                    StatusText->SetText(FText::FromString(FString::Printf(
                        TEXT("%s  •  %s"),
                        ActiveSession != nullptr && ActiveSession->IsDirty() ? TEXT("Unsaved changes") : TEXT("Saved"),
                        EditContext.InvocationPath.IsEmpty() ? TEXT("Root definition") : TEXT("Nested definition"))));
                    StatusText->SetToolTipText(FText::FromString(FString::Printf(
                        TEXT("Editing: %s\nInvocation: %s\nSaving updates the shared definition used by %d placement%s."),
                        *EditContext.EditedLogicalName,
                        EditContext.InvocationPath.IsEmpty() ? TEXT("Root") : *EditContext.InvocationPath,
                        EditContext.ConsumerPlacements,
                        EditContext.ConsumerPlacements == 1 ? TEXT("") : TEXT("s"))));
                    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.2f)));
                }
                else if (Model.GetOverlayStatus().IsEmpty())
                {
                    StatusText->SetText(LOCTEXT("OverlayActive", "Resolved overlay active"));
                    StatusText->SetToolTipText(FText::GetEmpty());
                    StatusText->SetColorAndOpacity(FSlateColor::UseSubduedForeground());
                }
                else
                {
                    StatusText->SetText(LOCTEXT("OverlayUnavailable", "Preview unavailable"));
                    StatusText->SetToolTipText(FText::FromString(Model.GetOverlayStatus()));
                    StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.18f, 0.12f)));
                }
            }
            // The nested draft rows are created only after its occurrence is
            // expanded. Do that before semantic GUID selection is mirrored.
            if (!RowScopePath.IsEmpty()) ScopeItem = PrepareEditScope(RowScopePath);
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
                if (bScopeChanged && ScopeItem.IsValid() &&
                    SessionOf(CurrentActor.Get())->GetSelectedNodeIds().IsEmpty())
                    TreeView->RequestScrollIntoView(ScopeItem);
            }
            else if (SelectedItem.IsValid()) RevealItem(SelectedItem);
            else TreeView->ClearSelection();
        }
        if (bBuilt && ActiveSession != nullptr)
        {
            FocusedSessionId = ActiveSession->GetSessionId();
            FocusedEpoch = ActiveSession->GetEpoch();
            FocusedInvocationPath = ActiveSession->GetInvocationPath();
        }
        else
        {
            FocusedSessionId.Invalidate();
            FocusedEpoch = 0;
            FocusedInvocationPath.Reset();
        }
        RebuildNavigation();
        RebuildDetails();
    }

    FMHCompositeOutlinerModel Model;
    FMHCompositeOutlinerRefreshState RefreshState;
    TWeakObjectPtr<AMHCompositeActor> CurrentActor;
    TArray<TSharedPtr<FMHCompositeOutlinerItem>> RootItems;
    TSharedPtr<FMHCompositeOutlinerItem> SelectedItem;
    /** R6-UX1: invocation path of the session edited on CurrentActor (rows outside it are dimmed). */
    FString RowScopePath;
    TSharedPtr<STreeView<TSharedPtr<FMHCompositeOutlinerItem>>> TreeView;
    TSharedPtr<SHorizontalBox> NavigationBox;
    TSharedPtr<SVerticalBox> DetailsBox;
    TSharedPtr<STextBlock> HeaderText;
    TSharedPtr<STextBlock> StatusText;
    FDelegateHandle ModeChangedHandle;
    FDelegateHandle ComponentsEditedHandle;
    TWeakObjectPtr<UMHCompositeEditSession> ObservedSession;
    FDelegateHandle SessionSelectionChangedHandle;
    FDelegateHandle SessionChangedHandle;
    FGuid FocusedSessionId;
    uint32 FocusedEpoch = 0;
    FString FocusedInvocationPath;
    bool bRefreshPending = false;
    bool bRefreshingModel = false;
    bool bExecutingCommand = false;
    bool bSwitchingScope = false;
    bool bSyncingTreeSelection = false;
};

} // namespace

TSharedRef<SWidget> MHCreateCompositeOutlinerWidget()
{
    return SNew(SMHCompositeOutliner);
}

} // namespace UE::MimirComposite

#undef LOCTEXT_NAMESPACE
