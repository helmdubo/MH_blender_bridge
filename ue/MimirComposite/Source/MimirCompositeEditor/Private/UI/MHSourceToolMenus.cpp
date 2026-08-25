#include "UI/MHSourceToolMenus.h"

#include "AssetRegistry/AssetData.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "ContentBrowserMenuContexts.h"
#include "Diagnostics/MHSourceOperations.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Index/MHProjectResourceIndex.h"
#include "Logging/MessageLog.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MHSourceToolMenus"

namespace
{

using namespace UE::MimirComposite;

FString SourceRoot()
{
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    return Settings != nullptr ? Settings->GetSourceRootPath() : FString();
}

void AddDiagnostic(FMessageLog& Log, const FString& Text)
{
    if (Text.StartsWith(TEXT("MH_E_")))
    {
        Log.Error(FText::FromString(Text));
    }
    else if (Text.StartsWith(TEXT("MH_W_")))
    {
        Log.Warning(FText::FromString(Text));
    }
    else
    {
        Log.Info(FText::FromString(Text));
    }
}

void NotifyOperation(
    const FText& Page,
    const FText& Summary,
    const TArray<FString>& Warnings,
    const FString& Error)
{
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(Page);
    for (const FString& Warning : Warnings)
    {
        Log.Warning(FText::FromString(Warning));
    }
    if (!Error.IsEmpty())
    {
        Log.Error(FText::FromString(Error));
    }
    else
    {
        Log.Info(Summary);
    }
    Log.Notify(Summary, Error.IsEmpty() ? EMessageSeverity::Info : EMessageSeverity::Error, true);
}

TArray<AActor*> SelectedLevelActors()
{
    TArray<AActor*> Result;
    if (GEditor == nullptr || GEditor->GetSelectedActors() == nullptr)
    {
        return Result;
    }
    for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
    {
        if (AActor* Actor = Cast<AActor>(*It))
        {
            Result.Add(Actor);
        }
    }
    return Result;
}

TArray<AMHCompositeActor*> SelectedCompositeActors()
{
    TArray<AMHCompositeActor*> Result;
    for (AActor* Actor : SelectedLevelActors())
    {
        if (AMHCompositeActor* CompositeActor = Cast<AMHCompositeActor>(Actor))
        {
            Result.Add(CompositeActor);
        }
    }
    return Result;
}

UMHCompositeLevelSubsystem* LevelSubsystem()
{
    return GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>()
        : nullptr;
}

UMHSourceImporter* SourceImporter()
{
    return GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>()
        : nullptr;
}

bool PromptCompositeAdoptTarget(
    FMHCompositeAdoptTarget& OutTarget,
    const FString& SuggestedName,
    const FText& WindowTitle,
    const FText& AcceptLabel)
{
    OutTarget = FMHCompositeAdoptTarget();
    const FString Root = SourceRoot();
    if (Root.IsEmpty() || GEditor == nullptr)
    {
        return false;
    }

    TSharedPtr<SEditableTextBox> FolderBox;
    TSharedPtr<SEditableTextBox> NameBox;
    bool bAccepted = false;
    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(WindowTitle)
        .ClientSize(FVector2D(520.0, 180.0))
        .SupportsMinimize(false)
        .SupportsMaximize(false)
        [
            SNew(SBorder)
            .Padding(12.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock).Text(FText::Format(
                        LOCTEXT("CompositeSourceRoot", "Source root: {0}"),
                        FText::FromString(Root)))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
                [
                    SNew(STextBlock).Text(LOCTEXT(
                        "CompositeFolderLabel",
                        "Folder under source_root (empty = root)"))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SAssignNew(FolderBox, SEditableTextBox)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock).Text(LOCTEXT(
                        "CompositeNameLabel",
                        "Logical name [a-z0-9_]+"))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SAssignNew(NameBox, SEditableTextBox)
                    .Text(FText::FromString(SuggestedName))
                ]
                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0.0f, 12.0f, 0.0f, 0.0f)
                [
                    SNew(SUniformGridPanel).SlotPadding(FMargin(4.0f, 0.0f))
                    + SUniformGridPanel::Slot(0, 0)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("BuildCompositeCancel", "Cancel"))
                        .OnClicked_Lambda([&Window]()
                        {
                            Window->RequestDestroyWindow();
                            return FReply::Handled();
                        })
                    ]
                    + SUniformGridPanel::Slot(1, 0)
                    [
                        SNew(SButton)
                        .Text(AcceptLabel)
                        .IsEnabled_Lambda([NameBox]()
                        {
                            return NameBox.IsValid() && !NameBox->GetText().IsEmpty();
                        })
                        .OnClicked_Lambda([&Window, &bAccepted]()
                        {
                            bAccepted = true;
                            Window->RequestDestroyWindow();
                            return FReply::Handled();
                        })
                    ]
                ]
            ]
        ];
    GEditor->EditorAddModalWindow(Window);
    if (!bAccepted || !FolderBox.IsValid() || !NameBox.IsValid())
    {
        return false;
    }
    OutTarget.Folder = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(Root, FolderBox->GetText().ToString()));
    OutTarget.LogicalName = NameBox->GetText().ToString();
    return true;
}

