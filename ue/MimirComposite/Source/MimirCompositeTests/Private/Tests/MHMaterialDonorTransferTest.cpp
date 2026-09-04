#include "Material/MHMaterialDonorTransfer.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialProtocol.h"
#include "Material/MHMaterialSourceData.h"
#include "Material/MHUnrealMaterialDocument.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceComposition.h"
#include "StaticParameterSet.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FDonorTestFiles
{
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Mimir/MaterialDonorTransfer"), Suffix));

    FDonorTestFiles() { IFileManager::Get().MakeDirectory(*Root, true); }
    ~FDonorTestFiles() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
};

template <typename T>
T* DonorTestAsset(const FString& PackageRoot, const FString& Name)
{
    UPackage* Package = CreatePackage(*(PackageRoot / Name));
    return NewObject<T>(Package, FName(*Name), RF_Public | RF_Standalone);
}

UMaterialInstanceConstant* DonorTestMaterial(
    const FString& PackageRoot, const FString& Name, UMaterialInterface* Parent)
{
    UMaterialInstanceConstant* Material = DonorTestAsset<UMaterialInstanceConstant>(PackageRoot, Name);
    Material->SetParentEditorOnly(Parent);
    return Material;
}

TArray<uint8> DonorUtf8(const FString& Value)
{
    const FTCHARToUTF8 Utf8(*Value);
    return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

bool WriteDonorSource(const FString& Path, const FString& Class, float Roughness = 0.9f)
{
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    FMHMaterialDocument Document;
    Document.Parent = Class;
    FMHMaterialParameter Value;
    Value.Scalar = Roughness;
    Document.Params.Add(TEXT("old_roughness"), Value);
    TArray<uint8> Bytes;
    FString Error;
    return MHWriteCanonicalMaterialV4(Document, Bytes, Error) && FFileHelper::SaveArrayToFile(Bytes, *Path);
}

bool DonorSnapshot(const UMaterialInstanceConstant& Material, TArray<uint8>& Bytes, FString& Error)
{
    FMHMaterialDocument Document;
    return MHExtractUnrealMaterialV1(Material, Document, Error) &&
        MHWriteCanonicalMaterialV4(Document, Bytes, Error);
}

float DonorScalar(const UMaterialInstanceConstant& Material, const TCHAR* Name)
{
    for (const FScalarParameterValue& Value : Material.ScalarParameterValues)
    {
        if (Value.ParameterInfo.Name == FName(Name)) return Value.ParameterValue;
    }
    return -1000.0f;
}

void DeleteDonorTestPackage(const FString& PackageName)
{
    const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
    if (UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
    {
        ObjectTools::DeleteSingleObject(Asset, false);
    }
    if (UPackage* Package = FindPackage(nullptr, *PackageName)) Package->SetDirtyFlag(false);
    IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension()), false, true, true);
}

