#include "UI/MHCompositeNavigation.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Components/ChildActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor.h"
#include "EditorViewportCommands.h"
#include "Toolkits/GlobalEditorCommonCommands.h"
#include "Engine/StaticMesh.h"
#include "Framework/Commands/UICommandList.h"
#include "ILevelEditor.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "Selection.h"
#include "SLevelViewport.h"
#include "UI/MHCompositeOutlinerModel.h"

namespace UE::MimirComposite
{
FBox MHGetVisualFocusBounds(const USceneComponent* Component, const int32 InstanceIndex)
{
    if (const UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Component))
    {
        FTransform World;
        if (Instances->GetStaticMesh() && Instances->GetInstanceTransform(InstanceIndex, World, true))
            return Instances->GetStaticMesh()->GetBoundingBox().TransformBy(World);
        return FBox(ForceInit);
    }
    if (const UChildActorComponent* Child = Cast<UChildActorComponent>(Component))
    {
        if (const AActor* Actor = Child->GetChildActor()) return Actor->GetComponentsBoundingBox(true, true);
    }
    if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
    {
        if (Primitive->IsRegistered()) return Primitive->Bounds.GetBox();
    }
    return FBox(ForceInit);
}

FBox MHGetPlacementFocusBounds(const AMHCompositeActor& Actor)
{
    FBox Bounds(ForceInit);
    const FString& SelectedPath = Actor.GetSelectedPlacementLeafPath();
    for (const FMHCompositeLeafMaterialization& Row : Actor.GetLeafMaterializations())
    {
        if (!SelectedPath.IsEmpty() && Row.NodePath != SelectedPath) continue;
        const FBox LeafBounds = MHGetVisualFocusBounds(Row.Component, Row.InstanceIndex);
        if (LeafBounds.IsValid) Bounds += LeafBounds;
    }
    // Empty/nonvisual composites still have a useful location, without falling
    // back to another owner's instances in the shared pool.
    if (!Bounds.IsValid) Bounds = FBox::BuildAABB(Actor.GetActorLocation(), FVector(50.0));
    return Bounds;
}

FBox MHGetOutlinerFocusBounds(FMHCompositeOutlinerModel& Model, const TSharedPtr<FMHCompositeOutlinerItem>& Item)
{
    FBox Bounds(ForceInit);
    if (!Item.IsValid() || (Item->IsOption() && !Item->bSelectedOption)) return Bounds;
    const FBox Visual = MHGetVisualFocusBounds(Item->PlacementComponent.Get(), Item->PlacementInstanceIndex);
    if (Visual.IsValid) Bounds += Visual;
    if (Item->IsCompositeReference() && !Item->bNestedChildrenLoaded) Model.ExpandItem(Item);
    for (const auto& Child : Item->Children)
    {
        const FBox ChildBounds = MHGetOutlinerFocusBounds(Model, Child);
        if (ChildBounds.IsValid) Bounds += ChildBounds;
    }
    return Bounds;
}

void MHAppendOutlinerAssets(const FMHCompositeOutlinerModel& Model, const FMHCompositeOutlinerItem& Item, TArray<UObject*>& OutAssets)
{
    FMHCompositeOutlinerNavigation Navigation;
    if (Model.GetNavigation(Item, FString(), Navigation) && Navigation.Asset.IsValid())
    {
        UObject* Asset = Navigation.Asset.Get();
        if (const UClass* Class = Cast<UClass>(Asset); Class && Class->ClassGeneratedBy) Asset = Class->ClassGeneratedBy;
        OutAssets.AddUnique(Asset);
        return;
    }
    // Groups have no asset. A random node browses its realized option; an
    // explicitly selected option row (including inactive options) browses itself.
    if (Item.Kind != EMHRandomSemanticKind::Group && Item.Kind != EMHRandomSemanticKind::Random) return;
    for (const TSharedPtr<FMHCompositeOutlinerItem>& Child : Item.Children)
        if (Child.IsValid() && (!Child->IsOption() || Child->bSelectedOption)) MHAppendOutlinerAssets(Model, *Child, OutAssets);
}

void MHAppendEditSelectionAssets(const UMHCompositeEditSession& Session, TArray<UObject*>& OutAssets)
{
    if (!Session.IsOpen() || !Session.GetRootPlacement()) return;
    if (Session.GetSelectedNodeIds().IsEmpty())
    {
        if (UObject* Asset = Session.GetEditedAsset()) OutAssets.AddUnique(Asset);
        return;
    }
    FMHCompositeOutlinerModel Model;
    if (!Model.BuildFromActor(*Session.GetRootPlacement())) return;
    TArray<TSharedPtr<FMHCompositeOutlinerItem>> Rows = Model.GetRoots();
    if (Session.IsNested())
    {
        const TSharedPtr<FMHCompositeOutlinerItem> Scope = Model.FindByNodePath(Session.GetInvocationPath());
        if (!Scope.IsValid() || !Model.ExpandItem(Scope)) return;
        Rows = Scope->Children;
    }
    TFunction<void(const TSharedPtr<FMHCompositeOutlinerItem>&)> Visit = [&](const TSharedPtr<FMHCompositeOutlinerItem>& Row)
    {
        if (!Row.IsValid()) return;
        if (Row->DraftNodeId.IsValid() && Session.GetSelectedNodeIds().Contains(Row->DraftNodeId))
            MHAppendOutlinerAssets(Model, *Row, OutAssets);
        else for (const auto& Child : Row->Children) Visit(Child);
    };
    for (const auto& Row : Rows) Visit(Row);
}