void ExecuteScanProject(const FToolMenuContext&)
{
    FMHSourceAnalysis Analysis;
    FString Error;
    const bool bOk = MHScanSourcesOperation(SourceRoot(), Analysis, Error);
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(LOCTEXT("ScanProjectPage", "Scan MH project"));
    for (const FString& Warning : Analysis.Warnings) AddDiagnostic(Log, Warning);
    for (const FString& EntryError : Analysis.Errors) AddDiagnostic(Log, EntryError);
    if (!Error.IsEmpty()) AddDiagnostic(Log, Error);
    Log.Info(FText::Format(
        LOCTEXT("ScanProjectResult", "Scanned {0} resources; {1} errors."),
        FText::AsNumber(Analysis.Entries.Num()),
        FText::AsNumber(Analysis.Errors.Num())));
    Log.Notify(
        bOk && !Analysis.HasErrors()
            ? LOCTEXT("ScanProjectOk", "MH project scan completed")
            : LOCTEXT("ScanProjectErrors", "MH project scan found errors"),
        bOk && !Analysis.HasErrors() ? EMessageSeverity::Info : EMessageSeverity::Error,
        true);
}

void ExecuteImportChanged(const FToolMenuContext&)
{
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    UMHSourceImporter* Importer = SourceImporter();
    const bool bOk = Importer != nullptr && Importer->ImportSources(
        FMHImportSourcesScope::All(), Analysis, bExecuted);
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(LOCTEXT("ImportChangedPage", "Import changed MH sources"));
    for (const FString& Warning : Analysis.Warnings) AddDiagnostic(Log, Warning);
    for (const FString& Error : Analysis.Errors) AddDiagnostic(Log, Error);
    Log.Info(FText::Format(
        LOCTEXT("ImportChangedResult", "Plan: {0} resources; mutation executed: {1}."),
        FText::AsNumber(Analysis.Entries.Num()),
        bExecuted ? LOCTEXT("Yes", "yes") : LOCTEXT("No", "no")));
    Log.Notify(
        bOk && !Analysis.HasErrors()
            ? LOCTEXT("ImportChangedOk", "MH changed sources imported")
            : LOCTEXT("ImportChangedErrors", "MH source import failed"),
        bOk && !Analysis.HasErrors() ? EMessageSeverity::Info : EMessageSeverity::Error,
        true);
}

