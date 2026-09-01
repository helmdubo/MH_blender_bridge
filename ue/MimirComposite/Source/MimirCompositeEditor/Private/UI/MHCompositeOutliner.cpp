#include "UI/MHCompositeOutliner.h"

#include "Composite/MHCompositeActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorModeManager.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Elements/SMInstance/SMInstanceElementData.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
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
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STreeView.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "MHCompositeOutliner"

namespace UE::MimirComposite
{

const FName MHCompositeOutlinerTabName(TEXT("MHCompositeOutliner"));

namespace
{

bool GOutlinerRegistered = false;

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
                        .Text(LOCTEXT("NoOverlay", "Source tree and resolved overlay are read-only"))
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
                        .SelectionMode(ESelectionMode::Single)
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
        if (CurrentActor.IsValid()) CurrentActor->ReleaseResolvedDebugPlan();
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
                .ColorAndOpacity_Lambda([Item]()
                {
                    if (!Item.IsValid()) return FSlateColor::UseForeground();
                    if (Item->bMissingEndpoint) return FSlateColor(FLinearColor(1.0f, 0.18f, 0.12f));
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
        SelectedItem = Item;
        RebuildDetails();
        if (!Item.IsValid() || SelectInfo == ESelectInfo::Direct || GEditor == nullptr) return;
        USceneComponent* Component = Item->PlacementComponent.Get();
        if (!IsValid(Component)) return;
        if (Item->PlacementInstanceIndex != INDEX_NONE)
        {
            UInstancedStaticMeshComponent* Bucket =
                Cast<UInstancedStaticMeshComponent>(Component);
            UTypedElementSelectionSet* SelectionSet =
                GLevelEditorModeTools().GetEditorSelectionSet();
            if (Bucket == nullptr || SelectionSet == nullptr) return;
            const FTypedElementHandle Handle =
                UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(
                    Bucket, Item->PlacementInstanceIndex);
            if (!Handle) return;
            CurrentActor->SelectPlacementLeaf(Component, Item->PlacementInstanceIndex);
            const TArray<FTypedElementHandle> Selection{Handle};
            SelectionSet->SetSelection(Selection, FTypedElementSelectionOptions());
            GEditor->RedrawLevelEditingViewports();
            return;
        }
        USelection* Components = GEditor->GetSelectedComponents();
        if (Components == nullptr) return;
        Components->BeginBatchSelectOperation();
        Components->DeselectAll();
        GEditor->SelectComponent(Component, true, false, true);
        Components->EndBatchSelectOperation(true);
        GEditor->NoteSelectionChange();
        GEditor->RedrawLevelEditingViewports();
        CurrentActor->SelectPlacementLeaf(Component);
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
            if (Component == nullptr || Component->GetOwner() != CurrentActor.Get()) continue;
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
        AMHCompositeActor* NextActor =
            MHResolveCompositeOutlinerActor(SelectedActors, SelectedInstances);
        const FMHCompositeOutlinerFreshness NextFreshness = NextActor != nullptr
            ? FMHCompositeOutlinerFreshness::Capture(*NextActor)
            : FMHCompositeOutlinerFreshness();
        const bool bNeedsRebuild =
            RefreshState.NeedsRebuild(NextActor, NextFreshness);
        if (CurrentActor.Get() != NextActor)
        {
            if (CurrentActor.IsValid()) CurrentActor->ReleaseResolvedDebugPlan();
            CurrentActor = NextActor;
            if (CurrentActor.IsValid()) CurrentActor->RetainResolvedDebugPlan();
        }
        if (!bNeedsRebuild) return;
        RefreshModel();
    }

    void RefreshModel()
    {
        const FString PreviousPath = SelectedItem.IsValid() ? SelectedItem->NodePath : FString();
        SelectedItem.Reset();
        RootItems.Reset();
        const bool bBuilt = CurrentActor.IsValid() && Model.BuildFromActor(*CurrentActor);
        const FMHCompositeOutlinerFreshness BuiltFreshness = CurrentActor.IsValid()
            ? FMHCompositeOutlinerFreshness::Capture(*CurrentActor)
            : FMHCompositeOutlinerFreshness();
        const bool bFreshBuild = bBuilt && CurrentActor.IsValid() &&
            CurrentActor->HasResidentResolvedDebugPlan();
        RefreshState.RecordRebuild(CurrentActor.Get(), BuiltFreshness, bFreshBuild);
        if (!bBuilt)
        {
            if (HeaderText.IsValid()) HeaderText->SetText(LOCTEXT("NoCompositeSelected", "Select one MH Composite actor"));
            if (StatusText.IsValid())
            {
                StatusText->SetText(LOCTEXT("NoOverlay", "Source tree and resolved overlay are read-only"));
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
                if (Model.GetOverlayStatus().IsEmpty())
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
        }
        if (TreeView.IsValid())
        {
            TreeView->RequestTreeRefresh();
            if (SelectedItem.IsValid()) RevealItem(SelectedItem);
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
    TSharedPtr<STreeView<TSharedPtr<FMHCompositeOutlinerItem>>> TreeView;
    TSharedPtr<SVerticalBox> DetailsBox;
    TSharedPtr<STextBlock> HeaderText;
    TSharedPtr<STextBlock> StatusText;
    FDelegateHandle ActorSelectionHandle;
    FDelegateHandle ComponentsEditedHandle;
    FDelegateHandle SelectionChangedHandle;
    FDelegateHandle SelectObjectHandle;
    FDelegateHandle TypedSelectionChangedHandle;
    bool bRefreshPending = false;
};

TSharedRef<SDockTab> SpawnOutlinerTab(const FSpawnTabArgs&)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SMHCompositeOutliner)
        ];
}

} // namespace

void MHRegisterCompositeOutliner()
{
    if (GOutlinerRegistered || IsRunningCommandlet()) return;
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        MHCompositeOutlinerTabName,
        FOnSpawnTab::CreateStatic(&SpawnOutlinerTab))
        .SetDisplayName(LOCTEXT("OutlinerTabName", "MH Composite Outliner"))
        .SetTooltipText(LOCTEXT("OutlinerTabTooltip", "Inspect and navigate the selected MH Composite placement."))
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("ClassIcon.DataAsset")))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCategory());
    GOutlinerRegistered = true;
}

void MHUnregisterCompositeOutliner()
{
    if (!GOutlinerRegistered || !FSlateApplication::IsInitialized()) return;
    if (TSharedPtr<SDockTab> LiveTab =
            FGlobalTabmanager::Get()->FindExistingLiveTab(MHCompositeOutlinerTabName))
    {
        LiveTab->RequestCloseTab();
    }
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(MHCompositeOutlinerTabName);
    GOutlinerRegistered = false;
}

void MHOpenCompositeOutliner()
{
    if (GOutlinerRegistered) FGlobalTabmanager::Get()->TryInvokeTab(MHCompositeOutlinerTabName);
}

} // namespace UE::MimirComposite

#undef LOCTEXT_NAMESPACE
