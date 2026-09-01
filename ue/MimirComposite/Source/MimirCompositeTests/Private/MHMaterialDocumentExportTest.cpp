#include "Material/MHMaterialDocumentExport.h"

#include "HAL/FileManager.h"
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
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

UMaterial* MakeDocumentExportParent(const FString& Root, const FString& Token)
{
    const FString PackageName = Root / Token;
    UPackage* Package = CreatePackage(*PackageName);
    if (UMaterial* Existing = FindObject<UMaterial>(Package, *Token))
    {
        return Existing;
    }
    return NewObject<UMaterial>(Package, FName(*Token), RF_Public | RF_Standalone);
}

UMaterialInstanceConstant* MakeDocumentExportMaterial(
    const TCHAR* Name,
    UMaterial* Parent)
{
    UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(
        GetTransientPackage(),
        FName(Name));
    if (Parent != nullptr)
    {
        Material->SetParentEditorOnly(Parent);
    }
    return Material;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialDocumentExportTest,
    "Mimir.V5.Material.DocumentExport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialDocumentExportTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    bool bPassed = true;
    const FString TestRoot = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(
            FPaths::ProjectIntermediateDir(),
            TEXT("MHMaterialDocumentExport"),
            FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    const FString SourceRoot = TestRoot / TEXT("Source");
    const FString ExportRoot = TestRoot / TEXT("Export");
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    IFileManager::Get().MakeDirectory(*ExportRoot, true);

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    UMaterial* Parent = MakeDocumentExportParent(Settings->MasterRoot, TEXT("document_export"));
    UMaterialInstanceConstant* Managed = MakeDocumentExportMaterial(TEXT("ManagedAssetName"), Parent);
    UMHMaterialSourceData* Receipt = NewObject<UMHMaterialSourceData>(Managed);
    Receipt->LogicalName = TEXT("managed_leaf");
    Receipt->SourceRelativePath = TEXT("materials/managed_leaf.material");
    Managed->AddAssetUserData(Receipt);

    FMHMaterialDocument Extracted;
    TArray<uint8> ExpectedBytes;
    FString Error;
    bPassed &= TestTrue(
        TEXT("fixture extracts"),
        MHExtractMaterialV4(*Managed, *Settings, Extracted, Error));
    bPassed &= TestTrue(
        TEXT("fixture canonicalizes"),
        MHWriteCanonicalMaterialV4(Extracted, ExpectedBytes, Error));
    Receipt->AppliedHash = MHRawPayloadHash(ExpectedBytes);

    bPassed &= TestEqual(
        TEXT("receipt logical name wins"),
        MHGetMaterialDocumentExportLogicalName(*Managed),
        FString(TEXT("managed_leaf")));
    UMaterialInstanceConstant* Fallback = MakeDocumentExportMaterial(TEXT("FallbackAssetName"), Parent);
    bPassed &= TestEqual(
        TEXT("unmanaged material falls back to asset name"),
        MHGetMaterialDocumentExportLogicalName(*Fallback),
        FString(TEXT("FallbackAssetName")));

    FMHMaterialDocumentExportPlan RoundTripPlan;
    const FString RoundTripPath = ExportRoot / TEXT("managed_leaf.material");
    const TArray<FMHMaterialDocumentExportRequest> RoundTripRequests = {
        {Managed, RoundTripPath}};
    Error.Reset();
    const bool bRoundTripPrepared = MHPrepareMaterialDocumentExport(
        RoundTripRequests,
        *Settings,
        SourceRoot,
        RoundTripPlan,
        Error);
    bPassed &= TestTrue(TEXT("round-trip preflight succeeds"), bRoundTripPrepared);
    if (bRoundTripPrepared && RoundTripPlan.Ready.Num() == 1)
    {
        bPassed &= TestTrue(
            TEXT("canonical bytes are byte-identical"),
            RoundTripPlan.Ready[0].CanonicalBytes == ExpectedBytes);
        bPassed &= TestEqual(
            TEXT("canonical hash equals managed AppliedHash"),
            RoundTripPlan.Ready[0].CanonicalHash,
            Receipt->AppliedHash);
        bPassed &= TestTrue(
            TEXT("managed receipt match is recorded"),
            RoundTripPlan.Ready[0].bMatchesAppliedHash);
    }
    else
    {
        AddError(FString::Printf(TEXT("round-trip plan missing ready item: %s"), *Error));
        bPassed = false;
    }

    const FString ExistingPath = ExportRoot / TEXT("existing.material");
    const FString FreshPath = ExportRoot / TEXT("fresh.material");
    const TArray<uint8> OldBytes = {0x6f, 0x6c, 0x64};
    bPassed &= TestTrue(
        TEXT("collision fixture writes"),
        FFileHelper::SaveArrayToFile(OldBytes, *ExistingPath));
    FMHMaterialDocumentExportPlan CollisionPlan;
    const TArray<FMHMaterialDocumentExportRequest> CollisionRequests = {
        {Managed, ExistingPath},
        {Fallback, FreshPath}};
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("collision preflight succeeds"),
        MHPrepareMaterialDocumentExport(
            CollisionRequests,
            *Settings,
            SourceRoot,
            CollisionPlan,
            Error));
    bPassed &= TestEqual(
        TEXT("one overwrite is reported"),
        CollisionPlan.OverwritePaths.Num(),
        1);
    FMHMaterialDocumentExportResult CancelledResult;
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("collision cancellation is a valid result"),
        MHCommitMaterialDocumentExport(CollisionPlan, false, CancelledResult, Error));
    bPassed &= TestTrue(TEXT("batch reports cancellation"), CancelledResult.bCancelled);
    TArray<uint8> ExistingAfterCancel;
    FFileHelper::LoadFileToArray(ExistingAfterCancel, *ExistingPath);
    bPassed &= TestTrue(TEXT("existing file remains byte-identical"), ExistingAfterCancel == OldBytes);
    bPassed &= TestFalse(TEXT("fresh peer is not written on cancel"), IFileManager::Get().FileExists(*FreshPath));
    FMHMaterialDocumentExportResult OverwriteResult;
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("confirmed collision batch commits"),
        MHCommitMaterialDocumentExport(CollisionPlan, true, OverwriteResult, Error));
    bPassed &= TestEqual(TEXT("confirmed batch exports both peers"), OverwriteResult.ExportedCount, 2);
    TArray<uint8> ExistingAfterOverwrite;
    FFileHelper::LoadFileToArray(ExistingAfterOverwrite, *ExistingPath);
    bPassed &= TestTrue(
        TEXT("confirmed overwrite receives canonical bytes"),
        ExistingAfterOverwrite == ExpectedBytes);
    bPassed &= TestTrue(
        TEXT("confirmed non-collision peer is written"),
        IFileManager::Get().FileExists(*FreshPath));

    FMHMaterialDocumentExportPlan SourceRootPlan;
    const TArray<FMHMaterialDocumentExportRequest> SourceRootRequests = {
        {Managed, SourceRoot / TEXT("blocked.material")}};
    Error.Reset();
    bPassed &= TestFalse(
        TEXT("Source Root export is refused"),
        MHPrepareMaterialDocumentExport(
            SourceRootRequests,
            *Settings,
            SourceRoot,
            SourceRootPlan,
            Error));
    bPassed &= TestTrue(
        TEXT("Source Root refusal points to Publish"),
        Error.Contains(TEXT("Publish Material to MH Source"), ESearchCase::CaseSensitive));
    bPassed &= TestFalse(
        TEXT("Source Root refusal writes nothing"),
        IFileManager::Get().FileExists(*(SourceRoot / TEXT("blocked.material"))));

    UMaterialInstanceConstant* Invalid = MakeDocumentExportMaterial(TEXT("InvalidMaterial"), nullptr);
    const FString GoodPath = ExportRoot / TEXT("good.material");
    const FString BadPath = ExportRoot / TEXT("bad.material");
    FMHMaterialDocumentExportPlan PartialPlan;
    const TArray<FMHMaterialDocumentExportRequest> PartialRequests = {
        {Invalid, BadPath},
        {Managed, GoodPath}};
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("bad peer does not reject batch preflight"),
        MHPrepareMaterialDocumentExport(
            PartialRequests,
            *Settings,
            SourceRoot,
            PartialPlan,
            Error));
    bPassed &= TestEqual(TEXT("one valid material remains ready"), PartialPlan.Ready.Num(), 1);
    bPassed &= TestEqual(TEXT("one invalid material is skipped"), PartialPlan.Skipped.Num(), 1);
    FMHMaterialDocumentExportResult PartialResult;
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("valid peer commits"),
        MHCommitMaterialDocumentExport(PartialPlan, true, PartialResult, Error));
    bPassed &= TestEqual(TEXT("one material exported"), PartialResult.ExportedCount, 1);
    bPassed &= TestTrue(TEXT("valid peer file exists"), IFileManager::Get().FileExists(*GoodPath));
    bPassed &= TestFalse(TEXT("invalid peer file absent"), IFileManager::Get().FileExists(*BadPath));

    IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
