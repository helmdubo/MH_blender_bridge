#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "FileHelpers.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "StaticMeshCompiler.h"
#include "Index/MHProjectResourceIndex.h"
#include "Material/MHMaterialSourceData.h"
#include "Material/MHMaterialProtocol.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "Source/MHSourceImportMetrics.h"
#include "UObject/Package.h"
#include "UObject/PackageReload.h"

namespace UE::MimirComposite::Tests
{
namespace
{
bool PersistFixtureParent(FAutomationTestBase& Test, UMaterial* Parent)
{
    if (!Test.TestNotNull(TEXT("fixture parent exists"), Parent)) return false;
    FAssetRegistryModule::AssetCreated(Parent);
    Parent->MarkPackageDirty();
    UPackage* Package = Parent->GetOutermost();
    if (!UEditorLoadingAndSavingUtils::SavePackages({Package}, true))
    {
        Test.AddError(FString::Printf(TEXT("could not persist fixture parent: %s"), *Parent->GetPathName()));
        return false;
    }
    // ReloadPackage must be able to resolve the material's parent import from a
    // real package, just as it does for the project's authored master materials.
    return Test.TestTrue(TEXT("fixture parent package persisted before reimport"),
        IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension())));
}

struct FMaterialBatchFixture
{
    FString Root;
    FString Token;
    FString ExpectedHash;
    UMHCompositeSettings* Settings = nullptr;
    UMaterial* Parent = nullptr;
    TArray<UMaterialInstanceConstant*> Materials;
    TArray<FString> PackageNames;

    ~FMaterialBatchFixture()
    {
        MHShutdownProjectIndex();
        for (const FString& PackageName : PackageNames)
        {
            const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
            if (UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *(PackageName + TEXT(".") + Name)))
            {
                ObjectTools::DeleteSingleObject(Asset, false);
            }
            if (UPackage* Package = FindPackage(nullptr, *PackageName)) Package->SetDirtyFlag(false);
            IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(
                PackageName, FPackageName::GetAssetPackageExtension()), false, true, true);
        }
        if (!Root.IsEmpty()) IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    bool Build(FAutomationTestBase& Test, const int32 Count)
    {
        Token = TEXT("force_batch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests"), Token));
        IFileManager::Get().MakeDirectory(*Root, true);
        Settings = NewObject<UMHCompositeSettings>();
        Settings->MasterRoot = TEXT("/Game/MH/TestMasterMaterials");
        const FString ParentName = Token + TEXT("_parent");
        const FString ParentPackage = Settings->MasterRoot / ParentName;
        PackageNames.Add(ParentPackage);
        Parent = NewObject<UMaterial>(CreatePackage(*ParentPackage), FName(*ParentName), RF_Public | RF_Standalone);
        if (!PersistFixtureParent(Test, Parent)) return false;
        FMHMaterialDocument Document;
        Document.Parent = ParentName;
        Document.bHasTwoSided = true;
        Document.bTwoSided = true;
        FMHMaterialParameter Scalar;
        Scalar.Scalar = 0.375f;
        Document.Params.Add(TEXT("batch_scalar"), Scalar);
        TArray<uint8> Bytes;
        FString Error;
        if (!MHWriteCanonicalMaterialV4(Document, Bytes, Error))
        {
            Test.AddError(Error);
            return false;
        }
        ExpectedHash = MHRawPayloadHash(Bytes);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FString Name = FString::Printf(TEXT("%s_%03d"), *Token, Index);
            if (!FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(Root, Name + TEXT(".material")))) return false;
            const FString PackageName = FString(TEXT("/Game/MH/Generated/Materials/")) + Name;
            PackageNames.Add(PackageName);
            UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(
                CreatePackage(*PackageName), FName(*Name), RF_Public | RF_Standalone);
            UMHMaterialSourceData* Receipt = NewObject<UMHMaterialSourceData>(Material);
            Receipt->LogicalName = Name;
            Receipt->SourceRelativePath = Name + TEXT(".material");
            // Equal source hashes must never skip this explicit source-wins command.
            Receipt->SourceHash = ExpectedHash;
            Receipt->AppliedHash = ExpectedHash;
            Receipt->AppliedParent = TEXT("class:") + ParentName;
            Material->AddAssetUserData(Receipt);
            FAssetRegistryModule::AssetCreated(Material);
            Materials.Add(Material);
        }
        return true;
    }

    bool CheckMaterial(FAutomationTestBase& Test, UMaterialInstanceConstant* Material)
    {
        if (!Test.TestNotNull(TEXT("material exists"), Material)) return false;
        const UMHMaterialSourceData* Receipt = Cast<UMHMaterialSourceData>(
            Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
        bool bOk = Test.TestEqual(TEXT("source parent applied"), Material->Parent.Get(), static_cast<UMaterialInterface*>(Parent));
        if (Material->Parent.Get() != Parent)
            Test.AddError(FString::Printf(TEXT("material parent mismatch after apply/reload: material=%s expected=%s actual=%s"),
                *Material->GetPathName(), *GetPathNameSafe(Parent), *GetPathNameSafe(Material->Parent.Get())));
        bOk &= Test.TestTrue(TEXT("source two-sided applied"),
            Material->BasePropertyOverrides.bOverride_TwoSided && Material->BasePropertyOverrides.TwoSided);
        bOk &= Test.TestEqual(TEXT("source scalar only"), Material->ScalarParameterValues.Num(), 1);
        if (Material->ScalarParameterValues.Num() == 1)
            bOk &= Test.TestEqual(TEXT("source scalar value"), Material->ScalarParameterValues[0].ParameterValue, 0.375f);
        bOk &= Test.TestNotNull(TEXT("receipt exists"), Receipt);
        if (Receipt != nullptr)
        {
            bOk &= Test.TestEqual(TEXT("source receipt hash"), Receipt->SourceHash, ExpectedHash);
            bOk &= Test.TestEqual(TEXT("applied receipt hash"), Receipt->AppliedHash, ExpectedHash);
        }
        bOk &= Test.TestFalse(TEXT("package was saved"), Material->GetOutermost()->IsDirty());
        bOk &= Test.TestTrue(TEXT("package exists on disk"), IFileManager::Get().FileExists(
            *FPackageName::LongPackageNameToFilename(Material->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension())));
        return bOk;
    }
};

