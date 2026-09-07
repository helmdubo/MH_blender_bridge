#include "MHRecipeTestFixture.h"

#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeThumbnailRenderer.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/App.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "RenderingThread.h"
#include "StaticMeshCompiler.h"
#include "ThumbnailHelpers.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace UE::MimirComposite::Tests
{
namespace
{
FMHResourceKey ThumbnailMeshKey(const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::StaticMesh;
    Key.LogicalName = Name;
    return Key;
}

UStaticMesh* ThumbnailCube(FRecipeFixture& Fixture, const FString& Name)
{
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!Fixture.Test.TestNotNull(TEXT("stock cube"), Cube)) return nullptr;
    // Waiting is restricted to test fixture construction.
    FStaticMeshCompilingManager::Get().FinishCompilation({Cube});
    UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube,
        CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + Name)), FName(*Name));
    Mesh->SetFlags(RF_Public | RF_Standalone);
    Fixture.Assets.Add(Mesh);
    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
    UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
    Receipt->LogicalName = Name;
    Receipt->SourceRelativePath = Name + TEXT(".mesh.fbx");
    Receipt->SourceHash = MHRawPayloadHash({0x74, 0x68, 0x75, 0x6d, 0x62});
    Receipt->ImporterVersion = MHStaticMeshImporterVersion;
    Mesh->SetAssetImportData(Receipt);
    return Mesh;
}

UMHCompositeAsset* ThumbnailSingle(FRecipeFixture& Fixture, const FString& MeshName)
{
    FMHCompositeDocument Document;
    FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
    Leaf.Kind = EMHCompositeNodeKind::Mesh;
    Leaf.Resource = MeshName;
    return Fixture.Composite(Fixture.Name(TEXT("thumbnail_root")), Document, {});
}

