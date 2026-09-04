#include "Engine/Texture2D.h"
#include "Material/MHMaterialClipboard.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialProtocol.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

UMaterial* ClipboardParent(const FString& PackageName, const FString& Name)
{
    UPackage* Package = CreatePackage(*PackageName);
    if (UMaterial* Existing = FindObject<UMaterial>(Package, *Name)) return Existing;
    return NewObject<UMaterial>(Package, FName(*Name), RF_Public | RF_Standalone);
}

UTexture2D* ClipboardTexture(const FString& Name)
{
    const FString PackageName = TEXT("/Game/MH/Generated/Textures/") + Name;
    UPackage* Package = CreatePackage(*PackageName);
    if (UTexture2D* Existing = FindObject<UTexture2D>(Package, *Name)) return Existing;
    return NewObject<UTexture2D>(Package, FName(*Name), RF_Public | RF_Standalone);
}

float ScalarOverride(const UMaterialInstanceConstant& Material, const TCHAR* Name)
{
    for (const FScalarParameterValue& Value : Material.ScalarParameterValues)
    {
        if (Value.ParameterInfo.Name == FName(Name)) return Value.ParameterValue;
    }
    return -1.0f;
}

} // namespace

// Owner workflow (2026-09-03): a donor Material Instance that predates the MH
// source protocol (parent outside MasterRoot) hands its parent, parameters and
// static state to a managed instance, with no export or import in between.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialClipboardTest,
    "Mimir.V5.Material.ClipboardCopyPaste",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialClipboardTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower().Left(8);
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    // The donor's parent lives outside MasterRoot: it is exactly the case no
    // export path can represent.
    UMaterial* LegacyParent = ClipboardParent(TEXT("/Game/ImportedMeshes/Legacy/m_legacy_") + Suffix, TEXT("m_legacy_") + Suffix);
    UMaterial* ManagedParent = ClipboardParent(Settings->MasterRoot / TEXT("simple"), TEXT("simple"));
    UTexture2D* DonorTexture = ClipboardTexture(TEXT("clip_donor_tex_") + Suffix);
    if (!TestNotNull(TEXT("legacy parent"), LegacyParent) || !TestNotNull(TEXT("managed parent"), ManagedParent) ||
        !TestNotNull(TEXT("donor texture"), DonorTexture)) return false;

    UMaterialInstanceConstant* Donor = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), FName(*(TEXT("clip_donor_") + Suffix)));
    Donor->SetParentEditorOnly(LegacyParent, false);
    Donor->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("roughness")), 0.25f);
    Donor->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("tint")), FLinearColor(0.5f, 0.25f, 0.125f, 1.0f));
    Donor->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("tex0")), DonorTexture);
    FMaterialInstanceBasePropertyOverrides DonorOverrides;
    DonorOverrides.bOverride_TwoSided = true;
    DonorOverrides.TwoSided = true;
    DonorOverrides.bOverride_BlendMode = true;
    DonorOverrides.BlendMode = BLEND_Masked;
    FStaticParameterSet DonorStatic;
    DonorStatic.StaticSwitchParameters.Add(FStaticSwitchParameter(
        FMaterialParameterInfo(TEXT("use_detail")), true, true, FGuid::NewGuid()));
    Donor->UpdateStaticPermutation(DonorStatic, DonorOverrides);

    // The target is a managed material: it has an MH receipt and a registered parent.
    UMaterialInstanceConstant* Target = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), FName(*(TEXT("clip_target_") + Suffix)));
    Target->SetParentEditorOnly(ManagedParent, false);
    Target->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("roughness")), 0.9f);
    UMHMaterialSourceData* Receipt = NewObject<UMHMaterialSourceData>(Target);
    Receipt->LogicalName = TEXT("clip_target_") + Suffix;
    Receipt->SourceRelativePath = Receipt->LogicalName + TEXT(".material");
    Target->AddAssetUserData(Receipt);
    FMHMaterialDocument TargetDocument;
    TArray<uint8> TargetBytes;
    FString Error;
    bool bPassed = TestTrue(TEXT("target extracts before paste"), MHExtractMaterialV4(*Target, *Settings, TargetDocument, Error));
    bPassed &= TestTrue(TEXT("target canonicalizes"), MHWriteCanonicalMaterialV4(TargetDocument, TargetBytes, Error));
    Receipt->AppliedHash = MHRawPayloadHash(TargetBytes);

    MHClearMaterialClipboard();
    bPassed &= TestFalse(TEXT("clipboard starts empty"), MHHasMaterialClipboardData());
    TArray<FString> Warnings;
    Error.Reset();
    bPassed &= TestFalse(TEXT("paste without a copy is refused"),
        MHPasteMaterialDataFromClipboard(*Target, *Settings, Warnings, Error));
    bPassed &= TestTrue(TEXT("empty clipboard names a code"), Error.StartsWith(TEXT("MH_E_INVALID_RESOURCE_SOURCE")));
    bPassed &= TestEqual(TEXT("refused paste leaves the target untouched"), ScalarOverride(*Target, TEXT("roughness")), 0.9f);

    Error.Reset();
    bPassed &= TestTrue(TEXT("copy from the legacy donor succeeds: ") + Error,
        MHCopyMaterialDataToClipboard(*Donor, Warnings, Error));
    bPassed &= TestTrue(TEXT("clipboard holds a copy"), MHHasMaterialClipboardData());
    bPassed &= TestEqual(TEXT("clipboard names the donor"), MHGetMaterialClipboardSourceLabel(), Donor->GetPathName());

    Warnings.Reset();
    Error.Reset();
    bPassed &= TestTrue(TEXT("paste into the managed target succeeds: ") + Error,
        MHPasteMaterialDataFromClipboard(*Target, *Settings, Warnings, Error));
    bPassed &= TestEqual(TEXT("target adopts the donor parent"), Target->Parent.Get(), static_cast<UMaterialInterface*>(LegacyParent));
    bPassed &= TestEqual(TEXT("target adopts the donor scalar"), ScalarOverride(*Target, TEXT("roughness")), 0.25f);
    bPassed &= TestEqual(TEXT("target carries exactly the donor scalar overrides"), Target->ScalarParameterValues.Num(), 1);
    bPassed &= TestEqual(TEXT("target carries the donor vector override"), Target->VectorParameterValues.Num(), 1);
    if (Target->TextureParameterValues.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("target adopts the donor texture"),
            Target->TextureParameterValues[0].ParameterValue.Get(), static_cast<UTexture*>(DonorTexture));
    }
    else
    {
        bPassed &= TestEqual(TEXT("target carries the donor texture override"), Target->TextureParameterValues.Num(), 1);
    }
    bPassed &= TestTrue(TEXT("target adopts the donor base overrides"),
        Target->BasePropertyOverrides.bOverride_TwoSided && Target->BasePropertyOverrides.TwoSided &&
        Target->BasePropertyOverrides.bOverride_BlendMode && Target->BasePropertyOverrides.BlendMode == BLEND_Masked);
    const FStaticParameterSet TargetStatic = Target->GetStaticParameters();
    const FStaticSwitchParameter* Switch = TargetStatic.StaticSwitchParameters.FindByPredicate(
        [](const FStaticSwitchParameter& Value) { return Value.ParameterInfo.Name == FName(TEXT("use_detail")); });
    bPassed &= TestTrue(TEXT("target adopts the donor static switch"), Switch != nullptr && Switch->bOverride && Switch->Value);
    bPassed &= TestTrue(TEXT("the managed target keeps its MH receipt"),
        Cast<UMHMaterialSourceData>(Target->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass())) == Receipt &&
        Receipt->LogicalName == TEXT("clip_target_") + Suffix);
    // The paste is honest about consequences: the target now differs from its
    // source, and an unregistered parent puts it outside the publish path.
    const bool bWarnsLocalModification = Warnings.ContainsByPredicate([](const FString& Warning)
        { return Warning.Contains(TEXT("MH_W_MANAGED_ASSET_LOCALLY_MODIFIED")); });
    const bool bWarnsPublish = Warnings.ContainsByPredicate([](const FString& Warning)
        { return Warning.Contains(TEXT("can no longer be published to MH Source")); });
    bPassed &= TestTrue(TEXT("paste warns that the managed target is locally modified"), bWarnsLocalModification);
    bPassed &= TestTrue(TEXT("paste warns that an unregistered parent blocks publishing"), bWarnsPublish);

    // Pasting a registered-parent donor leaves the target publishable again.
    UMaterialInstanceConstant* RegisteredDonor = NewObject<UMaterialInstanceConstant>(
        GetTransientPackage(), FName(*(TEXT("clip_registered_") + Suffix)));
    RegisteredDonor->SetParentEditorOnly(ManagedParent, false);
    RegisteredDonor->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("metallic")), 1.0f);
    Warnings.Reset();
    Error.Reset();
    bPassed &= TestTrue(TEXT("copy from a registered-parent donor succeeds"),
        MHCopyMaterialDataToClipboard(*RegisteredDonor, Warnings, Error));
    Warnings.Reset();
    bPassed &= TestTrue(TEXT("second paste succeeds: ") + Error,
        MHPasteMaterialDataFromClipboard(*Target, *Settings, Warnings, Error));
    bPassed &= TestFalse(TEXT("a registered parent raises no publish warning"),
        Warnings.ContainsByPredicate([](const FString& Warning)
            { return Warning.Contains(TEXT("can no longer be published to MH Source")); }));
    FMHMaterialDocument AfterDocument;
    Error.Reset();
    bPassed &= TestTrue(TEXT("target extracts after the second paste: ") + Error,
        MHExtractMaterialV4(*Target, *Settings, AfterDocument, Error));
    bPassed &= TestTrue(TEXT("extracted document carries the pasted parameter"),
        AfterDocument.Params.Contains(TEXT("metallic")));
    bPassed &= TestFalse(TEXT("the first donor state is fully replaced"),
        AfterDocument.Params.Contains(TEXT("roughness")));
    MHClearMaterialClipboard();
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