void ExecuteBuildComposite(const FToolMenuContext&)
{
    const TArray<AActor*> Actors = SelectedLevelActors();
    FString SuggestedName;
    if (Actors.Num() == 1 && MHIsCanonicalCompositeToken(Actors[0]->GetActorLabel()))
    {
        SuggestedName = Actors[0]->GetActorLabel();
    }
    FMHCompositeAdoptTarget Target;
    if (Actors.IsEmpty() || !PromptCompositeAdoptTarget(
            Target,
            SuggestedName,
            LOCTEXT("BuildCompositeTargetTitle", "Build MH Composite"),
            LOCTEXT("BuildCompositeAccept", "Build")))
    {
        if (Actors.IsEmpty())
        {
            NotifyOperation(
                LOCTEXT("BuildCompositePage", "Build MH Composite"),
                LOCTEXT("BuildCompositeFailed", "Build Composite failed"),
                {},
                TEXT("MH_E_UNREPRESENTABLE_SCENE_OBJECT: select one or more level actors"));
        }
        return;
    }
    TArray<FString> Warnings;
    FString Error;
    AMHCompositeActor* Result = nullptr;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Subsystem == nullptr || !Subsystem->BuildComposite(Actors, Target, Result, Warnings, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_IMPORT_THREAD_INVALID: composite level subsystem is unavailable");
    }
    NotifyOperation(
        LOCTEXT("BuildCompositePage", "Build MH Composite"),
        Result != nullptr
            ? FText::Format(LOCTEXT("BuiltComposite", "Built composite {0}"), FText::FromString(Result->GetActorLabel()))
            : LOCTEXT("BuildCompositeFailed", "Build Composite failed"),
        Warnings,
        Error);
}

void ExecuteBreakComposite(const FToolMenuContext&)
{
    TArray<AActor*> Spawned;
    TArray<FString> Warnings;
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Subsystem == nullptr || !Subsystem->BreakComposites(
            SelectedCompositeActors(), Spawned, Warnings, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select one or more MH Composite actors");
    }
    NotifyOperation(
        LOCTEXT("BreakCompositePage", "Break MH Composite"),
        FText::Format(LOCTEXT("BrokenComposite", "Created {0} level actors"), FText::AsNumber(Spawned.Num())),
        Warnings,
        Error);
}

void ExecuteBeginEditComposite(const FToolMenuContext&)
{
    const TArray<AMHCompositeActor*> Actors = SelectedCompositeActors();
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Actors.Num() != 1 || Subsystem == nullptr || !Subsystem->BeginEditComposite(Actors[0], Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select exactly one MH Composite actor");
    }
    NotifyOperation(
        LOCTEXT("EditCompositePage", "Edit MH Composite"),
        LOCTEXT("EditCompositeStarted", "Top-level placement transforms are now editable"),
        {},
        Error);
}

void ExecuteCommitEditComposite(const FToolMenuContext&)
{
    TArray<FString> Warnings;
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Subsystem == nullptr || !Subsystem->CommitEditComposite(Warnings, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no composite edit session is active");
    }
    NotifyOperation(
        LOCTEXT("CommitCompositePage", "Commit MH Composite Edit"),
        LOCTEXT("CommitCompositeOk", "Composite edit published and instances rebuilt"),
        Warnings,
        Error);
}

void ExecuteCancelEditComposite(const FToolMenuContext&)
{
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Subsystem == nullptr || !Subsystem->CancelEditComposite(Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no composite edit session is active");
    }
    NotifyOperation(
        LOCTEXT("CancelCompositePage", "Cancel MH Composite Edit"),
        LOCTEXT("CancelCompositeOk", "Composite edit discarded"),
        {},
        Error);
}

void ExecuteRebuildSelected(const FToolMenuContext&)
{
    TArray<FString> Warnings;
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Subsystem == nullptr || !Subsystem->RebuildComposites(SelectedCompositeActors(), Warnings, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select one or more MH Composite actors");
    }
    NotifyOperation(
        LOCTEXT("RebuildCompositePage", "Rebuild MH Composite"),
        LOCTEXT("RebuildCompositeOk", "Selected composite instances rebuilt"),
        Warnings,
        Error);
}

