#include "HAL/FileManager.h"
#include "Material/MHMaterialDocumentExport.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialProtocol.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceImporter.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

UMaterial* OverrideTestParent(const FString& Root, const FString& Token)
{
    UPackage* Package = CreatePackage(*(Root / Token));
    if (UMaterial* Existing = FindObject<UMaterial>(Package, *Token)) return Existing;
    return NewObject<UMaterial>(Package, FName(*Token), RF_Public | RF_Standalone);
}

TArray<uint8> OverrideUtf8(const TCHAR* Text)
{
    const FTCHARToUTF8 Converted(Text);
    return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
}

FMHSourceAnalysisEntry OverrideEntry(const FString& Name, const FMHResolveOutcome& Outcome)
{
    FMHSourceAnalysisEntry Entry;
    Entry.Key.Kind = EMHResourceKind::Material;
    Entry.Key.LogicalName = Name;
    Entry.PayloadPath = Outcome.PayloadPath;
    Entry.SourcePath = Name + TEXT(".material");
    Entry.RawHash = Outcome.RawHash;
    Entry.Change = EMHSourceChange::Create;
    return Entry;
}

float ScalarOf(const UMaterialInstanceConstant& Material, const TCHAR* Name)
{
    for (const FScalarParameterValue& Value : Material.ScalarParameterValues)
    {
        if (Value.ParameterInfo.Name == FName(Name)) return Value.ParameterValue;
    }
    return -1.0f;
}

} // namespace