bool CheckBatchMetrics(FAutomationTestBase& Test, const int32 Count)
{
    const FMHSourceImportMetrics Metrics = MHGetSourceImportMetrics();
    bool bOk = Test.TestEqual(TEXT("one compilation wait"), Metrics.CallsForStage(EMHSourceImportMetricStage::BuildWait), 1ull);
    bOk &= Test.TestEqual(TEXT("one save call"), Metrics.CallsForStage(EMHSourceImportMetricStage::SavePackage), 1ull);
    bOk &= Test.TestEqual(TEXT("each unique material applied once"),
        Metrics.Get(EMHSourceImportMetricResource::Material, EMHSourceImportMetricStage::Create).Calls, static_cast<uint64>(Count));
    bOk &= Test.TestEqual(TEXT("one progress scope"), Metrics.ProgressScopes, 1ull);
    bOk &= Test.TestEqual(TEXT("one progress tick per applied target"), Metrics.ProgressResourceTicks, static_cast<uint64>(Count));
    return bOk;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHMaterialForceLargeBatchTest,
    "Mimir.V4.Material.BulkForceReimport.LargeBatch200",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialForceLargeBatchTest::RunTest(const FString& Parameters)
{
    FMaterialBatchFixture Fixture;
    if (!Fixture.Build(*this, 200)) return false;
    MHShutdownProjectIndex();
    TArray<UMaterialInstanceConstant*> Targets = Fixture.Materials;
    Targets.Add(Fixture.Materials[0]);
    bool bOk = true;
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        const TSharedPtr<FMHProjectResourceIndex> Before = MHPeekProjectIndex();
        const int32 ScansBefore = Before.IsValid() ? Before->GetFullScanCountForTests() : 0;
        const int64 GenerationBefore = Before.IsValid() ? Before->GetGeneration() : 0;
        MHResetSourceImportMetrics();
        TArray<FMHMaterialReimportResult> Results;
        FString Error;
        bOk &= TestTrue(TEXT("force reimport succeeds even with unchanged sources"),
            MHReimportMaterialsFromSource(Targets, Fixture.Root, *Fixture.Settings, Results, Error));
        if (!Error.IsEmpty()) AddError(Error);
        bOk &= TestEqual(TEXT("duplicate selection deduplicated"), Results.Num(), 200);
        for (const FMHMaterialReimportResult& Result : Results)
        {
            bOk &= TestTrue(*Result.Error, Result.bSucceeded);
            bOk &= Fixture.CheckMaterial(*this, Result.Material);
        }
        bOk &= CheckBatchMetrics(*this, 200);
        const TSharedPtr<FMHProjectResourceIndex> After = MHPeekProjectIndex();
        if (!TestTrue(TEXT("index remains available"), After.IsValid())) return false;
        bOk &= TestEqual(TEXT("one complete source scan"), After->GetFullScanCountForTests() - ScansBefore, 1);
        // Each full scan increments generation once. Exactly one further increment
        // proves the generated-asset projection was committed once for all 200 targets.
        bOk &= TestEqual(TEXT("one scan plus one projection"), After->GetGeneration() - GenerationBefore, static_cast<int64>(2));
    }
    for (const int32 Index : {0, 199})
    {
        const FString Name = Fixture.Materials[Index]->GetName();
        UPackage* Reloaded = ReloadPackage(Fixture.Materials[Index]->GetOutermost(), LOAD_None);
        UMaterialInstanceConstant* ReloadedMaterial = Reloaded != nullptr
            ? FindObject<UMaterialInstanceConstant>(Reloaded, *Name) : nullptr;
        bOk &= Fixture.CheckMaterial(*this, ReloadedMaterial);
    }
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHMaterialForceMixedCancelledTest,
    "Mimir.V4.Material.BulkForceReimport.InvalidAndCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialForceMixedCancelledTest::RunTest(const FString& Parameters)
{
    FMaterialBatchFixture Fixture;
    if (!Fixture.Build(*this, 3)) return false;
    UMaterialInstanceConstant* Invalid = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
    TArray<UMaterialInstanceConstant*> Targets = {Invalid, Fixture.Materials[0], Fixture.Materials[1], Fixture.Materials[2]};
    TArray<FMHMaterialReimportResult> Results;
    FString Error;
    int32 CancelChecks = 0;
    MHResetSourceImportMetrics();
    const bool bSucceeded = MHReimportMaterialsFromSource(Targets, Fixture.Root, *Fixture.Settings,
        Results, Error, false, [&CancelChecks]() { return ++CancelChecks > 1; });
    bool bOk = TestFalse(TEXT("mixed/cancelled batch returns false"), bSucceeded);
    bOk &= TestTrue(TEXT("partial commit has no batch error"), Error.IsEmpty());
    if (!TestEqual(TEXT("all targets have outcomes"), Results.Num(), 4)) return false;
    bOk &= TestTrue(TEXT("invalid receipt diagnosed"), Results[0].Error.StartsWith(TEXT("MH_E_INVALID_RESOURCE_SOURCE")));
    bOk &= TestNull(TEXT("invalid target never mutated"), Invalid->Parent.Get());
    bOk &= TestNull(TEXT("invalid target never adopted"), Invalid->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    bOk &= TestTrue(TEXT("completed prefix persisted"), Results[1].bSucceeded);
    bOk &= Fixture.CheckMaterial(*this, Results[1].Material);
    for (int32 Index = 2; Index < 4; ++Index)
    {
        bOk &= TestTrue(TEXT("unstarted target marked cancelled"), Results[Index].bCancelled);
        bOk &= TestFalse(TEXT("cancelled target not reported successful"), Results[Index].bSucceeded);
        bOk &= TestNull(TEXT("cancelled target never mutated"), Results[Index].Material->Parent.Get());
    }
    bOk &= CheckBatchMetrics(*this, 1);
    return bOk;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHMaterialForceLiveSceneTest,
    "Mimir.V4.Material.BulkForceReimport.LiveSceneParentChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialForceLiveSceneTest::RunTest(const FString& Parameters)
{
    FMaterialBatchFixture Fixture;
    if (!Fixture.Build(*this, 2)) return false;
    TArray<FMHMaterialReimportResult> Results;
    FString Error;
    if (!TestTrue(TEXT("initial materials imported"), MHReimportMaterialsFromSource(
        Fixture.Materials, Fixture.Root, *Fixture.Settings, Results, Error)))
    {
        AddError(Error);
        return false;
    }
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("renderable engine cube exists"), Cube)) return false;
    FStaticMeshCompilingManager::Get().FinishCompilation({Cube});
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    if (!TestNotNull(TEXT("isolated preview world exists"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); FlushRenderingCommands(); };
    AActor* Actor = World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("live material host actor exists"), Actor)) return false;
    UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(Actor);
    Actor->SetRootComponent(Mesh);
    Actor->AddInstanceComponent(Mesh);
    Mesh->SetStaticMesh(Cube);
    Mesh->SetMaterial(0, Fixture.Materials[0]);
    Mesh->RegisterComponent();
    World->SendAllEndOfFrameUpdates();
    FlushRenderingCommands();
    bool bOk = TestTrue(TEXT("mesh registered before material mutation"), Mesh->IsRegistered());
    if (!GUsingNullRHI)
    {
        bOk &= TestTrue(TEXT("render state exists before mutation"), Mesh->IsRenderStateCreated());
        bOk &= TestNotNull(TEXT("scene proxy exists before mutation"), Mesh->GetSceneProxy());
    }
    const FString ParentName = Fixture.Token + TEXT("_replacement_parent");
    const FString ParentPackage = Fixture.Settings->MasterRoot / ParentName;
    Fixture.PackageNames.Add(ParentPackage);
    UMaterial* ReplacementParent = NewObject<UMaterial>(CreatePackage(*ParentPackage), FName(*ParentName), RF_Public | RF_Standalone);
    if (!PersistFixtureParent(*this, ReplacementParent)) return false;
    FMHMaterialDocument Replacement;
    Replacement.Parent = ParentName;
    Replacement.bHasTwoSided = true;
    Replacement.bTwoSided = false;
    FMHMaterialParameter Scalar;
    Scalar.Scalar = 0.625f;
    Replacement.Params.Add(TEXT("batch_scalar"), Scalar);
    TArray<uint8> Bytes;
    if (!MHWriteCanonicalMaterialV4(Replacement, Bytes, Error)) { AddError(Error); return false; }
    for (UMaterialInstanceConstant* Material : Fixture.Materials)
    {
        if (!FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(Fixture.Root, Material->GetName() + TEXT(".material")))) return false;
    }
    MHResetSourceImportMetrics();
    bOk &= TestTrue(TEXT("live-scene parent/override reimport succeeds"), MHReimportMaterialsFromSource(
        Fixture.Materials, Fixture.Root, *Fixture.Settings, Results, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bOk &= CheckBatchMetrics(*this, 2);
    World->SendAllEndOfFrameUpdates();
    FlushRenderingCommands();
    bOk &= TestEqual(TEXT("same material remains assigned"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Fixture.Materials[0]));
    bOk &= TestEqual(TEXT("live material parent replaced"), Fixture.Materials[0]->Parent.Get(), static_cast<UMaterialInterface*>(ReplacementParent));
    bOk &= TestFalse(TEXT("live material two-sided replaced"), Fixture.Materials[0]->BasePropertyOverrides.TwoSided);
    bOk &= TestEqual(TEXT("live material retains one scalar"), Fixture.Materials[0]->ScalarParameterValues.Num(), 1);
    if (Fixture.Materials[0]->ScalarParameterValues.Num() == 1)
        bOk &= TestEqual(TEXT("live scalar replaced"), Fixture.Materials[0]->ScalarParameterValues[0].ParameterValue, 0.625f);
    bOk &= TestTrue(TEXT("mesh remains registered"), Mesh->IsRegistered());
    if (!GUsingNullRHI)
    {
        bOk &= TestTrue(TEXT("render state restored after mutation"), Mesh->IsRenderStateCreated());
        bOk &= TestNotNull(TEXT("scene proxy restored after mutation"), Mesh->GetSceneProxy());
    }
    else AddInfo(TEXT("NullRHI: scene proxy assertions require the D3D12 run"));
    return bOk;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHMaterialForceDuplicateSourceTest,
    "Mimir.V4.Material.BulkForceReimport.DuplicateSourceBlocked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialForceDuplicateSourceTest::RunTest(const FString& Parameters)
{
    FMaterialBatchFixture Fixture;
    if (!Fixture.Build(*this, 2)) return false;
    const FString Name = Fixture.Materials[0]->GetName();
    const FString BackupDirectory = FPaths::Combine(Fixture.Root, TEXT("backup"));
    IFileManager::Get().MakeDirectory(*BackupDirectory, true);
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Fixture.Root, Name + TEXT(".material"))) ||
        !FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(BackupDirectory, Name + TEXT(".material")))) return false;
    // A byte-identical backup is still a second source candidate. It must not
    // silently win or disappear merely because only two canonical targets were selected.
    MHShutdownProjectIndex();
    MHResetSourceImportMetrics();
    TArray<FMHMaterialReimportResult> Results;
    FString Error;
    const bool bSucceeded = MHReimportMaterialsFromSource(Fixture.Materials,
        Fixture.Root, *Fixture.Settings, Results, Error);
    bool bOk = TestFalse(TEXT("duplicate source makes aggregate result fail"), bSucceeded);
    if (!TestEqual(TEXT("both selected targets reported"), Results.Num(), 2)) return false;
    bOk &= TestTrue(TEXT("duplicate diagnosed by shared full snapshot"),
        Results[0].Error.StartsWith(TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME")));
    bOk &= TestFalse(TEXT("ambiguous target never reported successful"), Results[0].bSucceeded);
    bOk &= TestNull(TEXT("ambiguous target parent never mutated"), Fixture.Materials[0]->Parent.Get());
    bOk &= TestFalse(TEXT("ambiguous target never saved"), IFileManager::Get().FileExists(
        *FPackageName::LongPackageNameToFilename(Fixture.Materials[0]->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension())));
    bOk &= TestTrue(TEXT("unambiguous selected target still completes"), Results[1].bSucceeded);
    bOk &= Fixture.CheckMaterial(*this, Fixture.Materials[1]);
    bOk &= CheckBatchMetrics(*this, 1);
    const TSharedPtr<FMHProjectResourceIndex> Index = MHPeekProjectIndex();
    if (!TestTrue(TEXT("index available"), Index.IsValid())) return false;
    bOk &= TestEqual(TEXT("one full scan sees backup and originals"), Index->GetFullScanCountForTests(), 1);
    return bOk;
}
} // namespace UE::MimirComposite::Tests