enum class EMHDiagnosticView : uint8
{
    Duplicates,
    BrokenReferences,
    Orphans
};

void ExecuteDiagnosticView(const EMHDiagnosticView View)
{
    FMHSourceAnalysisServices Services;
    FString Error;
    FMessageLog Log(TEXT("Mimir"));
    const FText Page = View == EMHDiagnosticView::Duplicates
        ? LOCTEXT("DuplicatesPage", "MH duplicates")
        : View == EMHDiagnosticView::BrokenReferences
            ? LOCTEXT("BrokenReferencesPage", "MH broken references")
            : LOCTEXT("OrphansPage", "MH orphaned assets");
    Log.NewPage(Page);
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot(), Services, Error) || !Services.Index.IsValid())
    {
        AddDiagnostic(Log, Error);
        Log.Notify(Page, EMessageSeverity::Error, true);
        return;
    }

    int32 Count = 0;
    if (View == EMHDiagnosticView::Orphans)
    {
        TArray<FMHProjectIndexGeneratedAssetState> Assets;
        if (!Services.Index->GetAllGeneratedAssets(Assets, Error))
        {
            AddDiagnostic(Log, Error);
        }
        for (const FMHProjectIndexGeneratedAssetState& Asset : Assets)
        {
            if (Asset.Status == EMHGeneratedAssetStatus::Orphan)
            {
                ++Count;
                Log.Warning(FText::FromString(FString::Printf(
                    TEXT("orphan %s:%s -> %s"),
                    *Asset.KindLabel,
                    *Asset.LogicalName,
                    *Asset.UEObjectPath)));
            }
        }
    }
    else
    {
        TArray<FMHProjectIndexDiagnostic> Diagnostics;
        if (!Services.Index->GetDiagnostics(Diagnostics, Error))
        {
            AddDiagnostic(Log, Error);
        }
        for (const FMHProjectIndexDiagnostic& Diagnostic : Diagnostics)
        {
            const bool bDuplicate = Diagnostic.Code.Contains(TEXT("AMBIGUOUS")) ||
                Diagnostic.Code.Contains(TEXT("DUPLICATE"));
            const bool bBroken = !bDuplicate && (!Diagnostic.TargetKind.IsEmpty() ||
                Diagnostic.Code.Contains(TEXT("UNRESOLVED")) ||
                Diagnostic.Code.Contains(TEXT("BLOCKED")));
            if ((View == EMHDiagnosticView::Duplicates && !bDuplicate) ||
                (View == EMHDiagnosticView::BrokenReferences && !bBroken))
            {
                continue;
            }
            ++Count;
            const FString Text = FString::Printf(
                TEXT("%s: %s:%s%s%s%s - %s"),
                *Diagnostic.Code,
                *Diagnostic.OwnerKind,
                *Diagnostic.OwnerName,
                Diagnostic.TargetKind.IsEmpty() ? TEXT("") : TEXT(" -> "),
                *Diagnostic.TargetKind,
                *Diagnostic.TargetName,
                *Diagnostic.Message);
            if (Diagnostic.Severity == EMHProjectDiagnosticSeverity::Error)
            {
                Log.Error(FText::FromString(Text));
            }
            else
            {
                Log.Warning(FText::FromString(Text));
            }
        }
    }
    if (!Error.IsEmpty()) AddDiagnostic(Log, Error);
    Log.Info(FText::Format(LOCTEXT("DiagnosticViewCount", "Found {0} matching rows."), FText::AsNumber(Count)));
    Log.Notify(Page, Error.IsEmpty() ? EMessageSeverity::Info : EMessageSeverity::Error, true);
}

void ExecuteShowDuplicates(const FToolMenuContext&) { ExecuteDiagnosticView(EMHDiagnosticView::Duplicates); }
void ExecuteFindBrokenReferences(const FToolMenuContext&) { ExecuteDiagnosticView(EMHDiagnosticView::BrokenReferences); }
void ExecuteFindOrphans(const FToolMenuContext&) { ExecuteDiagnosticView(EMHDiagnosticView::Orphans); }