// Owner workflow (2026-09-03): export the live MI of material A into the
// source document of managed material B, then Import Changed must overwrite
// MI_B from that document.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialExportOverrideTest,
    "Mimir.V5.Material.ExportOverridesManagedSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialExportOverrideTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower().Left(8);
    const FString SourceRoot = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mimir/MaterialExportOverride_") + Suffix));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString NameA = TEXT("exp_override_a_") + Suffix;
    const FString NameB = TEXT("exp_override_b_") + Suffix;
    const FString PathA = FPaths::Combine(SourceRoot, NameA + TEXT(".material"));
    const FString PathB = FPaths::Combine(SourceRoot, NameB + TEXT(".material"));
    bool bPassed = TestTrue(TEXT("a.material written"), FFileHelper::SaveArrayToFile(
        OverrideUtf8(TEXT("{\"class\":\"simple\",\"params\":{\"roughness\":0.25,\"metallic\":1.0}}")), *PathA));
    bPassed &= TestTrue(TEXT("b.material written"), FFileHelper::SaveArrayToFile(
        OverrideUtf8(TEXT("{\"class\":\"simple\",\"params\":{\"roughness\":0.75}}")), *PathB));

    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FDirectoryPath PreviousRoot = Settings->SourceRoot;
    const FString PreviousMaster = Settings->MasterRoot;
    const FString PreviousLibrary = Settings->LibraryRoot;
    Settings->SourceRoot.Path = SourceRoot;
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    ON_SCOPE_EXIT
    {
        Settings->SourceRoot = PreviousRoot;
        Settings->MasterRoot = PreviousMaster;
        Settings->LibraryRoot = PreviousLibrary;
    };
    OverrideTestParent(Settings->MasterRoot, TEXT("simple"));

    FString Error;
    FMHPayloadScanResolver Resolver(SourceRoot);
    if (!TestTrue(TEXT("source scans"), Resolver.Initialize(Error))) { AddError(Error); return false; }
    FMHResourceKey KeyA, KeyB;
    KeyA.Kind = KeyB.Kind = EMHResourceKind::Material;
    KeyA.LogicalName = NameA;
    KeyB.LogicalName = NameB;
    const FMHMaterialOperationResult ImportedA = MHImportMaterialV4(OverrideEntry(NameA, Resolver.Resolve(KeyA)), Resolver, SourceRoot, *Settings);
    const FMHMaterialOperationResult ImportedB = MHImportMaterialV4(OverrideEntry(NameB, Resolver.Resolve(KeyB)), Resolver, SourceRoot, *Settings);
    if (!TestTrue(TEXT("A imports: ") + ImportedA.Error, ImportedA.Succeeded()) ||
        !TestTrue(TEXT("B imports: ") + ImportedB.Error, ImportedB.Succeeded())) return false;
    UMaterialInstanceConstant* MaterialA = ImportedA.Material;
    UMaterialInstanceConstant* MaterialB = ImportedB.Material;
    if (!TestNotNull(TEXT("MI_A"), MaterialA) || !TestNotNull(TEXT("MI_B"), MaterialB)) return false;
    bPassed &= TestEqual(TEXT("MI_B starts with its own roughness"), ScalarOf(*MaterialB, TEXT("roughness")), 0.75f);

    // Export the live MI_A into B's source document (destination inside Source Root).
    FMHMaterialDocumentExportPlan Plan;
    Error.Reset();
    bPassed &= TestTrue(TEXT("export A -> b.material prepares: ") + Error,
        MHPrepareMaterialDocumentExport({{MaterialA, PathB}}, *Settings, SourceRoot, Plan, Error));
    for (const FMHMaterialDocumentExportFailure& Skipped : Plan.Skipped) AddError(Skipped.Error);
    bPassed &= TestEqual(TEXT("b.material is an overwrite"), Plan.OverwritePaths.Num(), 1);
    FMHMaterialDocumentExportResult ExportResult;
    Error.Reset();
    bPassed &= TestTrue(TEXT("export A -> b.material commits: ") + Error,
        MHCommitMaterialDocumentExport(Plan, true, ExportResult, Error));
    for (const FMHMaterialDocumentExportFailure& Failed : ExportResult.FailedWrites) AddError(Failed.Error);
    bPassed &= TestEqual(TEXT("one document exported"), ExportResult.ExportedCount, 1);

    // Import Changed (same coordinator as the MH Source Tool menu).
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    FMHImportSourcesScope Scope;
    Scope.ResourceKeys.Add(KeyB);
    const bool bImported = MHImportSourcesHeadless(SourceRoot, Scope, *Settings, Analysis, bExecuted);
    for (const FString& Warning : Analysis.Warnings) AddInfo(Warning);
    for (const FString& ImportError : Analysis.Errors) AddInfo(TEXT("analysis error: ") + ImportError);
    const FMHSourceAnalysisEntry* EntryB = Analysis.Entries.FindByPredicate(
        [&KeyB](const FMHSourceAnalysisEntry& Candidate) { return Candidate.Key == KeyB; });
    if (TestNotNull(TEXT("Import Changed sees B"), EntryB))
    {
        AddInfo(FString::Printf(TEXT("B change=%s errors=%d"), MHSourceChangeLabel(EntryB->Change), EntryB->Errors.Num()));
        for (const FString& EntryError : EntryB->Errors) AddInfo(TEXT("B error: ") + EntryError);
        bPassed &= TestEqual(TEXT("B is planned as reimport"), EntryB->Change, EMHSourceChange::Reimport);
        bPassed &= TestTrue(TEXT("B has no import errors"), EntryB->Errors.IsEmpty());
    }
    else
    {
        bPassed = false;
    }
    bPassed &= TestTrue(TEXT("Import Changed executed a mutation"), bExecuted);
    bPassed &= TestTrue(TEXT("Import Changed coordinator result matches analysis"), bImported == !Analysis.HasErrors());
    bPassed &= TestEqual(TEXT("MI_B now carries A's roughness"), ScalarOf(*MaterialB, TEXT("roughness")), 0.25f);
    bPassed &= TestEqual(TEXT("MI_B now carries A's metallic"), ScalarOf(*MaterialB, TEXT("metallic")), 1.0f);
    const UMHMaterialSourceData* ReceiptB = Cast<UMHMaterialSourceData>(MaterialB->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    if (TestNotNull(TEXT("MI_B receipt"), ReceiptB))
    {
        TArray<uint8> BytesB;
        FFileHelper::LoadFileToArray(BytesB, *PathB);
        bPassed &= TestEqual(TEXT("MI_B receipt follows the exported document"), ReceiptB->SourceHash, MHRawPayloadHash(BytesB));
        bPassed &= TestEqual(TEXT("MI_B keeps its logical name"), ReceiptB->LogicalName, NameB);
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