FMHMaterialOperationResult ImportDonorTestSource(
    const FString& Name, const FString& RelativePath, FMHPayloadScanResolver& Resolver,
    const FString& Root, const UMHCompositeSettings& Settings)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Material;
    Key.LogicalName = Name;
    const FMHResolveOutcome Resolved = Resolver.Resolve(Key);
    FMHSourceAnalysisEntry Entry;
    Entry.Key = Key;
    Entry.PayloadPath = Resolved.PayloadPath;
    Entry.SourcePath = RelativePath;
    Entry.RawHash = Resolved.RawHash;
    Entry.Change = EMHSourceChange::Create;
    return MHImportMaterialV4(Entry, Resolver, Root, Settings);
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialDonorBatchRoundTripTest,
    "Mimir.V5.Material.DonorTransfer.BatchRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialDonorBatchRoundTripTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FDonorTestFiles Files;
    const FString AssetRoot = TEXT("/Game/MHMaterialDonorTests/") + Files.Suffix;
    const FString NameA = TEXT("donor_house_a_") + Files.Suffix;
    const FString NameB = TEXT("donor_house_b_") + Files.Suffix;
    const FString NameUntouched = TEXT("donor_other_") + Files.Suffix;
    const FString RelativeA = TEXT("houses/nested/") + NameA + TEXT(".material");
    const FString RelativeB = NameB + TEXT(".material");
    const FString RelativeUntouched = NameUntouched + TEXT(".material");
    const FString NewFolder = Files.Root / TEXT("new");
    const FString ParentName = TEXT("original_") + Files.Suffix;
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FDirectoryPath PreviousSourceRoot = Settings->SourceRoot;
    const FString PreviousMasterRoot = Settings->MasterRoot;
    const FString PreviousLibraryRoot = Settings->LibraryRoot;
    Settings->SourceRoot.Path = Files.Root;
    Settings->MasterRoot = AssetRoot / TEXT("Masters");
    Settings->LibraryRoot = AssetRoot / TEXT("Library");
    TArray<FString> TestPackages;
    UStaticMesh* ReferencingMesh = nullptr;
    ON_SCOPE_EXIT
    {
        if (ReferencingMesh != nullptr) ReferencingMesh->GetStaticMaterials().Reset();
        MHShutdownProjectIndex();
        Settings->SourceRoot = PreviousSourceRoot;
        Settings->MasterRoot = PreviousMasterRoot;
        Settings->LibraryRoot = PreviousLibraryRoot;
        for (int32 Index = TestPackages.Num() - 1; Index >= 0; --Index)
        {
            DeleteDonorTestPackage(TestPackages[Index]);
        }
    };
    UMaterial* OriginalParent = DonorTestAsset<UMaterial>(Settings->MasterRoot, ParentName);
    UMaterial* ExternalParent = DonorTestAsset<UMaterial>(AssetRoot / TEXT("Legacy"), TEXT("ExternalMaster"));
    UMaterial* RegisteredParent = DonorTestAsset<UMaterial>(Settings->MasterRoot, TEXT("replacement"));
    UTexture2D* Texture = DonorTestAsset<UTexture2D>(AssetRoot / TEXT("Legacy"), TEXT("ExternalAlbedo"));
    TestPackages.Append({OriginalParent->GetOutermost()->GetName(), ExternalParent->GetOutermost()->GetName(),
        RegisteredParent->GetOutermost()->GetName(), Texture->GetOutermost()->GetName()});
    if (!TestTrue(TEXT("initial A source"), WriteDonorSource(Files.Root / RelativeA, ParentName)) ||
        !TestTrue(TEXT("initial B source"), WriteDonorSource(Files.Root / RelativeB, ParentName)) ||
        !TestTrue(TEXT("unselected source"), WriteDonorSource(Files.Root / RelativeUntouched, ParentName))) return false;

    FString Error;
    FMHPayloadScanResolver InitialResolver(Files.Root);
    if (!TestTrue(TEXT("initial source inventory: ") + Error, InitialResolver.Initialize(Error))) return false;
    const FMHMaterialOperationResult InitialA = ImportDonorTestSource(NameA, RelativeA, InitialResolver, Files.Root, *Settings);
    const FMHMaterialOperationResult InitialB = ImportDonorTestSource(NameB, RelativeB, InitialResolver, Files.Root, *Settings);
    const FMHMaterialOperationResult InitialUntouched = ImportDonorTestSource(
        NameUntouched, RelativeUntouched, InitialResolver, Files.Root, *Settings);
    TestPackages.Append({TEXT("/Game/MH/Generated/Materials/") + NameA,
        TEXT("/Game/MH/Generated/Materials/") + NameB, TEXT("/Game/MH/Generated/Materials/") + NameUntouched});
    if (!TestTrue(TEXT("initial A imports: ") + InitialA.Error, InitialA.Succeeded()) ||
        !TestTrue(TEXT("initial B imports: ") + InitialB.Error, InitialB.Succeeded()) ||
        !TestTrue(TEXT("unselected material imports: ") + InitialUntouched.Error, InitialUntouched.Succeeded())) return false;
    UMaterialInstanceConstant* TargetA = InitialA.Material;
    UMaterialInstanceConstant* TargetB = InitialB.Material;
    const FSoftObjectPath TargetPathA(TargetA);
    const FSoftObjectPath TargetPathB(TargetB);
    ReferencingMesh = NewObject<UStaticMesh>();
    ReferencingMesh->GetStaticMaterials().Add(FStaticMaterial(TargetA));
    ReferencingMesh->GetStaticMaterials().Add(FStaticMaterial(TargetB));
    UMaterialInstanceConstant* DonorA = DonorTestMaterial(AssetRoot / TEXT("Donors"), TEXT("m_") + NameA, ExternalParent);
    UMaterialInstanceConstant* DonorB = DonorTestMaterial(AssetRoot / TEXT("Donors"), TEXT("m_") + NameB, RegisteredParent);
    TestPackages.Append({DonorA->GetOutermost()->GetName(), DonorB->GetOutermost()->GetName()});
    DonorA->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness Scale")), 0.125f);
    DonorA->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Polished Tint")), FLinearColor(0.1f, 0.25f, 0.5f, 0.75f));
    DonorA->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Albedo Map")), Texture);
    FMaterialInstanceBasePropertyOverrides Overrides;
    Overrides.bOverride_TwoSided = true;
    Overrides.TwoSided = true;
    Overrides.bOverride_BlendMode = true;
    Overrides.BlendMode = BLEND_Masked;
    Overrides.bOverride_OpacityMaskClipValue = true;
    Overrides.OpacityMaskClipValue = 0.375f;
    FStaticParameterSet StaticParameters;
    const FGuid SwitchGuid = FGuid::NewGuid();
    const FGuid MaskGuid = FGuid::NewGuid();
    StaticParameters.StaticSwitchParameters.Add(FStaticSwitchParameter(
        FMaterialParameterInfo(TEXT("Use Detail")), true, true, SwitchGuid));
    StaticParameters.EditorOnly.StaticComponentMaskParameters.Add(FStaticComponentMaskParameter(
        FMaterialParameterInfo(TEXT("Channel Mask")), true, false, true, false, true, MaskGuid));
    DonorA->UpdateStaticPermutation(StaticParameters, Overrides);
    DonorB->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("metallic")), 0.625f);
    TArray<uint8> DonorBeforeA, DonorBeforeB;
    if (!TestTrue(TEXT("external donor snapshot: ") + Error, DonorSnapshot(*DonorA, DonorBeforeA, Error)) ||
        !TestTrue(TEXT("registered donor snapshot: ") + Error, DonorSnapshot(*DonorB, DonorBeforeB, Error))) return false;
    const bool bDonorDirtyA = DonorA->GetOutermost()->IsDirty();
    const bool bDonorDirtyB = DonorB->GetOutermost()->IsDirty();
    // Even a changed unrelated source must not be imported by the batch action.
    if (!TestTrue(TEXT("unselected source changes"), WriteDonorSource(Files.Root / RelativeUntouched, ParentName, 0.2f))) return false;

    const TArray<UMaterialInstanceConstant*> Donors = {DonorB, DonorA};
    FMHMaterialDocumentExportPlan Plan;
    if (!TestTrue(TEXT("two donors prepare: ") + Error,
        MHPrepareMaterialDonorTransfer(Donors, Files.Root, NewFolder, Plan, Error))) return false;
    bool bPassed = TestEqual(TEXT("both donors are ready"), Plan.Ready.Num(), 2);
    bPassed &= TestEqual(TEXT("both existing sources will be replaced"), Plan.OverwritePaths.Num(), 2);
    const FMHPreparedMaterialDocumentExport* PlannedA = Plan.Ready.FindByPredicate(
        [&NameA](const FMHPreparedMaterialDocumentExport& Item) { return Item.LogicalName == NameA; });
    bPassed &= TestTrue(TEXT("nested source retains its original path"), PlannedA != nullptr &&
        FPaths::IsSamePath(PlannedA->DestinationPath, Files.Root / RelativeA));
    bPassed &= TestFalse(TEXT("preflight creates no new source directory"), IFileManager::Get().DirectoryExists(*NewFolder));
    FMHMaterialDocumentExportResult Exported;
    if (!TestTrue(TEXT("overwrite batch commits: ") + Error, MHCommitMaterialDocumentExport(Plan, true, Exported, Error))) return false;
    bPassed &= TestEqual(TEXT("both documents written"), Exported.ExportedCount, 2);
    bPassed &= TestFalse(TEXT("no duplicate source at requested new folder"),
        IFileManager::Get().FileExists(*(NewFolder / (NameA + TEXT(".material")))));
    const FMHImportSourcesScope Scope = MHMaterialDonorImportScope(Plan, Exported);
    if (!TestEqual(TEXT("import scope contains exactly both successful targets"), Scope.ResourceKeys.Num(), 2)) return false;
    bPassed &= TestTrue(TEXT("donor transfer requests explicit source-wins reimport"), Scope.bForceMaterialReimport);
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    const bool bImported = MHImportSourcesHeadless(Files.Root, Scope, *Settings, Analysis, bExecuted);
    for (const FString& ImportError : Analysis.Errors) AddInfo(ImportError);
    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
        for (const FString& ImportError : Entry.Errors) AddInfo(ImportError);
    bPassed &= TestTrue(TEXT("batch import succeeds"), bImported && bExecuted && !Analysis.HasErrors());
    bPassed &= TestTrue(TEXT("A retains its UObject identity"), TargetPathA.ResolveObject() == TargetA);
    bPassed &= TestTrue(TEXT("B retains its UObject identity"), TargetPathB.ResolveObject() == TargetB);
    bPassed &= TestTrue(TEXT("mesh slots still reference original targets"),
        ReferencingMesh->GetStaticMaterials()[0].MaterialInterface == TargetA &&
        ReferencingMesh->GetStaticMaterials()[1].MaterialInterface == TargetB);
    bPassed &= TestEqual(TEXT("external master transferred"), TargetA->Parent.Get(), static_cast<UMaterialInterface*>(ExternalParent));
    bPassed &= TestEqual(TEXT("registered replacement master transferred"), TargetB->Parent.Get(), static_cast<UMaterialInterface*>(RegisteredParent));
    bPassed &= TestEqual(TEXT("arbitrary scalar name and value transferred"), DonorScalar(*TargetA, TEXT("Roughness Scale")), 0.125f);
    bPassed &= TestEqual(TEXT("old scalar removed"), TargetA->ScalarParameterValues.Num(), 1);
    bPassed &= TestEqual(TEXT("second donor state transferred"), DonorScalar(*TargetB, TEXT("metallic")), 0.625f);
    bPassed &= TestEqual(TEXT("arbitrary vector preserved"), TargetA->VectorParameterValues.Num(), 1);
    if (TargetA->VectorParameterValues.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("vector name"), TargetA->VectorParameterValues[0].ParameterInfo.Name, FName(TEXT("Polished Tint")));
        bPassed &= TestEqual(TEXT("vector value"), TargetA->VectorParameterValues[0].ParameterValue, FLinearColor(0.1f, 0.25f, 0.5f, 0.75f));
    }
    bPassed &= TestEqual(TEXT("arbitrary texture preserved"), TargetA->TextureParameterValues.Num(), 1);
    if (TargetA->TextureParameterValues.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("texture name"), TargetA->TextureParameterValues[0].ParameterInfo.Name, FName(TEXT("Albedo Map")));
        bPassed &= TestEqual(TEXT("external texture object"), TargetA->TextureParameterValues[0].ParameterValue.Get(), static_cast<UTexture*>(Texture));
    }
    bPassed &= TestTrue(TEXT("base properties transferred"), TargetA->BasePropertyOverrides.bOverride_TwoSided &&
        TargetA->BasePropertyOverrides.TwoSided && TargetA->BasePropertyOverrides.bOverride_BlendMode &&
        TargetA->BasePropertyOverrides.BlendMode == BLEND_Masked &&
        TargetA->BasePropertyOverrides.bOverride_OpacityMaskClipValue &&
        TargetA->BasePropertyOverrides.OpacityMaskClipValue == 0.375f);
    const FStaticParameterSet TargetStatic = TargetA->GetStaticParameters();
    const FStaticSwitchParameter* Switch = TargetStatic.StaticSwitchParameters.FindByPredicate(
        [](const FStaticSwitchParameter& Value) { return Value.ParameterInfo.Name == FName(TEXT("Use Detail")); });
    const FStaticComponentMaskParameter* Mask = TargetStatic.EditorOnly.StaticComponentMaskParameters.FindByPredicate(
        [](const FStaticComponentMaskParameter& Value) { return Value.ParameterInfo.Name == FName(TEXT("Channel Mask")); });
    bPassed &= TestTrue(TEXT("static switch and expression identity preserved"),
        Switch != nullptr && Switch->bOverride && Switch->Value && Switch->ExpressionGUID == SwitchGuid);
    bPassed &= TestTrue(TEXT("static component mask and expression identity preserved"), Mask != nullptr &&
        Mask->bOverride && Mask->R && !Mask->G && Mask->B && !Mask->A && Mask->ExpressionGUID == MaskGuid);
    bPassed &= TestEqual(TEXT("unselected material remains unchanged"), DonorScalar(*InitialUntouched.Material, TEXT("old_roughness")), 0.9f);

    TArray<uint8> TargetBytesA, TargetBytesB, DonorAfterA, DonorAfterB, WrittenA;
    bPassed &= TestTrue(TEXT("target A snapshot"), DonorSnapshot(*TargetA, TargetBytesA, Error));
    bPassed &= TestTrue(TEXT("target B snapshot"), DonorSnapshot(*TargetB, TargetBytesB, Error));
    bPassed &= TestTrue(TEXT("complete supported A state roundtrips"), TargetBytesA == DonorBeforeA);
    bPassed &= TestTrue(TEXT("complete supported B state roundtrips"), TargetBytesB == DonorBeforeB);
    bPassed &= TestTrue(TEXT("donor A snapshot after"), DonorSnapshot(*DonorA, DonorAfterA, Error));
    bPassed &= TestTrue(TEXT("donor B snapshot after"), DonorSnapshot(*DonorB, DonorAfterB, Error));
    bPassed &= TestTrue(TEXT("donors remain byte-identical"), DonorAfterA == DonorBeforeA && DonorAfterB == DonorBeforeB);
    bPassed &= TestTrue(TEXT("donor dirty state remains unchanged"),
        DonorA->GetOutermost()->IsDirty() == bDonorDirtyA && DonorB->GetOutermost()->IsDirty() == bDonorDirtyB);
    bPassed &= TestTrue(TEXT("donors remain unmanaged"),
        DonorA->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()) == nullptr &&
        DonorB->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()) == nullptr);
    const UMHMaterialSourceData* ReceiptA = Cast<UMHMaterialSourceData>(TargetA->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    bPassed &= TestTrue(TEXT("exported source can be read"), FFileHelper::LoadFileToArray(WrittenA, *(Files.Root / RelativeA)));
    bPassed &= TestTrue(TEXT("target keeps logical name and nested source identity with advanced hash"), ReceiptA != nullptr &&
        ReceiptA->LogicalName == NameA && ReceiptA->SourceRelativePath == RelativeA &&
        ReceiptA->SourceHash == MHRawPayloadHash(WrittenA));

    // Repeating the same export must repair a locally modified target even
    // though the source hash is unchanged. Ordinary NoChange must not hide it.
    TargetA->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness Scale")), 0.99f);
    TargetA->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("local_only")), 7.0f);
    FMHMaterialDocumentExportPlan RepeatPlan;
    FMHMaterialDocumentExportResult RepeatExport;
    if (!TestTrue(TEXT("identical export prepares"), MHPrepareMaterialDonorTransfer(Donors, Files.Root, NewFolder, RepeatPlan, Error)) ||
        !TestTrue(TEXT("identical export commits"), MHCommitMaterialDocumentExport(RepeatPlan, true, RepeatExport, Error))) return false;
    TArray<uint8> RepeatedBytes;
    FFileHelper::LoadFileToArray(RepeatedBytes, *(Files.Root / RelativeA));
    bPassed &= TestTrue(TEXT("repeat source bytes are unchanged"), RepeatedBytes == WrittenA);
    const FMHImportSourcesScope RepeatScope = MHMaterialDonorImportScope(RepeatPlan, RepeatExport);
    if (!TestEqual(TEXT("repeat scope stays targeted"), RepeatScope.ResourceKeys.Num(), 2)) return false;
    FMHSourceAnalysis RepeatAnalysis;
    bExecuted = false;
    bPassed &= TestTrue(TEXT("equal-hash donor import executes"),
        MHImportSourcesHeadless(Files.Root, RepeatScope, *Settings, RepeatAnalysis, bExecuted) && bExecuted);
    bPassed &= TestEqual(TEXT("equal-hash reimport repairs polished scalar"), DonorScalar(*TargetA, TEXT("Roughness Scale")), 0.125f);
    bPassed &= TestEqual(TEXT("equal-hash reimport clears local-only override"), TargetA->ScalarParameterValues.Num(), 1);
    bPassed &= TestEqual(TEXT("repeat does not import unrelated changed source"), DonorScalar(*InitialUntouched.Material, TEXT("old_roughness")), 0.9f);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialDonorAdmissionTest,
    "Mimir.V5.Material.DonorTransfer.StrictAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialDonorAdmissionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FDonorTestFiles Files;
    const FString AssetRoot = TEXT("/Game/MHMaterialDonorAdmission/") + Files.Suffix;
    UMaterial* Parent = DonorTestAsset<UMaterial>(AssetRoot, TEXT("ExternalMaster"));
    TArray<FString> TestPackages = {Parent->GetOutermost()->GetName()};
    ON_SCOPE_EXIT
    {
        for (int32 Index = TestPackages.Num() - 1; Index >= 0; --Index) DeleteDonorTestPackage(TestPackages[Index]);
    };
    auto Make = [&](const FString& Folder, const FString& Name)
    {
        UMaterialInstanceConstant* Material = DonorTestMaterial(AssetRoot / Folder, Name, Parent);
        TestPackages.Add(Material->GetOutermost()->GetName());
        return Material;
    };
    UMaterialInstanceConstant* Good = Make(TEXT("a"), TEXT("m_house"));
    bool bPassed = true;
    FString Error;
    const FString NewFolder = Files.Root / TEXT("new");
    auto Rejected = [&](const FString& Label, const TArray<UMaterialInstanceConstant*>& Selection)
    {
        FMHMaterialDocumentExportPlan Plan;
        const bool bPrepared = MHPrepareMaterialDonorTransfer(Selection, Files.Root, NewFolder, Plan, Error);
        bPassed &= TestFalse(Label + TEXT(" rejects"), bPrepared);
        bPassed &= TestTrue(Label + TEXT(" provides diagnostic"), !Error.IsEmpty());
        bPassed &= TestTrue(Label + TEXT(" clears every ready peer"), Plan.Ready.IsEmpty() && Plan.OverwritePaths.IsEmpty());
        bPassed &= TestFalse(Label + TEXT(" writes no directory"), IFileManager::Get().DirectoryExists(*NewFolder));
    };
    Rejected(TEXT("empty selection"), {});
    Rejected(TEXT("missing prefix"), {Good, Make(TEXT("missing"), TEXT("house"))});
    Rejected(TEXT("case-sensitive prefix"), {Good, Make(TEXT("upperprefix"), TEXT("M_other"))});
    Rejected(TEXT("noncanonical remainder"), {Good, Make(TEXT("badname"), TEXT("m_UpperCase"))});
    Rejected(TEXT("empty remainder"), {Good, Make(TEXT("empty"), TEXT("m_"))});
    Rejected(TEXT("duplicate output names"), {Good, Make(TEXT("duplicate"), TEXT("m_house"))});
    Rejected(TEXT("null peer"), {Good, nullptr});
    UMaterialInstanceConstant* Unsupported = Make(TEXT("unsupported"), TEXT("m_font"));
    Unsupported->FontParameterValues.AddDefaulted();
    Rejected(TEXT("unsupported font state"), {Good, Unsupported});
    UMaterialInstanceConstant* Atlas = Make(TEXT("atlas"), TEXT("m_atlas"));
    Atlas->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("atlas_position")), 0.5f);
    Atlas->ScalarParameterValues[0].AtlasData.bIsUsedAsAtlasPosition = true;
    Rejected(TEXT("unsupported scalar atlas state"), {Good, Atlas});
    const FString OverlapName = TEXT("m_overlap_") + Files.Suffix;
    UMaterialInstanceConstant* OverlapTarget = DonorTestMaterial(
        TEXT("/Game/MH/Generated/Materials"), OverlapName, Parent);
    TestPackages.Add(OverlapTarget->GetOutermost()->GetName());
    Rejected(TEXT("selected donor is another donor's target"),
        {OverlapTarget, Make(TEXT("overlap"), TEXT("m_") + OverlapName)});

    UMaterialInstanceConstant* RepeatedPrefix = Make(TEXT("twoprefixes"), TEXT("m_m_roof"));
    UMaterialInstanceConstant* InternalPrefix = Make(TEXT("internal"), TEXT("m_trim_m_panel"));
    FMHMaterialDocumentExportPlan PrefixPlan;
    const TArray<UMaterialInstanceConstant*> PrefixDonors = {RepeatedPrefix, InternalPrefix};
    bPassed &= TestTrue(TEXT("valid repeated and internal prefixes prepare"),
        MHPrepareMaterialDonorTransfer(PrefixDonors, Files.Root, NewFolder, PrefixPlan, Error));
    bPassed &= TestTrue(TEXT("exactly one leading prefix removed"), PrefixPlan.Ready.ContainsByPredicate(
        [](const FMHPreparedMaterialDocumentExport& Item) { return Item.LogicalName == TEXT("m_roof"); }));
    bPassed &= TestTrue(TEXT("internal prefix preserved"), PrefixPlan.Ready.ContainsByPredicate(
        [](const FMHPreparedMaterialDocumentExport& Item) { return Item.LogicalName == TEXT("trim_m_panel"); }));
    bPassed &= TestTrue(TEXT("new outputs use chosen folder"), PrefixPlan.Ready.Num() == 2 &&
        FPaths::IsSamePath(PrefixPlan.Ready[0].DestinationPath, NewFolder / TEXT("m_roof.material")) &&
        FPaths::IsSamePath(PrefixPlan.Ready[1].DestinationPath, NewFolder / TEXT("trim_m_panel.material")));

    const FString ExistingPath = Files.Root / TEXT("nested/house.material");
    bPassed &= TestTrue(TEXT("existing nested source fixture"), WriteDonorSource(ExistingPath, TEXT("simple")));
    FMHMaterialDocumentExportPlan NestedPlan;
    const TArray<UMaterialInstanceConstant*> OneDonor = {Good};
    // Name mapping is deliberately independent of source receipts. Keep this
    // malformed-receipt fixture inside read-only preflight and detach it before
    // any coordinator can scan managed receipts from the live asset registry.
    UMHMaterialSourceData* DonorReceipt = NewObject<UMHMaterialSourceData>(Good);
    DonorReceipt->LogicalName = TEXT("do_not_use_this_donor_receipt");
    Good->AddAssetUserData(DonorReceipt);
    bPassed &= TestTrue(TEXT("unique nested existing source prepares"),
        MHPrepareMaterialDonorTransfer(OneDonor, Files.Root, NewFolder, NestedPlan, Error));
    bPassed &= TestTrue(TEXT("unique nested source wins over chosen new folder"), NestedPlan.Ready.Num() == 1 &&
        NestedPlan.Ready[0].LogicalName == TEXT("house") && FPaths::IsSamePath(NestedPlan.Ready[0].DestinationPath, ExistingPath));
    bPassed &= TestTrue(TEXT("preflight preserves attached donor receipt"),
        Good->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()) == DonorReceipt &&
        DonorReceipt->LogicalName == TEXT("do_not_use_this_donor_receipt"));
    Good->RemoveUserDataOfClass(UMHMaterialSourceData::StaticClass());
    bPassed &= TestTrue(TEXT("duplicate source fixture"), WriteDonorSource(Files.Root / TEXT("other/house.material"), TEXT("simple")));
    Rejected(TEXT("ambiguous existing sources"), {Good});
    TArray<uint8> ExistingAfter;
    bPassed &= TestTrue(TEXT("source remains readable after rejected preflight"), FFileHelper::LoadFileToArray(ExistingAfter, *ExistingPath));
    FMHMaterialDocument ParsedExisting;
    bPassed &= TestTrue(TEXT("rejected source remains original class document"),
        MHParseMaterialV4(ExistingAfter, ParsedExisting, Error) && ParsedExisting.Mode == EMHMaterialMode::Class);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialDonorWriteRaceTest,
    "Mimir.V5.Material.DonorTransfer.WriteRaceAndScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialDonorWriteRaceTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FDonorTestFiles Files;
    const FString AssetRoot = TEXT("/Game/MHMaterialDonorRace/") + Files.Suffix;
    UMaterial* Parent = DonorTestAsset<UMaterial>(AssetRoot, TEXT("ExternalMaster"));
    UMaterialInstanceConstant* Changed = DonorTestMaterial(AssetRoot, TEXT("m_changed"), Parent);
    UMaterialInstanceConstant* Successful = DonorTestMaterial(AssetRoot, TEXT("m_successful"), Parent);
    UMaterialInstanceConstant* Appeared = DonorTestMaterial(AssetRoot, TEXT("m_appeared"), Parent);
    ON_SCOPE_EXIT
    {
        DeleteDonorTestPackage(Appeared->GetOutermost()->GetName());
        DeleteDonorTestPackage(Successful->GetOutermost()->GetName());
        DeleteDonorTestPackage(Changed->GetOutermost()->GetName());
        DeleteDonorTestPackage(Parent->GetOutermost()->GetName());
    };
    const FString ChangedPath = Files.Root / TEXT("changed.material");
    if (!TestTrue(TEXT("existing source"), WriteDonorSource(ChangedPath, TEXT("simple")))) return false;
    const TArray<UMaterialInstanceConstant*> Donors = {Changed, Successful, Appeared};
    FMHMaterialDocumentExportPlan Plan;
    FString Error;
    if (!TestTrue(TEXT("race batch prepares"), MHPrepareMaterialDonorTransfer(Donors, Files.Root, Files.Root, Plan, Error))) return false;
    const TArray<uint8> ConcurrentBytes = DonorUtf8(TEXT("{\"class\":\"concurrent_edit\"}\n"));
    bool bPassed = TestTrue(TEXT("existing source concurrently edited"), FFileHelper::SaveArrayToFile(ConcurrentBytes, *ChangedPath));
    FMHMaterialDocumentExportResult Result;
    bPassed &= TestFalse(TEXT("race reports partial write failure"), MHCommitMaterialDocumentExport(Plan, true, Result, Error));
    bPassed &= TestEqual(TEXT("only uncontested peers exported"), Result.ExportedCount, 2);
    bPassed &= TestEqual(TEXT("edited source rejected"), Result.FailedWrites.Num(), 1);
    TArray<uint8> ChangedAfter;
    FFileHelper::LoadFileToArray(ChangedAfter, *ChangedPath);
    bPassed &= TestTrue(TEXT("concurrent file contents preserved"), ChangedAfter == ConcurrentBytes);
    const FMHImportSourcesScope Scope = MHMaterialDonorImportScope(Plan, Result);
    bPassed &= TestTrue(TEXT("only successful outputs enter import scope"), Scope.ResourceKeys.Num() == 2 &&
        !Scope.ResourceKeys.ContainsByPredicate([](const FMHResourceKey& Key) { return Key.LogicalName == TEXT("changed"); }));
    FMHMaterialDocumentExportResult Empty;
    bPassed &= TestTrue(TEXT("no successful writes means empty scope"), MHMaterialDonorImportScope(Plan, Empty).ResourceKeys.IsEmpty());
    Result.bCancelled = true;
    bPassed &= TestTrue(TEXT("cancelled result never imports peers"), MHMaterialDonorImportScope(Plan, Result).ResourceKeys.IsEmpty());

    // A newly appearing logical name changes the source mapping itself. It
    // must block every peer before commit, including otherwise valid outputs.
    FDonorTestFiles MappingFiles;
    FMHMaterialDocumentExportPlan MappingPlan;
    bPassed &= TestTrue(TEXT("new-source mapping prepares"),
        MHPrepareMaterialDonorTransfer(Donors, MappingFiles.Root, MappingFiles.Root, MappingPlan, Error));
    const FString AppearedPath = MappingFiles.Root / TEXT("elsewhere/appeared.material");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(AppearedPath), true);
    bPassed &= TestTrue(TEXT("source appears elsewhere after preflight"), FFileHelper::SaveArrayToFile(ConcurrentBytes, *AppearedPath));
    FMHMaterialDocumentExportResult MappingResult;
    bPassed &= TestFalse(TEXT("mapping race blocks the batch"), MHCommitMaterialDocumentExport(MappingPlan, true, MappingResult, Error));
    bPassed &= TestEqual(TEXT("mapping race exports no peer"), MappingResult.ExportedCount, 0);
    bPassed &= TestFalse(TEXT("valid peer remains unwritten"), IFileManager::Get().FileExists(*(MappingFiles.Root / TEXT("successful.material"))));
    TArray<uint8> AppearedAfter;
    FFileHelper::LoadFileToArray(AppearedAfter, *AppearedPath);
    bPassed &= TestTrue(TEXT("new concurrent source preserved"), AppearedAfter == ConcurrentBytes);
    bPassed &= TestTrue(TEXT("blocked mapping has empty import scope"), MHMaterialDonorImportScope(MappingPlan, MappingResult).ResourceKeys.IsEmpty());
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialDonorParentOrderTest,
    "Mimir.V5.Material.DonorTransfer.ProspectiveParentOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialDonorParentOrderTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FDonorTestFiles Files;
    const FString AssetRoot = TEXT("/Game/MHMaterialDonorParents/") + Files.Suffix;
    const FString NameA = TEXT("parent_a_") + Files.Suffix;
    const FString NameB = TEXT("parent_b_") + Files.Suffix;
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FDirectoryPath PreviousRoot = Settings->SourceRoot;
    const FString PreviousMaster = Settings->MasterRoot;
    const FString PreviousLibrary = Settings->LibraryRoot;
    Settings->SourceRoot.Path = Files.Root;
    Settings->MasterRoot = AssetRoot / TEXT("Masters");
    Settings->LibraryRoot = AssetRoot / TEXT("Library");
    TArray<FString> Packages;
    ON_SCOPE_EXIT
    {
        MHShutdownProjectIndex();
        Settings->SourceRoot = PreviousRoot;
        Settings->MasterRoot = PreviousMaster;
        Settings->LibraryRoot = PreviousLibrary;
        for (int32 Index = Packages.Num() - 1; Index >= 0; --Index) DeleteDonorTestPackage(Packages[Index]);
    };
    UMaterial* Master = DonorTestAsset<UMaterial>(Settings->MasterRoot, TEXT("simple"));
    Packages.Add(Master->GetOutermost()->GetName());
    if (!TestTrue(TEXT("A original source"), WriteDonorSource(Files.Root / (NameA + TEXT(".material")), TEXT("simple"))) ||
        !TestTrue(TEXT("B original source"), WriteDonorSource(Files.Root / (NameB + TEXT(".material")), TEXT("simple")))) return false;
    FMHPayloadScanResolver Resolver(Files.Root);
    FString Error;
    if (!TestTrue(TEXT("parent fixture inventory"), Resolver.Initialize(Error))) return false;
    const FMHMaterialOperationResult ImportedA = ImportDonorTestSource(NameA, NameA + TEXT(".material"), Resolver, Files.Root, *Settings);
    const FMHMaterialOperationResult ImportedB = ImportDonorTestSource(NameB, NameB + TEXT(".material"), Resolver, Files.Root, *Settings);
    Packages.Append({TEXT("/Game/MH/Generated/Materials/") + NameA, TEXT("/Game/MH/Generated/Materials/") + NameB});
    if (!TestTrue(TEXT("A initial import: ") + ImportedA.Error, ImportedA.Succeeded()) ||
        !TestTrue(TEXT("B initial import: ") + ImportedB.Error, ImportedB.Succeeded())) return false;
    UMaterialInstanceConstant* TargetA = ImportedA.Material;
    UMaterialInstanceConstant* TargetB = ImportedB.Material;
    UMaterialInstanceConstant* DonorA = DonorTestMaterial(AssetRoot, TEXT("m_") + NameA, TargetB);
    UMaterialInstanceConstant* DonorB = DonorTestMaterial(AssetRoot, TEXT("m_") + NameB, TargetA);
    Packages.Append({DonorA->GetOutermost()->GetName(), DonorB->GetOutermost()->GetName()});
    const TArray<UMaterialInstanceConstant*> Donors = {DonorA, DonorB};
    FMHMaterialDocumentExportPlan CyclicPlan;
    bool bPassed = TestFalse(TEXT("prospective A -> B -> A cycle rejected"),
        MHPrepareMaterialDonorTransfer(Donors, Files.Root, Files.Root, CyclicPlan, Error));
    bPassed &= TestTrue(TEXT("cycle rejection clears full batch"), CyclicPlan.Ready.IsEmpty());
    bPassed &= TestTrue(TEXT("cycle rejection preserves live target parents"), TargetA->Parent.Get() == Master && TargetB->Parent.Get() == Master);

    // Existing B -> A must be broken before applying new A -> B. The final
    // graph A -> B -> Master is valid even though applying A first is cyclic.
    DonorB->SetParentEditorOnly(Master);
    TargetB->SetParentEditorOnly(TargetA);
    DonorA->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("a_polish")), 0.25f);
    DonorB->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("b_polish")), 0.75f);
    FMHMaterialDocumentExportPlan Plan;
    if (!TestTrue(TEXT("acyclic final graph prepares: ") + Error,
        MHPrepareMaterialDonorTransfer(Donors, Files.Root, Files.Root, Plan, Error))) return false;
    bPassed &= TestTrue(TEXT("parent-changing B is ordered before A"), Plan.Ready.Num() == 2 &&
        Plan.Ready[0].LogicalName == NameB && Plan.Ready[1].LogicalName == NameA);
    FMHMaterialDocumentExportResult Exported;
    if (!TestTrue(TEXT("parent-order documents commit: ") + Error, MHCommitMaterialDocumentExport(Plan, true, Exported, Error))) return false;
    const FMHImportSourcesScope Scope = MHMaterialDonorImportScope(Plan, Exported);
    if (!TestTrue(TEXT("import scope retains safe B then A order"), Scope.ResourceKeys.Num() == 2 &&
        Scope.ResourceKeys[0].LogicalName == NameB && Scope.ResourceKeys[1].LogicalName == NameA)) return false;
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    const bool bImported = MHImportSourcesHeadless(Files.Root, Scope, *Settings, Analysis, bExecuted);
    for (const FString& ImportError : Analysis.Errors) AddInfo(ImportError);
    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
        for (const FString& ImportError : Entry.Errors) AddInfo(ImportError);
    bPassed &= TestTrue(TEXT("safe parent-order import succeeds"), bImported && bExecuted && !Analysis.HasErrors());
    bPassed &= TestTrue(TEXT("final target chain is A -> B -> Master"), TargetA->Parent.Get() == TargetB && TargetB->Parent.Get() == Master);
    bPassed &= TestEqual(TEXT("A donor state applied"), DonorScalar(*TargetA, TEXT("a_polish")), 0.25f);
    bPassed &= TestEqual(TEXT("B donor state applied"), DonorScalar(*TargetB, TEXT("b_polish")), 0.75f);
    // Release target ancestry before cleanup so both generated test packages
    // can be deleted without retaining the other through its parent pointer.
    TargetA->SetParentEditorOnly(Master);
    TargetB->SetParentEditorOnly(Master);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialDonorLegacyCanonicalTest,
    "Mimir.V5.Material.DonorTransfer.LegacyCanonicalUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialDonorLegacyCanonicalTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const TArray<uint8> ClassBytes = DonorUtf8(TEXT("{\n  \"class\": \"simple\",\n  \"params\": {\n    \"roughness\": 0.25\n  }\n}\n"));
    const TArray<uint8> LibraryBytes = DonorUtf8(TEXT("{\n  \"library\": \"concrete_wet_01\"\n}\n"));
    bool bPassed = true;
    for (const TArray<uint8>& Expected : {ClassBytes, LibraryBytes})
    {
        FMHMaterialDocument Parsed;
        TArray<uint8> Actual;
        FString Error;
        bPassed &= TestTrue(TEXT("legacy document parses: ") + Error, MHParseMaterialV4(Expected, Parsed, Error));
        bPassed &= TestTrue(TEXT("legacy document remains legacy mode"), Parsed.Mode != EMHMaterialMode::UnrealInstance && !Parsed.UnrealInstance.IsValid());
        bPassed &= TestTrue(TEXT("legacy document writes: ") + Error, MHWriteCanonicalMaterialV4(Parsed, Actual, Error));
        bPassed &= TestTrue(TEXT("legacy canonical bytes unchanged"), Actual == Expected);
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