void ExecutePublishMaterials(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    TArray<FString> AllWarnings;
    FString Error;
    int32 Count = 0;
    if (Context != nullptr && SourceImporter() != nullptr)
    {
        for (UMaterialInstanceConstant* Material : Context->LoadSelectedObjects<UMaterialInstanceConstant>())
        {
            TArray<FString> Warnings;
            FString ItemError;
            if (!SourceImporter()->PublishMaterialInteractive(Material, Warnings, ItemError))
            {
                Error += FString::Printf(TEXT("%s: %s\n"), *Material->GetPathName(), *ItemError);
            }
            else
            {
                ++Count;
            }
            AllWarnings.Append(Warnings);
        }
    }
    NotifyOperation(
        LOCTEXT("PublishMaterialPage", "Publish MH Materials"),
        FText::Format(LOCTEXT("PublishedMaterialCount", "Published {0} materials"), FText::AsNumber(Count)),
        AllWarnings,
        Error);
}

void ExecutePublishComposites(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    TArray<FString> AllWarnings;
    FString Error;
    int32 Count = 0;
    if (Context != nullptr && SourceImporter() != nullptr)
    {
        for (UMHCompositeAsset* Asset : Context->LoadSelectedObjects<UMHCompositeAsset>())
        {
            TArray<FString> Warnings;
            FString ItemError;
            FString AdoptFolder;
            FString AdoptName;
            if (Asset != nullptr && Asset->SourceRelativePath.IsEmpty())
            {
                FMHCompositeAdoptTarget Target;
                const FString SuggestedName = MHIsCanonicalCompositeToken(Asset->GetName())
                    ? Asset->GetName()
                    : FString();
                if (!PromptCompositeAdoptTarget(
                        Target,
                        SuggestedName,
                        LOCTEXT("PublishCompositeTargetTitle", "Publish MH Composite"),
                        LOCTEXT("PublishCompositeAccept", "Publish")))
                {
                    continue;
                }
                AdoptFolder = MoveTemp(Target.Folder);
                AdoptName = MoveTemp(Target.LogicalName);
            }
            if (!SourceImporter()->PublishComposite(
                    Asset,
                    AdoptFolder,
                    AdoptName,
                    Warnings,
                    ItemError))
            {
                Error += FString::Printf(TEXT("%s: %s\n"), *Asset->GetPathName(), *ItemError);
            }
            else
            {
                ++Count;
            }
            AllWarnings.Append(Warnings);
        }
    }
    NotifyOperation(
        LOCTEXT("PublishCompositePage", "Publish MH Composites"),
        FText::Format(LOCTEXT("PublishedCompositeCount", "Published {0} composites"), FText::AsNumber(Count)),
        AllWarnings,
        Error);
}

void ExecuteRebuildAllCompositeAssets(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    TArray<FString> Warnings;
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Context != nullptr && Subsystem != nullptr)
    {
        for (UMHCompositeAsset* Asset : Context->LoadSelectedObjects<UMHCompositeAsset>())
        {
            TArray<FString> ItemWarnings;
            FString ItemError;
            if (!Subsystem->RebuildAllInstances(Asset, ItemWarnings, ItemError))
            {
                Error += FString::Printf(TEXT("%s: %s\n"), *Asset->GetPathName(), *ItemError);
            }
            Warnings.Append(ItemWarnings);
        }
    }
    NotifyOperation(
        LOCTEXT("RebuildAllCompositePage", "Rebuild MH Composite Instances"),
        LOCTEXT("RebuildAllCompositeOk", "Loaded composite instances rebuilt"),
        Warnings,
        Error);
}

