#include "UI/MHSourceToolMenus.h"

#include "AssetRegistry/AssetData.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "ContentBrowserMenuContexts.h"
#include "Diagnostics/MHSourceOperations.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDesktopPlatform.h"
#include "Index/MHProjectResourceIndex.h"
#include "LevelEditorMenuContext.h"
#include "Logging/MessageLog.h"
#include "Material/MHMaterialClipboard.h"
#include "Material/MHMaterialDocumentExport.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "UI/MHSourceOverwritePolicy.h"
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

TArray<TWeakObjectPtr<AActor>> ContextLevelActors(UToolMenu* Menu)
{
    TArray<TWeakObjectPtr<AActor>> Result;
    const ULevelEditorContextMenuContext* Context = Menu != nullptr
        ? Menu->FindContext<ULevelEditorContextMenuContext>()
        : nullptr;
    if (Context == nullptr || Context->CurrentSelection == nullptr)
    {
        return Result;
    }
    for (UObject* SelectedObject : Context->CurrentSelection->GetSelectedObjects(AActor::StaticClass()))
    {
        if (AActor* Actor = Cast<AActor>(SelectedObject))
        {
            Result.Add(Actor);
        }
    }
    return Result;
}

TArray<TWeakObjectPtr<AMHCompositeActor>> ContextCompositeActors(
    const TArray<TWeakObjectPtr<AActor>>& Actors)
{
    TArray<TWeakObjectPtr<AMHCompositeActor>> Result;
    for (const TWeakObjectPtr<AActor>& ActorPtr : Actors)
    {
        if (AMHCompositeActor* CompositeActor = Cast<AMHCompositeActor>(ActorPtr.Get()))
        {
            Result.Add(CompositeActor);
        }
    }
    return Result;
}

