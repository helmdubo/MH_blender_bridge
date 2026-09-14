#include "MHRecipeTestFixture.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeDependencies.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeSelectionAdapter.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "ContentBrowserMenuContexts.h"
#include "Editor.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "LevelEditorMenuContext.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "MaterialDomain.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "Selection.h"
#include "Source/MHSourceImportBatch.h"
#include "StaticMeshCompiler.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "UObject/UObjectIterator.h"

namespace UE::MimirComposite::Tests
{
namespace
{
struct FDependencyFixture
{
    FRecipeFixture Recipe;
    explicit FDependencyFixture(FAutomationTestBase& Test) : Recipe(Test) {}
    ~FDependencyFixture()
    {
        for (UObject* Asset : Recipe.Assets)
        {
            if (!IsValid(Asset)) continue;
            FAssetRegistryModule::AssetDeleted(Asset);
            Asset->GetOutermost()->SetDirtyFlag(false);
        }
    }
    UMaterialInstanceConstant* Material(const FString& Stem)
    {
        const FString Name = Recipe.Name(Stem);
        UMaterialInstanceConstant* Result = NewObject<UMaterialInstanceConstant>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Materials/") + Name)), FName(*Name), RF_Public | RF_Standalone);
        Recipe.Assets.Add(Result);
        {
            FMaterialUpdateContext Context;
            Context.AddMaterialInstance(Result);
            Result->SetParentEditorOnly(UMaterial::GetDefaultMaterial(MD_Surface));
        }
        UMHMaterialSourceData* Receipt = NewObject<UMHMaterialSourceData>(Result);
        Receipt->LogicalName = Name;
        Receipt->SourceRelativePath = Name + TEXT(".material");
        Receipt->SourceHash = MHRawPayloadHash({0x11, 0x22});
        Receipt->AppliedHash = Receipt->SourceHash;
        Result->AddAssetUserData(Receipt);
        FAssetRegistryModule::AssetCreated(Result);
        return Result;
    }
    UStaticMesh* Mesh(const FString& Stem, const TArray<FName>& Slots)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (Cube == nullptr) return nullptr;
        FStaticMeshCompilingManager::Get().FinishCompilation({Cube});
        const FString Name = Recipe.Name(Stem);
        UStaticMesh* Result = DuplicateObject<UStaticMesh>(Cube,
            CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + Name)), FName(*Name));
        Recipe.Assets.Add(Result);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Result);
        Receipt->LogicalName = Name;
        Receipt->SourceRelativePath = Name + TEXT(".mesh.fbx");
        Receipt->SourceHash = MHRawPayloadHash({0x33, 0x44});
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Result->SetAssetImportData(Receipt);
        Result->GetStaticMaterials().Reset();
        for (const FName Slot : Slots)
            Result->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), Slot, Slot));
        FAssetRegistryModule::AssetCreated(Result);
        return Result;
    }
    UMHCompositeAsset* Composite(const FString& Stem, const TArray<FMHCompositeNode>& Nodes)
    {
        FMHCompositeDocument Document;
        Document.Nodes = Nodes;
        UMHCompositeAsset* Result = Recipe.Composite(Recipe.Name(Stem), Document, {});
        if (Result != nullptr) FAssetRegistryModule::AssetCreated(Result);
        return Result;
    }
};

FMHCompositeNode DependencyNode(const EMHCompositeNodeKind Kind, const FString& Resource)
{
    FMHCompositeNode Node;
    Node.Kind = Kind;
    Node.Resource = Resource;
    return Node;
}