void ExecuteDeleteCompositeResources(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    if (Context == nullptr)
    {
        return;
    }
    const EAppReturnType::Type Choice = FMessageDialog::Open(
        EAppMsgType::YesNoCancel,
        LOCTEXT(
            "DeleteCompositePrompt",
            "Break loaded level instances before deleting the source file and generated asset?\n\n"
            "Yes: Break loaded instances, then delete.\n"
            "No: Delete and leave unresolved placeholders.\n"
            "Cancel: Do nothing."));
    if (Choice == EAppReturnType::Cancel)
    {
        return;
    }
    TArray<FString> Warnings;
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Subsystem != nullptr)
    {
        for (UMHCompositeAsset* Asset : Context->LoadSelectedObjects<UMHCompositeAsset>())
        {
            TArray<FString> ItemWarnings;
            FString ItemError;
            if (!Subsystem->DeleteCompositeResource(
                    Asset,
                    Choice == EAppReturnType::Yes,
                    ItemWarnings,
                    ItemError))
            {
                Error += FString::Printf(TEXT("%s: %s\n"), *Asset->GetPathName(), *ItemError);
            }
            Warnings.Append(ItemWarnings);
        }
    }
    NotifyOperation(
        LOCTEXT("DeleteCompositePage", "Delete MH Composite Resource"),
        LOCTEXT("DeleteCompositeOk", "Composite resources deleted"),
        Warnings,
        Error);
}

void AddLevelAction(
    FToolMenuSection& Section,
    const FName Name,
    const FText& Label,
    const FText& Tooltip,
    const FToolMenuExecuteAction& Execute)
{
    FToolUIAction Action;
    Action.ExecuteAction = Execute;
    Section.AddMenuEntry(Name, Label, Tooltip, FSlateIcon(), Action);
}

} // namespace