bool MHFocusCompositeSelection()
{
    if (!GEditor) return false;
    FBox Bounds(ForceInit);
    bool bCompositeSelection = false;
    if (const UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive())
    {
        Bounds = Mode->ComputeCustomViewportFocus();
        bCompositeSelection = true;
    }
    else if (USelection* Selection = GEditor->GetSelectedActors())
    {
        for (FSelectionIterator It(*Selection); It; ++It)
        {
            const AActor* Actor = Cast<AActor>(*It);
            if (!Actor) continue;
            if (const AMHCompositeActor* Composite = Cast<AMHCompositeActor>(Actor))
            {
                Bounds += MHGetPlacementFocusBounds(*Composite);
                bCompositeSelection = true;
            }
            else Bounds += Actor->GetComponentsBoundingBox(true, true);
        }
    }
    if (!bCompositeSelection) return false;
    if (Bounds.IsValid) GEditor->MoveViewportCamerasToBox(Bounds, true);
    return true;
}

namespace
{
struct FFocusBinding
{
    TWeakPtr<FUICommandList> Commands;
    TSharedPtr<const FUICommandInfo> Command;
    FUIAction Original;
    bool bRestoreOriginal = true;
};
TArray<FFocusBinding> FocusBindings;
FDelegateHandle CreatedHandle;
FDelegateHandle ContentHandle;

void BindFocusCommands()
{
    const FLevelEditorModule* Module = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
    const TSharedPtr<ILevelEditor> Editor = Module ? Module->GetFirstLevelEditor() : nullptr;
    if (!Editor.IsValid()) return;
    FocusBindings.RemoveAll([](const FFocusBinding& Binding) { return !Binding.Commands.IsValid(); });
    const auto Command = FEditorViewportCommands::Get().FocusViewportToSelection;
    for (const auto& Viewport : Editor->GetViewports())
    {
        if (!Viewport.IsValid()) continue;
        const TSharedPtr<FUICommandList> Commands = Viewport->GetCommandList();
        if (!Commands.IsValid() || FocusBindings.ContainsByPredicate([&](const FFocusBinding& Binding) { return Binding.Commands.Pin() == Commands; })) continue;
        const FUIAction* Original = Commands->GetActionForCommand(Command);
        if (!Original) continue;
        const FUIAction Saved = *Original;
        FocusBindings.Add({Commands, Command, Saved});
        FUIAction Replacement = Saved;
        Replacement.ExecuteAction = FExecuteAction::CreateLambda([Saved]()
        {
            if (!MHFocusCompositeSelection()) Saved.Execute();
        });
        Commands->UnmapAction(Command);
        Commands->MapAction(Command, Replacement);
        const auto BrowseCommand = FGlobalEditorCommonCommands::Get().FindInContentBrowser;
        if (const FUIAction* Browse = Commands->GetActionForCommand(BrowseCommand))
        {
            const FUIAction SavedBrowse = *Browse;
            // This is normally inherited, so remove only our local mapping on shutdown.
            FUIAction BrowseAction = SavedBrowse;
            BrowseAction.ExecuteAction = FExecuteAction::CreateLambda([SavedBrowse]()
            {
                const UMHCompositeLevelSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
                const UMHCompositeEditSession* Session = Subsystem ? Subsystem->GetEditSession() : nullptr;
                if (!Session || !Session->IsOpen()) { SavedBrowse.Execute(); return; }
                TArray<UObject*> Assets;
                MHAppendEditSelectionAssets(*Session, Assets);
                if (!Assets.IsEmpty()) GEditor->SyncBrowserToObjects(Assets);
            });
            BrowseAction.CanExecuteAction = FCanExecuteAction::CreateLambda([SavedBrowse]()
            {
                return UMHCompositeEditorMode::IsActive() || SavedBrowse.CanExecute();
            });
            FocusBindings.Add({Commands, BrowseCommand, SavedBrowse, false});
            Commands->MapAction(BrowseCommand, BrowseAction);
        }
    }
}
}

void MHStartupCompositeNavigation()
{
    FLevelEditorModule& Module = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    CreatedHandle = Module.OnLevelEditorCreated().AddLambda([](TSharedPtr<ILevelEditor>) { BindFocusCommands(); });
    ContentHandle = Module.OnTabContentChanged().AddStatic(&BindFocusCommands);
    BindFocusCommands();
}

void MHShutdownCompositeNavigation()
{
    if (FLevelEditorModule* Module = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
    {
        Module->OnLevelEditorCreated().Remove(CreatedHandle);
        Module->OnTabContentChanged().Remove(ContentHandle);
    }
    for (const FFocusBinding& Binding : FocusBindings)
    {
        if (const auto Commands = Binding.Commands.Pin())
        {
            Commands->UnmapAction(Binding.Command);
            if (Binding.bRestoreOriginal)
                Commands->MapAction(Binding.Command, Binding.Original);
        }
    }
    FocusBindings.Reset();
}
}