FToolMenuEntry* DependencyMenuEntry(UToolMenu* Menu)
{
    if (Menu == nullptr) return nullptr;
    for (FToolMenuSection& Section : Menu->Sections)
        if (FToolMenuEntry* Entry = Section.FindEntry(TEXT("MHUpdateCompositeDependencies"))) return Entry;
    return nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeDependencyUpdateMenuTest,
    "Mimir.V5.Composite.UpdateDependencies.ContentBrowserAction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeDependencyUpdateMenuTest::RunTest(const FString& Parameters)
{
    FDependencyFixture Fixture(*this);
    UMaterialInstanceConstant* Material = Fixture.Material(TEXT("dependency_cb_material"));
    UStaticMesh* Mesh = Fixture.Mesh(TEXT("dependency_cb_mesh"), {Material->GetFName()});
    if (Mesh == nullptr) return false;
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("dependency_cb_root"),
        {DependencyNode(EMHCompositeNodeKind::Mesh, Mesh->GetName())});
    if (Root == nullptr) return false;
    UContentBrowserAssetContextMenuContext* Assets = NewObject<UContentBrowserAssetContextMenuContext>();
    Assets->SelectedAssets.Add(FAssetData(Root));
    FToolMenuContext Context;
    Context.AddObject(Assets);
    UToolMenu* Menu = UToolMenus::Get()->GenerateMenu(
        TEXT("ContentBrowser.AssetContextMenu.MHCompositeAsset"), Context);
    if (!TestNotNull(TEXT("composite asset menu exists"), Menu)) return false;
    FToolMenuSection* Section = Menu->FindSection(TEXT("GetAssetActions"));
    if (!TestNotNull(TEXT("composite asset actions section exists"), Section)) return false;
    FToolMenuEntry* Entry = Section->FindEntry(TEXT("MHUpdateCompositeDependencies"));
    if (!TestNotNull(TEXT("composite asset has Update dependencies action"), Entry)) return false;
    if (!TestTrue(TEXT("Content Browser action executable"), Entry->TryExecuteToolUIAction(Context))) return false;
    const bool bOk = TestEqual(TEXT("Content Browser selection drives dependency update"),
        Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    GEditor->ResetTransaction(INVTEXT("Dependency CB test cleanup"));
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeDependencyClosureTest,
    "Mimir.V5.Composite.UpdateDependencies.NestedAndAllOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeDependencyClosureTest::RunTest(const FString& Parameters)
{
    FDependencyFixture Fixture(*this);
    UMaterialInstanceConstant* A = Fixture.Material(TEXT("dependency_mat_a"));
    UMaterialInstanceConstant* B = Fixture.Material(TEXT("dependency_mat_b"));
    UStaticMesh* Main = Fixture.Mesh(TEXT("dependency_main"), {A->GetFName(), A->GetFName()});
    UStaticMesh* Alternative = Fixture.Mesh(TEXT("dependency_alternative"), {B->GetFName()});
    UStaticMesh* Outside = Fixture.Mesh(TEXT("dependency_outside"), {A->GetFName()});
    if (Main == nullptr || Alternative == nullptr || Outside == nullptr) return false;
    UMHCompositeAsset* Nested = Fixture.Composite(TEXT("dependency_nested"),
        {DependencyNode(EMHCompositeNodeKind::Mesh, Main->GetName())});
    if (Nested == nullptr) return false;
    FMHCompositeNode Random;
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Options.Add({EMHCompositeOptionKind::Composite, Nested->LogicalName, 1.0f});
    Random.Options.Add({EMHCompositeOptionKind::Mesh, Alternative->GetName(), 0.0f});
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("dependency_root"),
        {Random, DependencyNode(EMHCompositeNodeKind::Composite, Nested->LogicalName)});
    if (Root == nullptr) return false;
    UMaterialInterface* Before = Main->GetMaterial(0);
    const FString ReceiptHash = CastChecked<UMHStaticMeshImportData>(Main->GetAssetImportData())->SourceHash;
    const int32 VertexCount = Main->GetNumVertices(0);
    const FMeshSectionInfo SectionBefore = Main->GetSectionInfoMap().Get(0, 0);
    GEditor->ResetTransaction(INVTEXT("Dependency update test"));
    int32 Notifications = 0;
    MHSetGeneratedResourceChangedObserverForTests([&](const FMHResourceKey& Key)
    {
        if (Key.Kind == EMHResourceKind::StaticMesh) ++Notifications;
    });
    ON_SCOPE_EXIT { MHSetGeneratedResourceChangedObserverForTests({}); GEditor->ResetTransaction(INVTEXT("Dependency update test cleanup")); };
    FMHCompositeDependencyUpdateResult Result;
    bool bOk = TestTrue(TEXT("complete dependency update succeeds"), MHUpdateCompositeDependencies({Root, Root}, Result));
    for (const FString& Error : Result.Errors) AddError(Error);
    bOk &= TestEqual(TEXT("root and nested composite visited once"), Result.CompositesVisited, 2);
    bOk &= TestEqual(TEXT("shared mesh and unselected option visited once"), Result.MeshesVisited, 2);
    bOk &= TestEqual(TEXT("two mesh assets changed"), Result.MeshesUpdated, 2);
    bOk &= TestEqual(TEXT("all matching slots restored"), Result.SlotsUpdated, 3);
    bOk &= TestEqual(TEXT("one notification per changed mesh"), Notifications, 2);
    bOk &= TestEqual(TEXT("nested mesh material restored"), Main->GetMaterial(0), static_cast<UMaterialInterface*>(A));
    bOk &= TestEqual(TEXT("duplicate slot restored"), Main->GetMaterial(1), static_cast<UMaterialInterface*>(A));
    bOk &= TestEqual(TEXT("zero-weight option restored"), Alternative->GetMaterial(0), static_cast<UMaterialInterface*>(B));
    bOk &= TestEqual(TEXT("outside mesh untouched"), Outside->GetMaterial(0), Before);
    bOk &= TestEqual(TEXT("slot name preserved"), Main->GetStaticMaterials()[0].MaterialSlotName, A->GetFName());
    bOk &= TestEqual(TEXT("imported slot name preserved"), Main->GetStaticMaterials()[0].ImportedMaterialSlotName, A->GetFName());
    bOk &= TestEqual(TEXT("section material index preserved"), Main->GetSectionInfoMap().Get(0, 0).MaterialIndex, SectionBefore.MaterialIndex);
    bOk &= TestEqual(TEXT("vertex count preserved"), Main->GetNumVertices(0), VertexCount);
    bOk &= TestEqual(TEXT("FBX receipt is not rewritten"), CastChecked<UMHStaticMeshImportData>(Main->GetAssetImportData())->SourceHash, ReceiptHash);
    bOk &= TestTrue(TEXT("mesh dirty for normal save"), Main->GetOutermost()->IsDirty());
    GEditor->UndoTransaction();
    FStaticMeshCompilingManager::Get().FinishCompilation({Main, Alternative});
    bOk &= TestEqual(TEXT("one undo restores all selected-closure assignments"), Main->GetMaterial(0), Before);
    bOk &= TestEqual(TEXT("one undo restores random alternative"), Alternative->GetMaterial(0), Before);
    GEditor->RedoTransaction();
    FStaticMeshCompilingManager::Get().FinishCompilation({Main, Alternative});
    bOk &= TestEqual(TEXT("redo restores assignment"), Main->GetMaterial(0), static_cast<UMaterialInterface*>(A));
    Main->GetOutermost()->SetDirtyFlag(false);
    bOk &= TestTrue(TEXT("repeating update succeeds"), MHUpdateCompositeDependencies({Root}, Result));
    bOk &= TestEqual(TEXT("repeating update changes no meshes"), Result.MeshesUpdated, 0);
    bOk &= TestFalse(TEXT("no-op does not dirty mesh"), Main->GetOutermost()->IsDirty());
    bOk &= TestEqual(TEXT("no-op does not notify again"), Notifications, 2);
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeDependencyFailureTest,
    "Mimir.V5.Composite.UpdateDependencies.MissingAndAmbiguousBindings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeDependencyFailureTest::RunTest(const FString& Parameters)
{
    FDependencyFixture Fixture(*this);
    UMaterialInstanceConstant* Good = Fixture.Material(TEXT("dependency_good"));
    UMaterialInstanceConstant* Ambiguous = Fixture.Material(TEXT("dependency_duplicate"));
    UMaterialInstanceConstant* Duplicate = DuplicateObject<UMaterialInstanceConstant>(Ambiguous,
        CreatePackage(*(TEXT("/Game/MH/DependencyTestDuplicates/") + Ambiguous->GetName())), Ambiguous->GetFName());
    Fixture.Recipe.Assets.Add(Duplicate);
    FAssetRegistryModule::AssetCreated(Duplicate);
    UStaticMesh* Mesh = Fixture.Mesh(TEXT("dependency_partial"),
        {Good->GetFName(), Ambiguous->GetFName(), FName(*Fixture.Recipe.Name(TEXT("dependency_missing")))});
    if (Mesh == nullptr) return false;
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("dependency_partial_root"),
        {DependencyNode(EMHCompositeNodeKind::Mesh, Mesh->GetName())});
    if (Root == nullptr) return false;
    UMaterialInterface* Before = Mesh->GetMaterial(1);
    FMHCompositeDependencyUpdateResult Result;
    bool bOk = TestFalse(TEXT("unresolved dependencies reported as partial result"), MHUpdateCompositeDependencies({Root}, Result));
    bOk &= TestEqual(TEXT("unique slot still repaired"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Good));
    bOk &= TestEqual(TEXT("ambiguous binding is not guessed"), Mesh->GetMaterial(1), Before);
    bOk &= TestEqual(TEXT("missing binding is not cleared"), Mesh->GetMaterial(2), Before);
    bOk &= TestTrue(TEXT("duplicate has diagnostic"), FString::Join(Result.Errors, TEXT("\n")).Contains(TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET")));
    bOk &= TestTrue(TEXT("missing has diagnostic"), FString::Join(Result.Errors, TEXT("\n")).Contains(TEXT("MH_E_RESOURCE_NOT_FOUND")));
    {
        FMHSourceImportBatchContext Batch;
        bOk &= TestFalse(TEXT("active import blocks dependency mutation"), MHUpdateCompositeDependencies({Root}, Result));
        bOk &= TestEqual(TEXT("blocked call changes no slots"), Result.SlotsUpdated, 0);
    }
    GEditor->ResetTransaction(INVTEXT("Dependency partial test cleanup"));
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeDependencySceneActionTest,
    "Mimir.V5.Composite.UpdateDependencies.SceneActionAndLiveComponents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeDependencySceneActionTest::RunTest(const FString& Parameters)
{
    FDependencyFixture Fixture(*this);
    UMaterialInstanceConstant* Material = Fixture.Material(TEXT("dependency_live_material"));
    UStaticMesh* Mesh = Fixture.Mesh(TEXT("dependency_live_mesh"), {Material->GetFName()});
    if (Mesh == nullptr) return false;
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("dependency_live_root"),
        {DependencyNode(EMHCompositeNodeKind::Mesh, Mesh->GetName())});
    if (Root == nullptr) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    if (!TestNotNull(TEXT("dependency preview world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); FlushRenderingCommands(); GEditor->ResetTransaction(INVTEXT("Dependency scene test cleanup")); };
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetAutoSeed(false);
    Actor->SetSeed(37);
    Actor->SetCompositeAsset(Root);
    const FTransform Before = Actor->GetActorTransform();
    UTypedElementSelectionSet* Selection = NewObject<UTypedElementSelectionSet>();
    Selection->SelectElement(UEngineElementsLibrary::AcquireEditorActorElementHandle(Actor), FTypedElementSelectionOptions());
    ULevelEditorContextMenuContext* SceneContext = NewObject<ULevelEditorContextMenuContext>();
    SceneContext->ContextType = ELevelEditorMenuContext::SceneOutliner;
    SceneContext->CurrentSelection = Selection;
    FToolMenuContext Context;
    Context.AddObject(SceneContext);
    UToolMenu* Menu = UToolMenus::Get()->GenerateMenu(TEXT("LevelEditor.ActorContextMenu"), Context);
    if (!TestNotNull(TEXT("scene menu exists"), Menu)) return false;
    UToolMenu* Options = UToolMenus::Get()->GenerateSubMenu(Menu, TEXT("MHCompositeOptionsSubMenu"));
    FToolMenuEntry* Entry = DependencyMenuEntry(Options);
    if (!TestNotNull(TEXT("scene Composite Options has Update dependencies"), Entry)) return false;
    if (!TestTrue(TEXT("scene action executable"), Entry->TryExecuteToolUIAction(Context))) return false;
    bool bOk = TestEqual(TEXT("scene action restores material"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    bOk &= TestEqual(TEXT("scene seed unchanged"), Actor->GetSeed(), 37);
    bOk &= TestTrue(TEXT("placement transform unchanged"), Actor->GetActorTransform().Equals(Before));
    World->SendAllEndOfFrameUpdates();
    FlushRenderingCommands();
    bool bFoundMesh = false;
    // Pooled ISM components are level-owned, not necessarily actor-owned.
    for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
    {
        UStaticMeshComponent* Component = *It;
        if (Component->GetWorld() != World || Component->GetStaticMesh() != Mesh) continue;
        bFoundMesh = true;
        bOk &= TestEqual(TEXT("live component receives restored material"), Component->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
        if (!GUsingNullRHI) bOk &= TestNotNull(TEXT("live component render proxy restored"), Component->GetSceneProxy());
    }
    bOk &= TestTrue(TEXT("selected composite still contains repaired mesh"), bFoundMesh);
    return bOk;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHDependencyStaticMeshMenuTest,
    "Mimir.V5.Composite.UpdateDependencies.StaticMeshContentBrowserAction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDependencyStaticMeshMenuTest::RunTest(const FString& Parameters)
{
    FDependencyFixture Fixture(*this);
    UMaterialInstanceConstant* Material = Fixture.Material(TEXT("dependency_static_material"));
    UStaticMesh* Mesh = Fixture.Mesh(TEXT("dependency_static_asset"), {Material->GetFName()});
    UStaticMesh* Unmanaged = Fixture.Mesh(TEXT("dependency_unmanaged_asset"), {Material->GetFName()});
    UStaticMesh* Outside = Fixture.Mesh(TEXT("dependency_static_outside"), {Material->GetFName()});
    if (Mesh == nullptr || Unmanaged == nullptr || Outside == nullptr) return false;
    Unmanaged->SetAssetImportData(nullptr);
    Unmanaged->GetStaticMaterials()[0].MaterialSlotName = NAME_None;
    UMaterialInterface* Before = Outside->GetMaterial(0);
    UContentBrowserAssetContextMenuContext* Assets = NewObject<UContentBrowserAssetContextMenuContext>();
    Assets->SelectedAssets = {FAssetData(Mesh), FAssetData(Unmanaged)};
    FToolMenuContext Context;
    Context.AddObject(Assets);
    FToolMenuEntry* Entry = DependencyMenuEntry(UToolMenus::Get()->GenerateMenu(
        TEXT("ContentBrowser.AssetContextMenu.StaticMesh"), Context));
    if (!TestNotNull(TEXT("Static Mesh asset has Update dependencies"), Entry)) return false;
    if (!TestTrue(TEXT("Static Mesh asset action executable"), Entry->TryExecuteToolUIAction(Context))) return false;
    bool bOk = TestEqual(TEXT("selected mesh repaired"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    bOk &= TestEqual(TEXT("mesh without MH receipt uses imported slot fallback"), Unmanaged->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    bOk &= TestEqual(TEXT("unselected mesh untouched"), Outside->GetMaterial(0), Before);
    GEditor->ResetTransaction(INVTEXT("Dependency static asset cleanup"));
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHDependencyStaticMeshSceneTest,
    "Mimir.V5.Composite.UpdateDependencies.StaticMeshSceneAction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDependencyStaticMeshSceneTest::RunTest(const FString& Parameters)
{
    FDependencyFixture Fixture(*this);
    UMaterialInstanceConstant* Material = Fixture.Material(TEXT("dependency_static_scene_material"));
    UStaticMesh* Mesh = Fixture.Mesh(TEXT("dependency_static_scene_mesh"), {Material->GetFName()});
    if (Mesh == nullptr) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    if (World == nullptr) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); FlushRenderingCommands(); GEditor->ResetTransaction(INVTEXT("Dependency static scene cleanup")); };
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
    Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
    UTypedElementSelectionSet* Selection = NewObject<UTypedElementSelectionSet>();
    Selection->SelectElement(UEngineElementsLibrary::AcquireEditorActorElementHandle(Actor), FTypedElementSelectionOptions());
    ULevelEditorContextMenuContext* Scene = NewObject<ULevelEditorContextMenuContext>();
    Scene->ContextType = ELevelEditorMenuContext::SceneOutliner;
    Scene->CurrentSelection = Selection;
    FToolMenuContext Context;
    Context.AddObject(Scene);
    FToolMenuEntry* Entry = DependencyMenuEntry(UToolMenus::Get()->GenerateMenu(TEXT("LevelEditor.ActorContextMenu"), Context));
    if (!TestNotNull(TEXT("Static Mesh actor has Update dependencies"), Entry)) return false;
    if (!TestTrue(TEXT("Static Mesh scene action executable"), Entry->TryExecuteToolUIAction(Context))) return false;
    bool bOk = TestEqual(TEXT("scene mesh repaired"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    bOk &= TestEqual(TEXT("scene component receives material"), Actor->GetStaticMeshComponent()->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHDependencySecondaryScopeTest,
    "Mimir.V5.Composite.UpdateDependencies.SecondarySelectionScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDependencySecondaryScopeTest::RunTest(const FString& Parameters)
{
    FDependencyFixture Fixture(*this);
    UMaterialInstanceConstant* Material = Fixture.Material(TEXT("dependency_scoped_material"));
    UStaticMesh* Chair = Fixture.Mesh(TEXT("dependency_chair"), {Material->GetFName()});
    UStaticMesh* Sibling = Fixture.Mesh(TEXT("dependency_sibling"), {Material->GetFName()});
    UStaticMesh* House = Fixture.Mesh(TEXT("dependency_house_mesh"), {Material->GetFName()});
    if (Chair == nullptr || Sibling == nullptr || House == nullptr) return false;
    UMHCompositeAsset* Interior = Fixture.Composite(TEXT("dependency_interior"), {
        DependencyNode(EMHCompositeNodeKind::Mesh, Chair->GetName()),
        DependencyNode(EMHCompositeNodeKind::Mesh, Sibling->GetName())});
    if (Interior == nullptr) return false;
    FMHCompositeNode Random;
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Options.Add({EMHCompositeOptionKind::Composite, Interior->LogicalName, 1.0f});
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("dependency_house"), {
        Random, DependencyNode(EMHCompositeNodeKind::Mesh, House->GetName())});
    if (Root == nullptr) return false;
    UMaterialInterface* Before = Chair->GetMaterial(0);
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    if (World == nullptr) return false;
    ON_SCOPE_EXIT { GEditor->SelectNone(false, true, false); World->DestroyWorld(false); FlushRenderingCommands(); GEditor->ResetTransaction(INVTEXT("Dependency selection cleanup")); };
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetAutoSeed(false);
    Actor->SetSeed(37);
    Actor->SetCompositeAsset(Root);
    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    if (Plan == nullptr) return false;
    FString ChairPath, SiblingPath;
    for (const FMHResolvedCompositeLeaf& Leaf : Plan->Leaves)
    {
        if (Leaf.Resource == Chair->GetName()) ChairPath = Leaf.Origin;
        if (Leaf.Resource == Sibling->GetName()) SiblingPath = Leaf.Origin;
    }
    if (!TestFalse(TEXT("nested chair path exists"), ChairPath.IsEmpty())) return false;
    UTypedElementSelectionSet* Selection = GEditor->GetSelectedActors()->GetElementSelectionSet();
    if (Selection == nullptr || !MHRegisterPoolInstanceSelection(*Selection)) return false;
    const FMHCompositeLeafMaterialization* Row = Actor->FindLeafMaterializationByNodePath(ChairPath);
    UInstancedStaticMeshComponent* Bucket = Row != nullptr ? Cast<UInstancedStaticMeshComponent>(Row->Component.Get()) : nullptr;
    if (Bucket == nullptr) return false;
    const FTypedElementHandle Raw = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(Bucket, Row->InstanceIndex);
    const FTypedElementSelectionOptions Options;
    GEditor->SelectNone(false, true, false);
    // Use the native primary/secondary adapter, not a hand-written selection flag.
    for (const ETypedElementSelectionMethod Method : {ETypedElementSelectionMethod::Primary, ETypedElementSelectionMethod::Secondary})
    {
        const FTypedElementHandle Resolved = Selection->GetSelectionElement(Raw, Method);
        Selection->ClearSelection(Options);
        Selection->SelectElement(Resolved, Options);
        Selection->NotifyPendingChanges();
    }
    if (!TestEqual(TEXT("native secondary selects chair"), Actor->GetSelectedPlacementLeafPath(), ChairPath)) return false;
    ULevelEditorContextMenuContext* Scene = NewObject<ULevelEditorContextMenuContext>();
    Scene->ContextType = ELevelEditorMenuContext::Viewport;
    Scene->CurrentSelection = Selection;
    FToolMenuContext Context;
    Context.AddObject(Scene);
    auto MenuEntry = [&]() -> FToolMenuEntry*
    {
        UToolMenu* Menu = UToolMenus::Get()->GenerateMenu(TEXT("LevelEditor.ActorContextMenu"), Context);
        return DependencyMenuEntry(UToolMenus::Get()->GenerateSubMenu(Menu, TEXT("MHCompositeOptionsSubMenu")));
    };
    FToolMenuEntry* Entry = MenuEntry();
    if (!TestNotNull(TEXT("secondary menu action exists"), Entry)) return false;
    // Opening a menu freezes the scope, even if another object is highlighted later.
    Actor->SelectPlacementLeafByNodePath(SiblingPath);
    if (!TestTrue(TEXT("secondary menu action executable"), Entry->TryExecuteToolUIAction(Context))) return false;
    bool bOk = TestEqual(TEXT("only picked chair repaired"), Chair->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    bOk &= TestEqual(TEXT("sibling in same interior untouched"), Sibling->GetMaterial(0), Before);
    bOk &= TestEqual(TEXT("outer house mesh untouched"), House->GetMaterial(0), Before);
    bOk &= TestEqual(TEXT("operation preserves current secondary highlight"), Actor->GetSelectedPlacementLeafPath(), SiblingPath);
    Actor->SelectPlacementLeafByNodePath(ChairPath);
    Entry = MenuEntry();
    if (!TestNotNull(TEXT("captured secondary action exists"), Entry)) return false;
    // Changing the definition while a menu is open must fail closed, even if
    // the new definition has a valid material-bearing mesh at another path.
    Actor->SetCompositeAsset(Interior);
    AddExpectedError(TEXT("MH_E_INVALID_RESOURCE_SOURCE: selected dependency target changed or is unavailable"), EAutomationExpectedErrorFlags::Contains, 1);
    Entry->TryExecuteToolUIAction(Context);
    bOk &= TestEqual(TEXT("stale selection does not widen to replacement composite"), Sibling->GetMaterial(0), Before);
    bOk &= TestEqual(TEXT("stale selection leaves outer asset untouched"), House->GetMaterial(0), Before);
    // Selecting the subcomposite itself includes its own dependencies only.
    Scene->ContextType = ELevelEditorMenuContext::SceneOutliner;
    Entry = MenuEntry();
    if (!TestNotNull(TEXT("subcomposite actor action exists"), Entry)) return false;
    Entry->TryExecuteToolUIAction(Context);
    bOk &= TestEqual(TEXT("selected subcomposite repairs its sibling"), Sibling->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    bOk &= TestEqual(TEXT("selected subcomposite excludes outer house"), House->GetMaterial(0), Before);
    Actor->SetCompositeAsset(Root);
    Actor->SelectPlacementLeafByNodePath(SiblingPath);
    // An Outliner actor selection deliberately ignores stale secondary context.
    Scene->ContextType = ELevelEditorMenuContext::SceneOutliner;
    Entry = MenuEntry();
    if (!TestNotNull(TEXT("primary actor menu action exists"), Entry)) return false;
    Entry->TryExecuteToolUIAction(Context);
    bOk &= TestEqual(TEXT("primary actor repairs sibling"), Sibling->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    bOk &= TestEqual(TEXT("primary actor repairs house"), House->GetMaterial(0), static_cast<UMaterialInterface*>(Material));
    return bOk;
}
} // namespace UE::MimirComposite::Tests