void MHRegisterS6ToolMenus()
{
    if (UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
    {
        FToolMenuSection& Project = ToolsMenu->FindOrAddSection(TEXT("MHSourceProject"));
        Project.Label = LOCTEXT("MHSourceProjectSection", "MH Source Tool");
        AddLevelAction(Project, TEXT("MHScanProject"), LOCTEXT("ScanProject", "Scan Project"),
            LOCTEXT("ScanProjectTip", "Rebuild the v4 project index and report diagnostics without importing."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteScanProject));
        AddLevelAction(Project, TEXT("MHImportChanged"), LOCTEXT("ImportChanged", "Import Changed"),
            LOCTEXT("ImportChangedTip", "Import all resources whose current source state differs from their managed receipts."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteImportChanged));
        AddLevelAction(Project, TEXT("MHShowDuplicates"), LOCTEXT("ShowDuplicates", "Show Duplicates"),
            LOCTEXT("ShowDuplicatesTip", "Show ambiguous source keys and duplicate managed claims in the Mimir Message Log."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteShowDuplicates));
        AddLevelAction(Project, TEXT("MHFindBrokenReferences"), LOCTEXT("FindBrokenReferences", "Find Broken References"),
            LOCTEXT("FindBrokenReferencesTip", "Show unresolved and transitively blocked resource references."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteFindBrokenReferences));
        AddLevelAction(Project, TEXT("MHFindOrphans"), LOCTEXT("FindOrphans", "Find Orphaned Assets"),
            LOCTEXT("FindOrphansTip", "Show managed assets whose source ResourceKey is absent."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteFindOrphans));

        FToolMenuSection& Placement = ToolsMenu->FindOrAddSection(TEXT("MHCompositePlacement"));
        Placement.Label = LOCTEXT("MHCompositePlacementSection", "MH Composite Placement");
        AddLevelAction(Placement, TEXT("MHBuildComposite"), LOCTEXT("BuildComposite", "Build Composite"),
            LOCTEXT("BuildCompositeTip", "Publish selected representable actors as one composite and replace the selection."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteBuildComposite));
        AddLevelAction(Placement, TEXT("MHBreakComposite"), LOCTEXT("BreakComposite", "Break Composite"),
            LOCTEXT("BreakCompositeTip", "Replace selected MH Composite actors with one authored placement layer."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteBreakComposite));
        AddLevelAction(Placement, TEXT("MHEditComposite"), LOCTEXT("EditComposite", "Edit Composite"),
            LOCTEXT("EditCompositeTip", "Unlock only top-level placement transforms on one selected instance."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteBeginEditComposite));
        AddLevelAction(Placement, TEXT("MHCommitCompositeEdit"), LOCTEXT("CommitCompositeEdit", "Commit Composite Edit"),
            LOCTEXT("CommitCompositeEditTip", "Publish the active transform edit and rebuild every live instance."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteCommitEditComposite));
        AddLevelAction(Placement, TEXT("MHCancelCompositeEdit"), LOCTEXT("CancelCompositeEdit", "Cancel Composite Edit"),
            LOCTEXT("CancelCompositeEditTip", "Discard the active transform edit and rebuild from the managed asset."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteCancelEditComposite));
        AddLevelAction(Placement, TEXT("MHRebuildComposite"), LOCTEXT("RebuildComposite", "Rebuild Composite"),
            LOCTEXT("RebuildCompositeTip", "Recompile the selected instances from current generated resources."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteRebuildSelected));
    }

    if (UToolMenu* MaterialMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(
            UMaterialInstanceConstant::StaticClass()))
    {
        FToolMenuSection& Section = MaterialMenu->FindOrAddSection(TEXT("GetAssetActions"));
        Section.AddDynamicEntry(
            TEXT("MHPublishMaterialActions"),
            FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& DynamicSection)
            {
                FToolUIAction Action;
                Action.ExecuteAction = FToolMenuExecuteAction::CreateStatic(&ExecutePublishMaterials);
                DynamicSection.AddMenuEntry(
                    TEXT("MHPublishMaterials"),
                    LOCTEXT("PublishMaterial", "Publish Material to MH Source"),
                    LOCTEXT("PublishMaterialTip", "Full-overwrite the selected .material documents from their live Material Instances."),
                    FSlateIcon(),
                    Action);
            }));
    }

    if (UToolMenu* CompositeMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(
            UMHCompositeAsset::StaticClass()))
    {
        FToolMenuSection& Section = CompositeMenu->FindOrAddSection(TEXT("GetAssetActions"));
        Section.AddDynamicEntry(
            TEXT("MHCompositeAssetActions"),
            FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& DynamicSection)
            {
                auto Add = [&DynamicSection](
                    const FName Name,
                    const FText& Label,
                    const FText& Tooltip,
                    const FToolMenuExecuteAction& Execute)
                {
                    FToolUIAction Action;
                    Action.ExecuteAction = Execute;
                    DynamicSection.AddMenuEntry(Name, Label, Tooltip, FSlateIcon(), Action);
                };
                Add(TEXT("MHPublishComposites"), LOCTEXT("PublishComposite", "Publish Composite to MH Source"),
                    LOCTEXT("PublishCompositeTip", "Full-overwrite the selected managed .composite documents."),
                    FToolMenuExecuteAction::CreateStatic(&ExecutePublishComposites));
                Add(TEXT("MHRebuildAllCompositeInstances"), LOCTEXT("RebuildAllCompositeInstances", "Rebuild All Loaded Instances"),
                    LOCTEXT("RebuildAllCompositeInstancesTip", "Recompile every loaded level instance that references this asset."),
                    FToolMenuExecuteAction::CreateStatic(&ExecuteRebuildAllCompositeAssets));
                Add(TEXT("MHDeleteCompositeResources"), LOCTEXT("DeleteCompositeResource", "Delete MH Resource"),
                    LOCTEXT("DeleteCompositeResourceTip", "Delete both the source document and generated asset, optionally breaking loaded instances first."),
                    FToolMenuExecuteAction::CreateStatic(&ExecuteDeleteCompositeResources));
            }));
    }
}

#undef LOCTEXT_NAMESPACE