bool IsThumbnailIsolatedHost(FAutomationTestBase& Test)
{
    return Test.TestTrue(TEXT("disk fixture is restricted to isolated automation project"),
        FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) == TEXT("MimirCompositeV5S6"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeThumbnailSelection,
    "Mimir.V5.Composite.Thumbnail.SelectedNestedLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeThumbnailSelection::RunTest(const FString& Parameters)
{
    FRecipeFixture Fixture(*this);
    const FString A = Fixture.Name(TEXT("thumbnail_a"));
    const FString B = Fixture.Name(TEXT("thumbnail_b"));
    Fixture.Mesh(A);
    Fixture.Mesh(B);
    FMHCompositeDocument ChildDocument;
    FMHCompositeNode& Random = ChildDocument.Nodes.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Transform.TranslationCm = FVector(10, 0, 0);
    for (const FString& Name : {A, B})
    {
        FMHCompositeOption& Option = Random.Options.AddDefaulted_GetRef();
        Option.Kind = EMHCompositeOptionKind::Mesh;
        Option.Resource = Name;
        Option.Weight = 1;
    }
    const FString ChildName = Fixture.Name(TEXT("thumbnail_child"));
    if (!Fixture.Composite(ChildName, ChildDocument, {})) return false;
    FMHCompositeDocument Document;
    FMHCompositeNode& Child = Document.Nodes.AddDefaulted_GetRef();
    Child.Kind = EMHCompositeNodeKind::Composite;
    Child.Resource = ChildName;
    Child.Transform.TranslationCm = FVector(100, 20, 30);
    Child.Transform.Scale = FVector(2);
    Child.Transform.RotationQuat = FQuat(FVector::UpVector, HALF_PI);
    UMHCompositeAsset* Asset = Fixture.Composite(Fixture.Name(TEXT("thumbnail_nested")), Document, {});
    if (!TestNotNull(TEXT("composite"), Asset)) return false;
    Asset->GetOutermost()->SetDirtyFlag(false);
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    const int32 ActorsBefore = EditorWorld && EditorWorld->GetCurrentLevel() ? EditorWorld->GetCurrentLevel()->Actors.Num() : 0;
    const bool bMapDirtyBefore = EditorWorld && EditorWorld->GetOutermost()->IsDirty();
    int32 ResourceChanges = 0;
    MHSetGeneratedResourceChangedObserverForTests([&](const FMHResourceKey&) { ++ResourceChanges; });
    ON_SCOPE_EXIT { MHSetGeneratedResourceChangedObserverForTests({}); };
    MHResetEndpointResolveMetrics();
    MHResetPlacementStageMetrics();
    // Stock UThumbnailManager intentionally leaves Renderer null under NullRHI
    // (RegisterCustomRenderer checks FApp::CanEverRender). Exercise preparation
    // directly there; the RHI lane must use the actual registered renderer.
    TStrongObjectPtr<UMHCompositeThumbnailRenderer> HeadlessRenderer;
    UMHCompositeThumbnailRenderer* Renderer = nullptr;
    if (FApp::CanEverRender())
    {
        FThumbnailRenderingInfo* Info = UThumbnailManager::Get().GetRenderingInfo(Asset);
        Renderer = Info ? Cast<UMHCompositeThumbnailRenderer>(Info->Renderer) : nullptr;
    }
    else
    {
        HeadlessRenderer.Reset(NewObject<UMHCompositeThumbnailRenderer>());
        Renderer = HeadlessRenderer.Get();
    }
    if (!TestNotNull(TEXT("thumbnail renderer (registered when RHI enabled)"), Renderer)) return false;
    ON_SCOPE_EXIT { Renderer->ReleaseResources(); };
    if (!TestTrue(TEXT("thumbnail preparation is ready"), Renderer->CanVisualizeAsset(Asset))) return false;
    const FMHResolvedCompositePlan* Plan = Renderer->GetPreparedPlanForTests(Asset);
    if (!TestNotNull(TEXT("prepared plan"), Plan) || !TestEqual(TEXT("one selected leaf"), Plan->Leaves.Num(), 1)) return false;
    const FString Selected = Plan->Leaves[0].Resource;
    TestTrue(TEXT("one random option selected"), Selected == A || Selected == B);
    TestTrue(TEXT("selected mesh requested"), Renderer->HasRequestedMeshForTests(Selected));
    TestFalse(TEXT("unselected mesh never requested"), Renderer->HasRequestedMeshForTests(Selected == A ? B : A));
    TestTrue(TEXT("nested scale, rotation and translation compose"),
        Plan->Leaves[0].WorldMatrix.GetOrigin().Equals(FVector(100, 40, 30), 0.01));
    TestEqual(TEXT("fixed layout seed"), Plan->Seed, 0);
    TestEqual(TEXT("fixed appearance seed"), Plan->Appearance.AppearanceSeed, 0);
    TestTrue(TEXT("preview plan has no proof signature"), Plan->ResolvedSignature.IsEmpty());
    TestTrue(TEXT("repeated query ready"), Renderer->CanVisualizeAsset(Asset));
    TestTrue(TEXT("cached plan reused"), Plan == Renderer->GetPreparedPlanForTests(Asset));
    TestEqual(TEXT("no resource/proof notifications"), ResourceChanges, 0);
    TestEqual(TEXT("no sync package loads"), MHGetEndpointResolveMetrics().PackageLoadsSync, 0ull);
    TestEqual(TEXT("no compilation waits"), MHGetPlacementStageMetrics().Get(EMHPlacementStage::WaitStaticMeshCompilation).Calls, 0ull);
    TestFalse(TEXT("asset package stays clean"), Asset->GetOutermost()->IsDirty());
    if (EditorWorld && EditorWorld->GetCurrentLevel())
    {
        TestEqual(TEXT("no actors added to editor map"), EditorWorld->GetCurrentLevel()->Actors.Num(), ActorsBefore);
        TestEqual(TEXT("map dirtiness unchanged"), EditorWorld->GetOutermost()->IsDirty(), bMapDirtyBefore);
    }
    // Exercise the real dependency notification after checking preview isolation.
    // The previous raw plan pointer is intentionally not used after invalidation.
    UMHCompositeAsset* ChildAsset = Fixture.Composites.FindChecked(ChildName);
    ChildAsset->Nodes[0].Transform.SetTranslation(FVector(25, 0, 0));
    MHNotifyCompositeAssetChanged(*ChildAsset);
    Renderer->TickPendingForTests();
    const FMHResolvedCompositePlan* Updated = Renderer->GetPreparedPlanForTests(Asset);
    if (TestNotNull(TEXT("parent thumbnail refreshes after child notification"), Updated) &&
        TestEqual(TEXT("updated thumbnail retains one selected leaf"), Updated->Leaves.Num(), 1))
    {
        TestTrue(TEXT("parent thumbnail uses changed nested transform"),
            Updated->Leaves[0].WorldMatrix.GetOrigin().Equals(FVector(100, 70, 30), 0.01));
        TestEqual(TEXT("child edit preserves deterministic random selection"), Updated->Leaves[0].Resource, Selected);
        TestFalse(TEXT("child refresh still does not request unselected option"),
            Renderer->HasRequestedMeshForTests(Selected == A ? B : A));
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeThumbnailCold,
    "Mimir.V5.Composite.Thumbnail.ColdEndpointRetriesWithoutPropertyChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeThumbnailCold::RunTest(const FString& Parameters)
{
    if (!IsThumbnailIsolatedHost(*this)) return false;
    FRecipeFixture Fixture(*this);
    const FString Name = Fixture.Name(TEXT("thumbnail_cold"));
    const FString UnselectedName = Fixture.Name(TEXT("thumbnail_cold_unselected"));
    TArray<FString> Filenames;
    ON_SCOPE_EXIT
    {
        for (const FString& Filename : Filenames) IFileManager::Get().Delete(*Filename, false, true, true);
    };
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    for (const FString& MeshName : {Name, UnselectedName})
    {
        UStaticMesh* Mesh = ThumbnailCube(Fixture, MeshName);
        if (!Mesh) return false;
        UPackage* Package = Mesh->GetOutermost();
        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        Filenames.Add(Filename);
        if (!TestTrue(TEXT("cold package saved"), UPackage::SavePackage(Package, Mesh, *Filename, Args))) return false;
        Fixture.Assets.Remove(Mesh);
        Mesh->ClearFlags(RF_Public | RF_Standalone);
        Mesh->MarkAsGarbage();
        Package->ClearFlags(RF_Public | RF_Standalone);
        Package->MarkAsGarbage();
    }
    CollectGarbage(RF_NoFlags);
    TestNull(TEXT("mesh is cold"), FindObject<UStaticMesh>(nullptr, *MHEndpointObjectPath(ThumbnailMeshKey(Name))));
    TestNull(TEXT("unselected mesh is cold"), FindObject<UStaticMesh>(nullptr, *MHEndpointObjectPath(ThumbnailMeshKey(UnselectedName))));
    FMHCompositeDocument Document;
    FMHCompositeNode& Random = Document.Nodes.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    for (const FString& MeshName : {Name, UnselectedName})
    {
        FMHCompositeOption& Option = Random.Options.AddDefaulted_GetRef();
        Option.Kind = EMHCompositeOptionKind::Mesh;
        Option.Resource = MeshName;
        Option.Weight = MeshName == Name ? 1.0f : 0.0f;
    }
    UMHCompositeAsset* Asset = Fixture.Composite(Fixture.Name(TEXT("thumbnail_cold_root")), Document, {});
    if (!Asset) return false;
    Asset->GetOutermost()->SetDirtyFlag(false);
    TStrongObjectPtr<UMHCompositeThumbnailRenderer> Renderer(NewObject<UMHCompositeThumbnailRenderer>());
    ON_SCOPE_EXIT { Renderer->ReleaseResources(); };
    int32 Dirtied = 0;
    int32 PropertyChanges = 0;
    const FDelegateHandle DirtyHandle = UThumbnailManager::Get().GetOnThumbnailDirtied().AddLambda(
        [&](const FSoftObjectPath& Path) { if (Path == FSoftObjectPath(Asset)) ++Dirtied; });
    const FDelegateHandle PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
        [&](UObject* Object, FPropertyChangedEvent&) { if (Object == Asset) ++PropertyChanges; });
    ON_SCOPE_EXIT
    {
        UThumbnailManager::Get().GetOnThumbnailDirtied().Remove(DirtyHandle);
        FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
    };
    MHResetEndpointResolveMetrics();
    TestFalse(TEXT("cold endpoint is pending, not a blank render"), Renderer->CanVisualizeAsset(Asset));
    TestNull(TEXT("pending query creates no preview scene"), Renderer->GetPreviewSceneForTests());
    TestTrue(TEXT("selected cold mesh requested"), Renderer->HasRequestedMeshForTests(Name));
    TestFalse(TEXT("unselected cold mesh never requested"), Renderer->HasRequestedMeshForTests(UnselectedName));
    TestTrue(TEXT("fixture completes thumbnail async loading"), Renderer->FlushAsyncLoadsForTests());
    UStaticMesh* Reloaded = FindObject<UStaticMesh>(nullptr, *MHEndpointObjectPath(ThumbnailMeshKey(Name)));
    if (!TestNotNull(TEXT("selected mesh loaded asynchronously"), Reloaded)) return false;
    Fixture.Assets.Add(Reloaded);
    // Compilation waits belong only to fixture completion, never the renderer.
    FStaticMeshCompilingManager::Get().FinishCompilation({Reloaded});
    Renderer->TickPendingForTests();
    TestTrue(TEXT("ready endpoint becomes visualizable"), Renderer->CanVisualizeAsset(Asset));
    TestEqual(TEXT("one explicit thumbnail retry"), Dirtied, 1);
    Renderer->TickPendingForTests();
    TestEqual(TEXT("ready state does not continuously invalidate"), Dirtied, 1);
    TestEqual(TEXT("no synthetic property event"), PropertyChanges, 0);
    TestEqual(TEXT("no synchronous endpoint loading"), MHGetEndpointResolveMetrics().PackageLoadsSync, 0ull);
    TestFalse(TEXT("completion does not dirty asset package"), Asset->GetOutermost()->IsDirty());
    TestFalse(TEXT("unselected mesh remains unrequested after completion"), Renderer->HasRequestedMeshForTests(UnselectedName));
    TestNull(TEXT("unselected mesh remains unloaded on disk"),
        FindObject<UStaticMesh>(nullptr, *MHEndpointObjectPath(ThumbnailMeshKey(UnselectedName))));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeThumbnailRaster,
    "Mimir.V5.Composite.Thumbnail.RenderAndCacheRealGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeThumbnailRaster::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("MHPreviewRenderSmoke")))
    {
        AddInfo(TEXT("RHI lane NOT RUN: requires -MHPreviewRenderSmoke without -nullrhi in isolated host"));
        return true;
    }
    if (!IsThumbnailIsolatedHost(*this)) return false;
    FRecipeFixture Fixture(*this);
    const FString Name = Fixture.Name(TEXT("thumbnail_render"));
    if (!ThumbnailCube(Fixture, Name)) return false;
    UMHCompositeAsset* Asset = ThumbnailSingle(Fixture, Name);
    if (!Asset) return false;
    FThumbnailRenderingInfo* Info = UThumbnailManager::Get().GetRenderingInfo(Asset);
    UMHCompositeThumbnailRenderer* Renderer = Info ? Cast<UMHCompositeThumbnailRenderer>(Info->Renderer) : nullptr;
    if (!TestNotNull(TEXT("registered renderer ready"), Renderer)) return false;
    ON_SCOPE_EXIT { Renderer->ReleaseResources(); };
    FObjectThumbnail Rendered;
    // Warm scene registration, then capture real pixels with settled render commands.
    ThumbnailTools::RenderThumbnail(Asset, 256, 256, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Rendered);
    const FThumbnailPreviewScene* Scene = Renderer->GetPreviewSceneForTests();
    if (!TestNotNull(TEXT("isolated thumbnail scene"), Scene)) return false;
    Scene->GetWorld()->SendAllEndOfFrameUpdates();
    ThumbnailTools::RenderThumbnail(Asset, 256, 256, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Rendered);
    TArray<UInstancedStaticMeshComponent*> Geometry;
    for (TObjectIterator<UInstancedStaticMeshComponent> It; It; ++It)
        if (It->GetWorld() == Scene->GetWorld()) { Geometry.Add(*It); It->SetVisibility(false); }
    TestEqual(TEXT("cube materialized in isolated ISM"), Geometry.Num(), 1);
    Scene->GetWorld()->SendAllEndOfFrameUpdates();
    FObjectThumbnail Background;
    ThumbnailTools::RenderThumbnail(Asset, 256, 256, ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Background);
    for (UInstancedStaticMeshComponent* Component : Geometry) Component->SetVisibility(true);
    const TArray<uint8>& Pixels = Rendered.GetUncompressedImageData();
    const TArray<uint8>& EmptyPixels = Background.GetUncompressedImageData();
    if (!TestEqual(TEXT("RGBA image byte count"), Pixels.Num(), 256 * 256 * 4) ||
        !TestEqual(TEXT("background image byte count"), EmptyPixels.Num(), Pixels.Num())) return false;
    int32 ChangedPixels = 0;
    for (int32 Index = 0; Index < Pixels.Num(); Index += 4)
        if (FMath::Abs(int32(Pixels[Index]) - int32(EmptyPixels[Index])) +
            FMath::Abs(int32(Pixels[Index + 1]) - int32(EmptyPixels[Index + 1])) +
            FMath::Abs(int32(Pixels[Index + 2]) - int32(EmptyPixels[Index + 2])) > 12) ++ChangedPixels;
    TestTrue(TEXT("actual mesh changes over 100 raster pixels, not merely sky/floor"), ChangedPixels > 100);
    TArray<FColor> Colors;
    Colors.SetNumUninitialized(256 * 256);
    FMemory::Memcpy(Colors.GetData(), Pixels.GetData(), Pixels.Num());
    TArray64<uint8> Png;
    FImageUtils::PNGCompressImageArray(256, 256, MakeArrayView(Colors), Png);
    const FString Output = FPaths::ProjectSavedDir() / TEXT("Automation/CompositeThumbnail.png");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true);
    TestTrue(TEXT("review PNG saved"), FFileHelper::SaveArrayToFile(Png, *Output));
    AddInfo(TEXT("thumbnail raster review: ") + FPaths::ConvertRelativePathToFull(Output));
    Scene->GetWorld()->SendAllEndOfFrameUpdates();
    FObjectThumbnail* NativeThumbnail = ThumbnailTools::GenerateThumbnailForObjectToSaveToDisk(Asset);
    if (!TestNotNull(TEXT("native save thumbnail generator"), NativeThumbnail)) return false;
    const int32 SavedWidth = NativeThumbnail->GetImageWidth();
    TestTrue(TEXT("native save thumbnail contains image"), SavedWidth > 0);
    // Thumbnail tables reconstruct the package name from the file path.
    // Save at this fixture's canonical path, not under the /Temp mount.
    const FString PackageFile = FPackageName::LongPackageNameToFilename(
        Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*PackageFile, false, true, true); };
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    TestTrue(TEXT("real thumbnail package saved"), UPackage::SavePackage(Asset->GetOutermost(), Asset, *PackageFile, SaveArgs));
    FThumbnailMap Loaded;
    TSet<FName> Names;
    Names.Add(FName(*Asset->GetFullName()));
    TestTrue(TEXT("thumbnail loads from saved package"), ThumbnailTools::LoadThumbnailsFromPackage(PackageFile, Names, Loaded));
    const FObjectThumbnail* Saved = Loaded.Find(FName(*Asset->GetFullName()));
    if (TestNotNull(TEXT("saved thumbnail entry"), Saved)) TestEqual(TEXT("saved raster width"), Saved->GetImageWidth(), SavedWidth);
    return !HasAnyErrors();
}
}