template <typename ActorType>
bool ResolveActorSnapshot(
    const TArray<TWeakObjectPtr<ActorType>>& Snapshot,
    TArray<ActorType*>& OutActors)
{
    OutActors.Reset(Snapshot.Num());
    for (const TWeakObjectPtr<ActorType>& ActorPtr : Snapshot)
    {
        ActorType* Actor = ActorPtr.Get();
        if (Actor == nullptr)
        {
            OutActors.Reset();
            return false;
        }
        OutActors.Add(Actor);
    }
    return !OutActors.IsEmpty();
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

void ExecuteBuildComposite(const TArray<TWeakObjectPtr<AActor>>& ActorSnapshot)
{
    TArray<AActor*> Actors;
    const bool bSelectionIsLive = ResolveActorSnapshot(ActorSnapshot, Actors);
    FString SuggestedName;
    if (Actors.Num() == 1 && MHIsCanonicalCompositeToken(Actors[0]->GetActorLabel()))
    {
        SuggestedName = Actors[0]->GetActorLabel();
    }
    FMHCompositeAdoptTarget Target;
    if (!bSelectionIsLive || !PromptCompositeAdoptTarget(
            Target,
            SuggestedName,
            LOCTEXT("BuildCompositeTargetTitle", "Build MH Composite"),
            LOCTEXT("BuildCompositeAccept", "Build")))
    {
        if (!bSelectionIsLive)
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
    FMHCompositeDocument Document;
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || !MHPreflightBuildComposite(Actors, *Settings, Document, Warnings, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_SOURCE_INDEX_INVALID: settings are unavailable");
        NotifyOperation(
            LOCTEXT("BuildCompositePage", "Build MH Composite"),
            LOCTEXT("BuildCompositeFailed", "Build Composite failed"), Warnings, Error);
        return;
    }
    if (!Warnings.IsEmpty())
        NotifyOperation(
            LOCTEXT("BuildCompositePage", "Build MH Composite"),
            LOCTEXT("BuildCompositeLostState", "Build Composite will discard unrepresentable instance state"),
            Warnings, {});
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

void ExecuteBreakComposite(const TArray<TWeakObjectPtr<AMHCompositeActor>>& ActorSnapshot)
{
    TArray<AMHCompositeActor*> Actors;
    TArray<AActor*> Spawned;
    TArray<FString> Warnings;
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (!ResolveActorSnapshot(ActorSnapshot, Actors) ||
        Subsystem == nullptr ||
        !Subsystem->BreakComposites(Actors, Spawned, Warnings, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select one or more MH Composite actors");
    }
    NotifyOperation(
        LOCTEXT("BreakCompositePage", "Break MH Composite"),
        FText::Format(LOCTEXT("BrokenComposite", "Created {0} level actors"), FText::AsNumber(Spawned.Num())),
        Warnings,
        Error);
}

void ExecuteBeginEditComposite(const TWeakObjectPtr<AMHCompositeActor> ActorSnapshot)
{
    AMHCompositeActor* Actor = ActorSnapshot.Get();
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (Actor == nullptr || Subsystem == nullptr || !Subsystem->BeginEditComposite(Actor, Error))
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
    if (Subsystem == nullptr)
    {
        Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no composite edit session is active");
    }
    else
    {
        const FString LogicalName = Subsystem->GetEditingCompositeLogicalName();
        FString SourceFile = Subsystem->GetEditingCompositeSourceRelativePath();
        if (SourceFile.IsEmpty())
        {
            SourceFile = (LogicalName.IsEmpty() ? TEXT("<unknown>") : LogicalName) + TEXT(".composite");
        }
        const FText Confirmation = FText::Format(
            LOCTEXT(
                "CommitCompositeIrreversiblePrompt",
                "This will overwrite {0}.composite. Unreal Editor Undo cannot restore the previous source file; revert with a new edit or VCS. Continue?"),
            FText::FromString(LogicalName.IsEmpty() ? TEXT("<unknown>") : LogicalName));
        const FText Audit = FText::Format(
            LOCTEXT("CommitCompositeOverwriteAudit", "{0} overwritten from edited transforms"),
            FText::FromString(SourceFile));
        const EMHSourceOverwriteExecution Execution = MHExecuteSourceOverwrite(
            SourceFile,
            Confirmation,
            Audit,
            [&Subsystem, &Warnings, &Error]()
            {
                if (!Subsystem->CommitEditComposite(Warnings, Error) && Error.IsEmpty())
                {
                    Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no composite edit session is active");
                }
                return Error.IsEmpty();
            });
        if (Execution == EMHSourceOverwriteExecution::Cancelled)
        {
            return;
        }
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

void ExecuteRebuildSelected(const TArray<TWeakObjectPtr<AMHCompositeActor>>& ActorSnapshot)
{
    TArray<AMHCompositeActor*> Actors;
    TArray<FString> Warnings;
    FString Error;
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (!ResolveActorSnapshot(ActorSnapshot, Actors) ||
        Subsystem == nullptr ||
        !Subsystem->RebuildComposites(Actors, Warnings, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select one or more MH Composite actors");
    }
    NotifyOperation(
        LOCTEXT("RebuildCompositePage", "Rebuild MH Composite"),
        LOCTEXT("RebuildCompositeOk", "Selected composite instances rebuilt"),
        Warnings,
        Error);
}

bool ResolveSeedCommandActors(
    const TArray<TWeakObjectPtr<AMHCompositeActor>>& ActorSnapshot,
    const bool bMutating,
    TArray<AMHCompositeActor*>& OutActors,
    FString& OutError)
{
    if (!ResolveActorSnapshot(ActorSnapshot, OutActors))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the complete MH Composite selection is no longer available");
        return false;
    }
    for (const AMHCompositeActor* Actor : OutActors)
    {
        if (Actor->IsTemplate() || Actor->IsActorBeingDestroyed())
        {
            OutError = FString::Printf(
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: seed command requires live placed actors: %s"),
                *Actor->GetPathName());
            return false;
        }
        if (bMutating && Actor->IsPlacementEditMode())
        {
            OutError = FString::Printf(
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel Composite Edit before changing seeds: %s"),
                *Actor->GetPathName());
            return false;
        }
    }
    if (bMutating && GEditor == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: editor is unavailable for a seed transaction");
        return false;
    }
    const UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    if (bMutating && Subsystem != nullptr && Subsystem->IsEditingComposite())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel the active Composite Edit before changing seeds");
        return false;
    }
    return true;
}

// Consume the entire clipboard string. Prefix parsers would accept an overflow,
// decimal fraction, or an otherwise unrelated clipboard value as a valid Seed.
bool ParseClipboardSeed(const FString& Text, int32& OutSeed)
{
    if (Text.IsEmpty()) return false;
    const bool bNegative = Text[0] == TEXT('-');
    int32 Index = bNegative || Text[0] == TEXT('+') ? 1 : 0;
    if (Index == Text.Len()) return false;
    const uint64 Limit = bNegative ? static_cast<uint64>(MAX_int32) + 1u : MAX_int32;
    uint64 Magnitude = 0;
    for (; Index < Text.Len(); ++Index)
    {
        const TCHAR Character = Text[Index];
        if (Character < TEXT('0') || Character > TEXT('9')) return false;
        const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
        if (Magnitude > (Limit - Digit) / 10u) return false;
        Magnitude = Magnitude * 10u + Digit;
    }
    OutSeed = static_cast<int32>(bNegative ? -static_cast<int64>(Magnitude) : static_cast<int64>(Magnitude));
    return true;
}

enum class EMHSeedCommand : uint8
{
    Reseed,
    ReseedAppearance,
    Individual,
    Equal,
    Paste,
    Lock,
    Unlock,
    KeepOnDuplicate
};

FText SeedCommandTitle(const EMHSeedCommand Command)
{
    switch (Command)
    {
    case EMHSeedCommand::Reseed: return LOCTEXT("ReseedCompositePage", "Reseed MH Composite");
    case EMHSeedCommand::ReseedAppearance: return LOCTEXT("ReseedAppearancePage", "Reseed MH Composite Appearance");
    case EMHSeedCommand::Individual: return LOCTEXT("IndividualSeedPage", "Randomize Selected: Individual");
    case EMHSeedCommand::Equal: return LOCTEXT("EqualSeedPage", "Randomize Selected: Equal");
    case EMHSeedCommand::Paste: return LOCTEXT("PasteSeedPage", "Paste MH Composite Seed");
    case EMHSeedCommand::Lock: return LOCTEXT("LockSeedPage", "Lock MH Composite Seed");
    case EMHSeedCommand::Unlock: return LOCTEXT("UnlockSeedPage", "Unlock MH Composite Seed");
    case EMHSeedCommand::KeepOnDuplicate: return LOCTEXT("KeepSeedPage", "Keep Seed on Duplicate");
    }
    return FText::GetEmpty();
}

void ExecuteSeedCommand(
    const TArray<TWeakObjectPtr<AMHCompositeActor>>& ActorSnapshot,
    const EMHSeedCommand Command)
{
    const FText Page = SeedCommandTitle(Command);
    TArray<AMHCompositeActor*> Actors;
    FString Error;
    if (!ResolveSeedCommandActors(ActorSnapshot, true, Actors, Error))
    {
        NotifyOperation(Page, LOCTEXT("SeedCommandFailed", "MH seed command failed"), {}, Error);
        return;
    }

    TArray<int32> NewSeeds;
    NewSeeds.Reserve(Actors.Num());
    if (Command == EMHSeedCommand::Paste)
    {
        FString Clipboard;
        int32 ClipboardSeed = 0;
        FPlatformApplicationMisc::ClipboardPaste(Clipboard);
        if (Actors.Num() != 1 || !ParseClipboardSeed(Clipboard, ClipboardSeed))
        {
            Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Paste Seed requires one MH Composite actor and a complete decimal int32 in the clipboard");
            NotifyOperation(Page, LOCTEXT("SeedCommandFailed", "MH seed command failed"), {}, Error);
            return;
        }
        NewSeeds.Add(ClipboardSeed);
    }
    else if (Command == EMHSeedCommand::Individual)
    {
        for (const AMHCompositeActor* Actor : Actors)
        {
            int32 NewSeed;
            do
            {
                NewSeed = AMHCompositeActor::GenerateAutoSeed(Actor->GetSeed());
            }
            while (NewSeeds.Contains(NewSeed));
            NewSeeds.Add(NewSeed);
        }
    }
    else if (Command == EMHSeedCommand::Equal)
    {
        TArray<int32> PreviousSeeds;
        for (const AMHCompositeActor* Actor : Actors) PreviousSeeds.Add(Actor->GetSeed());
        int32 NewSeed;
        do
        {
            NewSeed = AMHCompositeActor::GenerateAutoSeed();
        }
        while (PreviousSeeds.Contains(NewSeed));
        NewSeeds.Init(NewSeed, Actors.Num());
    }

    // All selection/clipboard validation and seed allocation precedes the first
    // Modify. One command is one undo step, including the multi-actor variants.
    {
        const FScopedTransaction Transaction(Page);
        for (int32 Index = 0; Index < Actors.Num(); ++Index)
        {
            AMHCompositeActor* Actor = Actors[Index];
            Actor->Modify();
            switch (Command)
            {
            case EMHSeedCommand::Reseed:
                Actor->Reseed();
                break;
            case EMHSeedCommand::ReseedAppearance:
                Actor->ReseedAppearance();
                break;
            case EMHSeedCommand::Individual:
            case EMHSeedCommand::Equal:
            case EMHSeedCommand::Paste:
                Actor->SetSeed(NewSeeds[Index]);
                break;
            case EMHSeedCommand::Lock:
            case EMHSeedCommand::KeepOnDuplicate:
                Actor->SetAutoSeed(false);
                break;
            case EMHSeedCommand::Unlock:
                Actor->SetAutoSeed(true);
                break;
            }
            if ((Command == EMHSeedCommand::Reseed ||
                 Command == EMHSeedCommand::ReseedAppearance || !NewSeeds.IsEmpty()) &&
                !Actor->GetLastPlacementError().IsEmpty())
            {
                Error += FString::Printf(TEXT("%s: %s\n"),
                    *Actor->GetPathName(), *Actor->GetLastPlacementError());
            }
        }
    }
    GEditor->RedrawLevelEditingViewports();
    NotifyOperation(
        Page,
        Error.IsEmpty()
            ? FText::Format(LOCTEXT("SeedCommandComplete", "Updated {0} MH Composite placements"), FText::AsNumber(Actors.Num()))
            : LOCTEXT("SeedResolutionFailed", "MH seed command encountered a placement error; see the Mimir log"),
        {},
        Error);
}

void ExecuteCopySeed(const TArray<TWeakObjectPtr<AMHCompositeActor>>& ActorSnapshot)
{
    TArray<AMHCompositeActor*> Actors;
    FString Error;
    if (!ResolveSeedCommandActors(ActorSnapshot, false, Actors, Error) || Actors.Num() != 1)
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Copy Seed requires exactly one MH Composite actor");
    }
    else
    {
        FPlatformApplicationMisc::ClipboardCopy(*FString::Printf(TEXT("%d"), Actors[0]->GetSeed()));
    }
    NotifyOperation(
        LOCTEXT("CopySeedPage", "Copy MH Composite Seed"),
        Error.IsEmpty() ? LOCTEXT("SeedCopied", "MH Composite seed copied") : LOCTEXT("CopySeedFailed", "Copy MH Composite Seed failed"),
        {},
        Error);
}

void ExecuteInspectResolvedPlan(
    const TArray<TWeakObjectPtr<AMHCompositeActor>>& ActorSnapshot,
    const bool bShowTrace)
{
    const FText Page = bShowTrace
        ? LOCTEXT("DecisionTracePage", "MH Composite Decision Trace")
        : LOCTEXT("ResolvedChoicesPage", "MH Composite Resolved Choices");
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(Page);
    TArray<AMHCompositeActor*> Actors;
    FString Error;
    if (!ResolveSeedCommandActors(ActorSnapshot, false, Actors, Error))
    {
        AddDiagnostic(Log, Error);
        Log.Notify(Page, EMessageSeverity::Error, true);
        return;
    }
    bool bHasErrors = false;
    for (const AMHCompositeActor* Actor : Actors)
    {
        // Inspection reconstructs the actor's already signed result on demand;
        // it never guesses a seed or changes the placement authority.
        const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
        if (Plan == nullptr || Plan->Seed != Actor->GetSeed() || !Actor->GetLastPlacementError().IsEmpty())
        {
            bHasErrors = true;
            Log.Error(FText::FromString(FString::Printf(
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: no current resolved plan for %s: %s"),
                *Actor->GetPathName(), *Actor->GetLastPlacementError())));
            continue;
        }
        Log.Info(FText::FromString(FString::Printf(
            TEXT("%s | Seed=%d | Resolver=%s | ResolvedSignature=%s | ClosureHash=%s"),
            *Actor->GetPathName(), Plan->Seed, MHRandomResolverTag,
            *Plan->ResolvedSignature, *Plan->Closure.ClosureHash)));
        for (const FMHResolvedCompositeDecision& Decision : Plan->Decisions)
        {
            TArray<FString> Weights;
            for (const float Weight : Decision.Weights)
            {
                Weights.Add(FString::Printf(TEXT("%.9g"), static_cast<double>(Weight)));
            }
            Log.Info(FText::FromString(FString::Printf(
                TEXT("choice %s | option_index=%d | weights=[%s] | raw_u32=%u | unit=%.17g | total=%.17g | target=%.17g"),
                *Decision.NodePath, Decision.OptionIndex, *FString::Join(Weights, TEXT(", ")),
                Decision.RawU32, Decision.Unit, Decision.Total, Decision.Target)));
        }
        if (bShowTrace)
        {
            for (int32 Index = 0; Index < Plan->Draws.Num(); ++Index)
            {
                const FMHResolvedCompositeDraw& Draw = Plan->Draws[Index];
                Log.Info(FText::FromString(FString::Printf(
                    TEXT("draw[%d] %s | role=%s | raw_u32=%u | unit=%.17g | sample=%.17g"),
                    Index, *Draw.NodePath, *Draw.Role, Draw.RawU32, Draw.Unit, Draw.Sample)));
            }
        }
        for (const FMHResolvedCompositeLeaf& Leaf : Plan->Leaves)
        {
            const TCHAR* Kind = Leaf.Kind == EMHRandomSemanticKind::Mesh ? TEXT("mesh") : TEXT("actor");
            Log.Info(FText::FromString(FString::Printf(
                TEXT("leaf %s -> %s:%s | world_matrix=%s"),
                *Leaf.Origin, Kind, *Leaf.Resource,
                *Leaf.WorldMatrix.ToString())));
        }
        Log.Info(FText::FromString(FString::Printf(
            TEXT("%d decisions, %d draws, %d leaves | SelectedDependencies=[%s]"),
            Plan->Decisions.Num(), Plan->Draws.Num(), Plan->Leaves.Num(),
            *FString::Join(Plan->SelectedDependencies, TEXT(", ")))));
    }
    Log.Notify(Page, bHasErrors ? EMessageSeverity::Error : EMessageSeverity::Info, true);
    Log.Open(EMessageSeverity::Info);
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

/**
 * Owner workflow (2026-09-03): move live Material Instance state from a donor
 * asset to other instances without going through source documents. The donor
 * may predate the MH source protocol and have a parent outside MasterRoot,
 * which no export path can represent.
 */
void ExecuteCopyMaterialData(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    TArray<UMaterialInstanceConstant*> Materials = Context != nullptr
        ? Context->LoadSelectedObjects<UMaterialInstanceConstant>()
        : TArray<UMaterialInstanceConstant*>();
    Materials.Remove(nullptr);
    TArray<FString> Warnings;
    FString Error;
    if (Materials.Num() != 1)
    {
        Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select exactly one Material Instance to copy");
    }
    else if (!MHCopyMaterialDataToClipboard(*Materials[0], Warnings, Error))
    {
        // Error already carries the machine-readable code.
    }
    NotifyOperation(
        LOCTEXT("CopyMaterialDataPage", "Copy MH Material Data"),
        FText::Format(
            LOCTEXT("CopyMaterialDataResult", "Copied material data from {0}"),
            FText::FromString(MHGetMaterialClipboardSourceLabel())),
        Warnings,
        Error);
}

void ExecutePasteMaterialData(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    TArray<UMaterialInstanceConstant*> Materials = Context != nullptr
        ? Context->LoadSelectedObjects<UMaterialInstanceConstant>()
        : TArray<UMaterialInstanceConstant*>();
    Materials.Remove(nullptr);
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    TArray<FString> AllWarnings;
    FString Error;
    int32 Count = 0;
    if (Materials.IsEmpty())
    {
        Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select one or more Material Instances to paste into");
    }
    else if (Settings != nullptr)
    {
        for (UMaterialInstanceConstant* Material : Materials)
        {
            TArray<FString> Warnings;
            FString ItemError;
            if (MHPasteMaterialDataFromClipboard(*Material, *Settings, Warnings, ItemError))
            {
                ++Count;
            }
            else
            {
                Error += FString::Printf(TEXT("%s: %s\n"), *Material->GetPathName(), *ItemError);
            }
            for (const FString& Warning : Warnings)
            {
                AllWarnings.Add(Material->GetPathName() + TEXT(": ") + Warning);
            }
        }
    }
    NotifyOperation(
        LOCTEXT("PasteMaterialDataPage", "Paste MH Material Data"),
        FText::Format(
            LOCTEXT("PasteMaterialDataResult", "Pasted material data into {0} Material Instances; save them to keep the change"),
            FText::AsNumber(Count)),
        AllWarnings,
        Error);
}

/**
 * Owner workflow (2026-09-03): after exporting one MI into another managed
 * material's source document, the target MI is refreshed from its source
 * without leaving the Content Browser. Same coordinator as Import Changed,
 * scoped to the selected managed materials.
 */
void ExecuteUpdateMaterialsFromSource(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    FMHImportSourcesScope Scope;
    FString Error;
    if (Context != nullptr)
    {
        for (UMaterialInstanceConstant* Material : Context->LoadSelectedObjects<UMaterialInstanceConstant>())
        {
            const UMHMaterialSourceData* Receipt = Material != nullptr
                ? Cast<UMHMaterialSourceData>(Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()))
                : nullptr;
            if (Receipt == nullptr || Receipt->LogicalName.IsEmpty())
            {
                Error += FString::Printf(
                    TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s is not a managed MH material (no receipt); use Publish or Adopt first\n"),
                    Material != nullptr ? *Material->GetPathName() : TEXT("<null>"));
                continue;
            }
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::Material;
            Key.LogicalName = Receipt->LogicalName;
            Scope.ResourceKeys.AddUnique(Key);
        }
    }
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    UMHSourceImporter* Importer = SourceImporter();
    const bool bOk = !Scope.ResourceKeys.IsEmpty() && Importer != nullptr &&
        Importer->ImportSources(Scope, Analysis, bExecuted);
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(LOCTEXT("UpdateMaterialsPage", "Update MH Materials from Source"));
    for (const FString& Warning : Analysis.Warnings) AddDiagnostic(Log, Warning);
    for (const FString& AnalysisError : Analysis.Errors) AddDiagnostic(Log, AnalysisError);
    int32 Updated = 0;
    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        if (!Scope.ResourceKeys.Contains(Entry.Key)) continue;
        for (const FString& Warning : Entry.Warnings) AddDiagnostic(Log, Warning);
        for (const FString& EntryError : Entry.Errors) AddDiagnostic(Log, EntryError);
        if (Entry.Errors.IsEmpty() &&
            (Entry.Change == EMHSourceChange::Reimport || Entry.Change == EMHSourceChange::Create ||
             Entry.Change == EMHSourceChange::Move))
        {
            ++Updated;
        }
        Log.Info(FText::FromString(FString::Printf(
            TEXT("%s: %s"), *Entry.Key.ToString(), MHSourceChangeLabel(Entry.Change))));
    }
    if (!Error.IsEmpty()) Log.Error(FText::FromString(Error));
    if (Scope.ResourceKeys.IsEmpty() && Error.IsEmpty())
    {
        Log.Error(LOCTEXT("UpdateMaterialsNoSelection", "MH_E_INVALID_RESOURCE_SOURCE: select one or more managed Material Instances"));
    }
    const bool bClean = bOk && !Analysis.HasErrors() && Error.IsEmpty();
    Log.Notify(
        FText::Format(
            LOCTEXT("UpdateMaterialsResult", "Updated {0} of {1} materials from MH Source (unchanged sources are left as is)"),
            FText::AsNumber(Updated),
            FText::AsNumber(Scope.ResourceKeys.Num())),
        bClean ? EMessageSeverity::Info : EMessageSeverity::Error,
        true);
}

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

void ReportMaterialDocumentExport(
    const FMHMaterialDocumentExportPlan& Plan,
    const FMHMaterialDocumentExportResult& Result,
    const FString& Error)
{
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(LOCTEXT("ExportMaterialDocumentPage", "Export MH Material Documents"));
    for (const FMHMaterialDocumentExportFailure& Skipped : Plan.Skipped)
    {
        Log.Error(FText::FromString(FString::Printf(
            TEXT("%s -> %s: %s"),
            *Skipped.MaterialPath,
            *Skipped.DestinationPath,
            *Skipped.Error)));
    }
    for (const FMHMaterialDocumentExportFailure& Failed : Result.FailedWrites)
    {
        Log.Error(FText::FromString(FString::Printf(
            TEXT("%s -> %s: %s"),
            *Failed.MaterialPath,
            *Failed.DestinationPath,
            *Failed.Error)));
    }
    for (const FString& Warning : Plan.Warnings)
    {
        Log.Warning(FText::FromString(Warning));
    }
    for (const FString& ExportedPath : Result.ExportedPaths)
    {
        Log.Info(FText::FromString(FString::Printf(
            TEXT("Exported material document: %s"),
            *ExportedPath)));
    }
    if (Result.bCancelled)
    {
        Log.Info(LOCTEXT(
            "ExportMaterialDocumentsCancelledLog",
            "Material document export cancelled before any file was written."));
    }
    if (!Error.IsEmpty())
    {
        Log.Error(FText::FromString(Error));
    }

    const FText Summary = Result.bCancelled
        ? LOCTEXT("ExportMaterialDocumentsCancelled", "Material document export cancelled")
        : FText::Format(
            LOCTEXT(
                "ExportMaterialDocumentsSummary",
                "Exported {0} material document(s); skipped {1}"),
            FText::AsNumber(Result.ExportedCount),
            FText::AsNumber(Plan.Skipped.Num() + Result.FailedWrites.Num()));
    const bool bHasErrors = !Error.IsEmpty() || !Plan.Skipped.IsEmpty() ||
        !Result.FailedWrites.IsEmpty();
    Log.Notify(
        Summary,
        bHasErrors ? EMessageSeverity::Warning : EMessageSeverity::Info,
        true);
}

void ExecuteExportMaterialDocuments(const FToolMenuContext& MenuContext)
{
    const UContentBrowserAssetContextMenuContext* Context =
        UContentBrowserAssetContextMenuContext::FindContextWithAssets(MenuContext);
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (Context == nullptr || Settings == nullptr || DesktopPlatform == nullptr ||
        !FSlateApplication::IsInitialized())
    {
        FMHMaterialDocumentExportPlan EmptyPlan;
        FMHMaterialDocumentExportResult EmptyResult;
        ReportMaterialDocumentExport(
            EmptyPlan,
            EmptyResult,
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: material document export UI is unavailable"));
        return;
    }

    TArray<UMaterialInstanceConstant*> Materials =
        Context->LoadSelectedObjects<UMaterialInstanceConstant>();
    Materials.Remove(nullptr);
    Materials.Sort([](const UMaterialInstanceConstant& Left, const UMaterialInstanceConstant& Right)
    {
        return Left.GetPathName() < Right.GetPathName();
    });
    if (Materials.IsEmpty())
    {
        FMHMaterialDocumentExportPlan EmptyPlan;
        FMHMaterialDocumentExportResult EmptyResult;
        ReportMaterialDocumentExport(
            EmptyPlan,
            EmptyResult,
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: no Material Instance is selected"));
        return;
    }

    const void* ParentWindow =
        FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
    const FString DefaultFolder = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir/MaterialExports"));
    TArray<FMHMaterialDocumentExportRequest> Requests;
    Requests.Reserve(Materials.Num());
    if (Materials.Num() == 1)
    {
        const FString SuggestedName =
            MHGetMaterialDocumentExportLogicalName(*Materials[0]) + TEXT(".material");
        TArray<FString> SelectedPaths;
        if (!DesktopPlatform->SaveFileDialog(
                ParentWindow,
                LOCTEXT("ExportMaterialDocumentDialogTitle", "Export Material Document").ToString(),
                DefaultFolder,
                SuggestedName,
                TEXT("MH Material Document (*.material)|*.material"),
                EFileDialogFlags::None,
                SelectedPaths) ||
            SelectedPaths.Num() != 1)
        {
            return;
        }
        Requests.Add({Materials[0], SelectedPaths[0]});
    }
    else
    {
        FString SelectedFolder;
        if (!DesktopPlatform->OpenDirectoryDialog(
                ParentWindow,
                LOCTEXT("ExportMaterialDocumentsFolderTitle", "Export Material Documents").ToString(),
                DefaultFolder,
                SelectedFolder))
        {
            return;
        }
        for (UMaterialInstanceConstant* Material : Materials)
        {
            Requests.Add({
                Material,
                FPaths::Combine(
                    SelectedFolder,
                    MHGetMaterialDocumentExportLogicalName(*Material) + TEXT(".material"))});
        }
    }

    FMHMaterialDocumentExportPlan Plan;
    FString Error;
    if (!MHPrepareMaterialDocumentExport(
            Requests,
            *Settings,
            Settings->GetSourceRootPath(),
            Plan,
            Error))
    {
        FMHMaterialDocumentExportResult EmptyResult;
        ReportMaterialDocumentExport(Plan, EmptyResult, Error);
        return;
    }

    bool bAllowOverwrite = false;
    if (!Plan.OverwritePaths.IsEmpty())
    {
        const FString Paths = FString::Join(Plan.OverwritePaths, TEXT("\n"));
        const FText Prompt = FText::Format(
            LOCTEXT(
                "ExportMaterialDocumentsOverwritePrompt",
                "The following material documents already exist:\n\n{0}\n\nOverwrite all of them? Cancelling writes nothing."),
            FText::FromString(Paths));
        bAllowOverwrite = FMessageDialog::Open(
            EAppMsgType::YesNo,
            Prompt,
            LOCTEXT("ExportMaterialDocumentsOverwriteTitle", "Overwrite Material Documents")) ==
            EAppReturnType::Yes;
    }

    FMHMaterialDocumentExportResult Result;
    Error.Reset();
    MHCommitMaterialDocumentExport(Plan, bAllowOverwrite, Result, Error);
    ReportMaterialDocumentExport(Plan, Result, Error);
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

void AddCompositeSeedActions(
    UToolMenu* Menu,
    const TArray<TWeakObjectPtr<AMHCompositeActor>>& Actors)
{
    FToolMenuSection& Seeds = Menu->AddSection(
        TEXT("MHCompositeSeedActions"), LOCTEXT("CompositeSeedActions", "Placement Seed"));
    auto AddSeedCommand = [&Seeds, &Actors](
        const FName Name, const FText& Label, const FText& Tooltip, const EMHSeedCommand Command)
    {
        AddLevelAction(Seeds, Name, Label, Tooltip,
            FToolMenuExecuteAction::CreateLambda([Actors, Command](const FToolMenuContext&)
            {
                ExecuteSeedCommand(Actors, Command);
            }));
    };
    if (Actors.Num() == 1)
    {
        AddSeedCommand(TEXT("MHReseedComposite"), LOCTEXT("ReseedComposite", "Reseed"),
            LOCTEXT("ReseedCompositeTip", "Assign a new nonzero seed, including when automatic duplicate reseeding is locked."),
            EMHSeedCommand::Reseed);
        AddSeedCommand(TEXT("MHReseedAppearance"), LOCTEXT("ReseedAppearance", "Reseed Appearance"),
            LOCTEXT("ReseedAppearanceTip", "Assign a new appearance seed: tint and wear channels reroll, layout stays."),
            EMHSeedCommand::ReseedAppearance);
        AddLevelAction(Seeds, TEXT("MHCopyCompositeSeed"), LOCTEXT("CopyCompositeSeed", "Copy Seed"),
            LOCTEXT("CopyCompositeSeedTip", "Copy this placement's signed int32 seed to the clipboard."),
            FToolMenuExecuteAction::CreateLambda([Actors](const FToolMenuContext&)
            {
                ExecuteCopySeed(Actors);
            }));
        AddSeedCommand(TEXT("MHPasteCompositeSeed"), LOCTEXT("PasteCompositeSeed", "Paste Seed"),
            LOCTEXT("PasteCompositeSeedTip", "Apply an exact decimal int32 from the clipboard. Zero is valid; duplication policy is unchanged."),
            EMHSeedCommand::Paste);
    }
    else
    {
        AddSeedCommand(TEXT("MHRandomizeCompositeSeedsIndividual"),
            LOCTEXT("IndividualCompositeSeeds", "Randomize Selected: Individual"),
            LOCTEXT("IndividualCompositeSeedsTip", "Assign a distinct new nonzero seed to every selected composite, in one undo step."),
            EMHSeedCommand::Individual);
        AddSeedCommand(TEXT("MHRandomizeCompositeSeedsEqual"),
            LOCTEXT("EqualCompositeSeeds", "Randomize Selected: Equal"),
            LOCTEXT("EqualCompositeSeedsTip", "Assign one new nonzero seed to all selected composites, in one undo step."),
            EMHSeedCommand::Equal);
    }
    AddSeedCommand(TEXT("MHLockCompositeSeed"), LOCTEXT("LockCompositeSeed", "Lock Seed"),
        LOCTEXT("LockCompositeSeedTip", "Keep the seed on duplicate. Explicit Reseed and Paste Seed remain available."),
        EMHSeedCommand::Lock);
    AddSeedCommand(TEXT("MHUnlockCompositeSeed"), LOCTEXT("UnlockCompositeSeed", "Unlock Seed"),
        LOCTEXT("UnlockCompositeSeedTip", "Restore automatic new seeds on duplication without changing the current seed."),
        EMHSeedCommand::Unlock);
    AddSeedCommand(TEXT("MHKeepCompositeSeedOnDuplicate"),
        LOCTEXT("KeepCompositeSeedOnDuplicate", "Keep Seed on Duplicate"),
        LOCTEXT("KeepCompositeSeedOnDuplicateTip", "Explicitly preserve each selected actor's current seed when it is duplicated."),
        EMHSeedCommand::KeepOnDuplicate);

    FToolMenuSection& Inspection = Menu->AddSection(
        TEXT("MHCompositeResolvedPlanActions"), LOCTEXT("CompositeResolvedPlanActions", "Resolved Placement"));
    AddLevelAction(Inspection, TEXT("MHShowResolvedChoices"), LOCTEXT("ShowResolvedChoices", "Show Resolved Choices"),
        LOCTEXT("ShowResolvedChoicesTip", "Show the current preview plan's signature, ordered choices, weights and selected leaves in the Mimir log."),
        FToolMenuExecuteAction::CreateLambda([Actors](const FToolMenuContext&)
        {
            ExecuteInspectResolvedPlan(Actors, false);
        }));
    AddLevelAction(Inspection, TEXT("MHShowDecisionTrace"), LOCTEXT("ShowDecisionTrace", "Show Decision Trace"),
        LOCTEXT("ShowDecisionTraceTip", "Show the current preview plan's ordered raw draws and profile samples without resolving again."),
        FToolMenuExecuteAction::CreateLambda([Actors](const FToolMenuContext&)
        {
            ExecuteInspectResolvedPlan(Actors, true);
        }));
}

void FillCompositeOptionsSubMenu(UToolMenu* Menu)
{
    if (Menu == nullptr)
    {
        return;
    }
    const TArray<TWeakObjectPtr<AActor>> Actors = ContextLevelActors(Menu);
    const TArray<TWeakObjectPtr<AMHCompositeActor>> CompositeActors = ContextCompositeActors(Actors);
    UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
    const bool bEditing = Subsystem != nullptr && Subsystem->IsEditingComposite();
    const bool bSelectedActiveEditActor = bEditing &&
        Actors.Num() == 1 &&
        CompositeActors.Num() == 1 &&
        Subsystem->IsEditingComposite(CompositeActors[0].Get());
    if (Actors.IsEmpty() || (bEditing && !bSelectedActiveEditActor))
    {
        return;
    }

    FToolMenuSection& Section = Menu->AddSection(
        TEXT("MHCompositeOptionsActions"),
        LOCTEXT("CompositeOptionsActions", "Composite Actions"));

    if (bSelectedActiveEditActor)
    {
        AddLevelAction(Section, TEXT("MHCommitCompositeEdit"), LOCTEXT("CommitCompositeEdit", "Apply Edited Transforms to Source"),
            LOCTEXT("CommitCompositeEditTip", "Publish the edited top-level transforms to the .composite source, then rebuild every loaded instance."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteCommitEditComposite));
        AddLevelAction(Section, TEXT("MHCancelCompositeEdit"), LOCTEXT("CancelCompositeEdit", "Cancel Transform Edit"),
            LOCTEXT("CancelCompositeEditTip", "Discard the edited transforms and refresh the instance from its managed asset."),
            FToolMenuExecuteAction::CreateStatic(&ExecuteCancelEditComposite));
        return;
    }

    AddLevelAction(Section, TEXT("MHBuildComposite"), LOCTEXT("BuildComposite", "Build Composite"),
        LOCTEXT("BuildCompositeTip", "Publish the selected representable actors as one composite and replace the selection."),
        FToolMenuExecuteAction::CreateLambda([Actors](const FToolMenuContext&)
        {
            ExecuteBuildComposite(Actors);
        }));

    if (!CompositeActors.IsEmpty() && CompositeActors.Num() == Actors.Num())
    {
        AddLevelAction(Section, TEXT("MHBreakComposite"), LOCTEXT("BreakComposite", "Break Composite"),
            LOCTEXT("BreakCompositeTip", "Materialize each selected instance's resolved plan as mesh and gameplay actors, dissolving nested composites and groups."),
            FToolMenuExecuteAction::CreateLambda([CompositeActors](const FToolMenuContext&)
            {
                ExecuteBreakComposite(CompositeActors);
            }));
        AddLevelAction(Section, TEXT("MHRefreshComposite"), LOCTEXT("RefreshComposite", "Refresh from Composite Asset"),
            LOCTEXT("RefreshCompositeTip", "Discard derived component edits and recompile the selected instances from their current managed assets."),
            FToolMenuExecuteAction::CreateLambda([CompositeActors](const FToolMenuContext&)
            {
                ExecuteRebuildSelected(CompositeActors);
            }));
    }

    if (CompositeActors.Num() == 1 && CompositeActors.Num() == Actors.Num())
    {
        AddLevelAction(Section, TEXT("MHEditComposite"), LOCTEXT("EditComposite", "Edit Placement Transforms"),
            LOCTEXT("EditCompositeTip", "Unlock the selected composite's top-level placement transforms."),
            FToolMenuExecuteAction::CreateLambda([Actor = CompositeActors[0]](const FToolMenuContext&)
            {
                ExecuteBeginEditComposite(Actor);
            }));
    }
    if (!CompositeActors.IsEmpty() && CompositeActors.Num() == Actors.Num())
    {
        AddCompositeSeedActions(Menu, CompositeActors);
    }
}

} // namespace

bool MHPromptCompositeAdoptTarget(
    UE::MimirComposite::FMHCompositeAdoptTarget& OutTarget,
    const FString& SuggestedName,
    const FText& WindowTitle,
    const FText& AcceptLabel)
{
    return PromptCompositeAdoptTarget(OutTarget, SuggestedName, WindowTitle, AcceptLabel);
}

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

    }

    if (UToolMenu* ActorContextMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.ActorContextMenu")))
    {
        ActorContextMenu->AddDynamicSection(
            TEXT("MHCompositeActorContext"),
            FNewToolMenuDelegate::CreateLambda([](UToolMenu* DynamicMenu)
            {
                const TArray<TWeakObjectPtr<AActor>> Actors = ContextLevelActors(DynamicMenu);
                if (Actors.IsEmpty())
                {
                    return;
                }
                UMHCompositeLevelSubsystem* Subsystem = LevelSubsystem();
                if (Subsystem != nullptr && Subsystem->IsEditingComposite())
                {
                    const TArray<TWeakObjectPtr<AMHCompositeActor>> CompositeActors =
                        ContextCompositeActors(Actors);
                    if (Actors.Num() != 1 || CompositeActors.Num() != 1 ||
                        !Subsystem->IsEditingComposite(CompositeActors[0].Get()))
                    {
                        return;
                    }
                }
                FToolMenuSection& Section = DynamicMenu->AddSection(TEXT("MHCompositeOptions"));
                Section.AddSubMenu(
                    TEXT("MHCompositeOptionsSubMenu"),
                    LOCTEXT("CompositeOptions", "Composite Options"),
                    LOCTEXT("CompositeOptionsTip", "Build, edit, apply, refresh or break MH composites."),
                    FNewToolMenuDelegate::CreateStatic(&FillCompositeOptionsSubMenu));
            }));
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
                FToolUIAction CopyAction;
                CopyAction.ExecuteAction = FToolMenuExecuteAction::CreateStatic(&ExecuteCopyMaterialData);
                DynamicSection.AddMenuEntry(
                    TEXT("MHCopyMaterialData"),
                    LOCTEXT("CopyMaterialData", "Copy MH Material Data"),
                    LOCTEXT(
                        "CopyMaterialDataTip",
                        "Copy the parent material, parameter overrides and static state of the selected Material Instance into the MH buffer."),
                    FSlateIcon(),
                    CopyAction);
                FToolUIAction PasteAction;
                PasteAction.ExecuteAction = FToolMenuExecuteAction::CreateStatic(&ExecutePasteMaterialData);
                PasteAction.CanExecuteAction = FToolMenuCanExecuteAction::CreateLambda(
                    [](const FToolMenuContext&) { return MHHasMaterialClipboardData(); });
                DynamicSection.AddMenuEntry(
                    TEXT("MHPasteMaterialData"),
                    LOCTEXT("PasteMaterialData", "Paste MH Material Data"),
                    LOCTEXT(
                        "PasteMaterialDataTip",
                        "Overwrite the selected Material Instances with the buffered parent, parameter overrides and static state."),
                    FSlateIcon(),
                    PasteAction);
                FToolUIAction UpdateAction;
                UpdateAction.ExecuteAction =
                    FToolMenuExecuteAction::CreateStatic(&ExecuteUpdateMaterialsFromSource);
                DynamicSection.AddMenuEntry(
                    TEXT("MHUpdateMaterialsFromSource"),
                    LOCTEXT("UpdateMaterialFromSource", "Update Material from MH Source"),
                    LOCTEXT(
                        "UpdateMaterialFromSourceTip",
                        "Re-apply the selected managed Material Instances from their .material source documents (source wins; unchanged sources are skipped)."),
                    FSlateIcon(),
                    UpdateAction);
                FToolUIAction ExportAction;
                ExportAction.ExecuteAction =
                    FToolMenuExecuteAction::CreateStatic(&ExecuteExportMaterialDocuments);
                DynamicSection.AddMenuEntry(
                    TEXT("MHExportMaterialDocuments"),
                    LOCTEXT("ExportMaterialDocument", "Export Material Document..."),
                    LOCTEXT(
                        "ExportMaterialDocumentTip",
                        "Write the canonical .material document of the selected Material Instance to any path, including another material's source inside MH Source Root; receipts are not changed. Non-serializable overrides are dropped with warnings."),
                    FSlateIcon(),
                    ExportAction);
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
