#include "MHGoldenRoot.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialProtocol.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceAnalyzer.h"
#include "StaticParameterSet.h"
#include "UObject/Package.h"
#include "UObject/PackageReload.h"

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<uint8> Utf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value, Value.Len());
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return Bytes;
}

TArray<uint8> JsonObjectBytes(const TSharedRef<FJsonObject>& Object)
{
    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(Object, Writer);
    return Utf8(Json);
}

UMaterial* MakeParent(const FString& Root, const FString& Token)
{
    const FString PackageName = Root + TEXT("/") + Token;
    UPackage* Package = CreatePackage(*PackageName);
    if (UMaterial* Existing = FindObject<UMaterial>(Package, *Token))
    {
        return Existing;
    }
    return NewObject<UMaterial>(Package, FName(*Token), RF_Public | RF_Standalone);
}

UTexture* MakeTexture(const FString& Token)
{
    UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/MH/Generated/Textures/%s"), *Token));
    if (UTexture2D* Existing = FindObject<UTexture2D>(Package, *Token))
    {
        return Existing;
    }
    return NewObject<UTexture2D>(Package, FName(*Token), RF_Public | RF_Standalone);
}

bool ErrorStartsWith(const FString& Error, const TCHAR* Code)
{
    return Error.StartsWith(Code, ESearchCase::CaseSensitive);
}

bool WriteTestPng(const FString& Path, const FColor Color = FColor(17, 83, 191, 255))
{
    TArray64<FColor> Pixels;
    Pixels.Add(Color);
    TArray64<uint8> Png;
    FImageUtils::PNGCompressImageArray(1, 1, Pixels, Png);
    return !Png.IsEmpty() && FFileHelper::SaveArrayToFile(Png, *Path);
}

FMHSourceAnalysisEntry MaterialEntry(
    const FString& LogicalName,
    const FString& SourceRelativePath,
    const FMHResolveOutcome& Resolved)
{
    FMHSourceAnalysisEntry Entry;
    Entry.Key.Kind = EMHResourceKind::Material;
    Entry.Key.LogicalName = LogicalName;
    Entry.PayloadPath = Resolved.PayloadPath;
    Entry.SourcePath = SourceRelativePath;
    Entry.RawHash = Resolved.RawHash;
    Entry.Change = EMHSourceChange::Create;
    return Entry;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialGoldenVectorsTest,
    "Mimir.V4.Material.CanonicalGoldenVectors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialGoldenVectorsTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot))
    {
        return false;
    }
    FString FixtureText;
    const FString FixturePath = FPaths::Combine(GoldenRoot, TEXT("material_v4_vectors.json"));
    if (!FFileHelper::LoadFileToString(FixtureText, *FixturePath))
    {
        AddError(FString::Printf(TEXT("cannot read %s"), *FixturePath));
        return false;
    }
    TSharedPtr<FJsonObject> Fixture;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FixtureText), Fixture) || !Fixture.IsValid())
    {
        AddError(TEXT("material_v4_vectors.json is not valid JSON"));
        return false;
    }
    FString Schema;
    bool bPassed = TestTrue(
        TEXT("fixture schema"),
        Fixture->TryGetStringField(TEXT("schema"), Schema) && Schema == TEXT("mh.material_v4_vectors"));
    const TArray<TSharedPtr<FJsonValue>>* Vectors = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("vectors"), Vectors) || Vectors == nullptr)
    {
        AddError(TEXT("fixture vectors missing"));
        return false;
    }
    for (const TSharedPtr<FJsonValue>& VectorValue : *Vectors)
    {
        const TSharedPtr<FJsonObject> Vector = VectorValue.IsValid() ? VectorValue->AsObject() : nullptr;
        FString Name;
        FString Expected;
        const TSharedPtr<FJsonObject>* Value = nullptr;
        if (!Vector.IsValid() || !Vector->TryGetStringField(TEXT("name"), Name) ||
            !Vector->TryGetStringField(TEXT("canonical_utf8"), Expected) ||
            !Vector->TryGetObjectField(TEXT("value"), Value) || Value == nullptr)
        {
            AddError(TEXT("malformed material vector envelope"));
            return false;
        }
        FMHMaterialDocument Document;
        FString Error;
        TArray<uint8> Actual;
        bPassed &= TestTrue(
            *FString::Printf(TEXT("%s parses"), *Name),
            MHParseMaterialV4(JsonObjectBytes((*Value).ToSharedRef()), Document, Error));
        bPassed &= TestTrue(
            *FString::Printf(TEXT("%s writes"), *Name),
            MHWriteCanonicalMaterialV4(Document, Actual, Error));
        bPassed &= TestTrue(
            *FString::Printf(TEXT("%s exact canonical bytes"), *Name),
            Actual == Utf8(Expected));
        if (!bPassed && !Error.IsEmpty())
        {
            AddError(Error);
        }
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialClosedGrammarTest,
    "Mimir.V4.Material.ClosedGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialClosedGrammarTest::RunTest(const FString& Parameters)
{
    struct FVector
    {
        const TCHAR* Json;
        const TCHAR* Code;
    };
    const FVector Vectors[] = {
        {TEXT("{\"class\":\"simple\",\"unknown\":1}"), TEXT("MH_E_MATERIAL_GRAMMAR")},
        {TEXT("{\"library\":\"lib\",\"params\":{}}"), TEXT("MH_E_MATERIAL_GRAMMAR")},
        {TEXT("{\"class\":\"simple\",\"textures\":{\"tex01\":\"a\"}}"), TEXT("MH_E_MATERIAL_GRAMMAR")},
        {TEXT("{\"class\":\"simple\",\"textures\":{\"tex0\":\"folder/a.png\"}}"), TEXT("MH_E_NONCANONICAL_TEXTURE_REFERENCE")},
        {TEXT("{\"class\":\"simple\",\"params\":{\"v\":[1,2,3]}}"), TEXT("MH_E_MATERIAL_GRAMMAR")},
        {TEXT("{\"class\":\"simple\",\"twosided\":1}"), TEXT("MH_E_MATERIAL_GRAMMAR")},
        {TEXT("{\"class\":\"simple\",\"tex16support\":true}"), TEXT("MH_E_MATERIAL_GRAMMAR")}};
    bool bPassed = true;
    for (const FVector& Vector : Vectors)
    {
        FMHMaterialDocument Document;
        FString Error;
        bPassed &= TestFalse(Vector.Json, MHParseMaterialV4(Utf8(Vector.Json), Document, Error));
        bPassed &= TestTrue(Vector.Code, ErrorStartsWith(Error, Vector.Code));
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialApplyExtractTest,
    "Mimir.V4.Material.ApplyExtractAndLocalChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialApplyExtractTest::RunTest(const FString& Parameters)
{
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    UMaterial* Parent = MakeParent(Settings->MasterRoot, TEXT("simple"));
    UTexture* Texture = MakeTexture(TEXT("albedo_d"));
    UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(GetTransientPackage());

    FMHMaterialDocument Source;
    Source.Parent = TEXT("simple");
    Source.bHasTwoSided = true;
    Source.bTwoSided = false;
    Source.Textures.Add(0, TEXT("albedo_d"));
    FMHMaterialParameter Scalar;
    Scalar.Scalar = static_cast<float>(0.10000000001);
    Source.Params.Add(TEXT("roughness"), Scalar);
    FMHMaterialParameter Vector;
    Vector.bVector = true;
    Vector.Vector = FVector4f(-0.0f, -1.25f, 1.0e-7f, 63.0f);
    Source.Params.Add(TEXT("negative_vector"), Vector);

    FString Error;
    bool bPassed = TestTrue(
        TEXT("class full apply"),
        MHApplyMaterialV4(*Material, *Parent, Source, {{TEXT("albedo_d"), Texture}}, Error));
    FMHMaterialDocument Extracted;
    bPassed &= TestTrue(TEXT("class extract"), MHExtractMaterialV4(*Material, *Settings, Extracted, Error));
    TArray<uint8> SourceBytes;
    TArray<uint8> ExtractedBytes;
    bPassed &= TestTrue(TEXT("source canonical"), MHWriteCanonicalMaterialV4(Source, SourceBytes, Error));
    bPassed &= TestTrue(TEXT("extract canonical"), MHWriteCanonicalMaterialV4(Extracted, ExtractedBytes, Error));
    bPassed &= TestTrue(TEXT("float32 apply/extract exact"), SourceBytes == ExtractedBytes);

    UMHMaterialSourceData* Data = NewObject<UMHMaterialSourceData>(Material);
    Data->AppliedHash = MHRawPayloadHash(ExtractedBytes);
    Material->AddAssetUserData(Data);
    Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("roughness")), 0.75f);
    FString Warning;
    bPassed &= TestTrue(TEXT("local change detected"), MHDetectManagedMaterialLocalModification(*Material, *Settings, Warning));
    bPassed &= TestTrue(TEXT("local change warning code"), Warning.StartsWith(TEXT("MH_W_MANAGED_ASSET_LOCALLY_MODIFIED")));

    FMaterialInstanceBasePropertyOverrides Unsupported;
    Unsupported.bOverride_BlendMode = true;
    Unsupported.BlendMode = BLEND_Masked;
    Material->BasePropertyOverrides = Unsupported;
    bPassed &= TestFalse(TEXT("unsupported base override rejected"), MHExtractMaterialV4(*Material, *Settings, Extracted, Error));
    bPassed &= TestTrue(TEXT("unsupported override code"), ErrorStartsWith(Error, TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE")));

    Material->BasePropertyOverrides = FMaterialInstanceBasePropertyOverrides();
    FStaticParameterSet StaticSet;
    StaticSet.StaticSwitchParameters.Add(FStaticSwitchParameter(
        FMaterialParameterInfo(TEXT("static_flag")), true, true, FGuid::NewGuid()));
    Material->UpdateStaticPermutation(StaticSet);
    bPassed &= TestFalse(TEXT("static override rejected"), MHExtractMaterialV4(*Material, *Settings, Extracted, Error));
    bPassed &= TestTrue(TEXT("static override code"), ErrorStartsWith(Error, TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialLibraryApplyTest,
    "Mimir.V4.Material.LibraryFullApply",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialLibraryApplyTest::RunTest(const FString& Parameters)
{
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    UMaterial* ClassParent = MakeParent(Settings->MasterRoot, TEXT("simple"));
    UMaterial* LibraryParent = MakeParent(Settings->LibraryRoot, TEXT("concrete_wet_01"));
    UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
    Material->SetParentEditorOnly(ClassParent);
    Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("scalar")), 1.0f);
    Material->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("vector")), FLinearColor::White);
    Material->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("tex0")), MakeTexture(TEXT("albedo_d")));
    FMaterialInstanceBasePropertyOverrides Overrides;
    Overrides.bOverride_TwoSided = true;
    Overrides.TwoSided = true;
    Material->BasePropertyOverrides = Overrides;

    FMHMaterialDocument Library;
    Library.Mode = EMHMaterialMode::Library;
    Library.Parent = TEXT("concrete_wet_01");
    FString Error;
    bool bPassed = TestTrue(TEXT("library full apply"), MHApplyMaterialV4(*Material, *LibraryParent, Library, {}, Error));
    bPassed &= TestEqual(TEXT("library parent"), Material->Parent.Get(), static_cast<UMaterialInterface*>(LibraryParent));
    bPassed &= TestTrue(TEXT("scalar overrides cleared"), Material->ScalarParameterValues.IsEmpty());
    bPassed &= TestTrue(TEXT("vector overrides cleared"), Material->VectorParameterValues.IsEmpty());
    bPassed &= TestTrue(TEXT("texture overrides cleared"), Material->TextureParameterValues.IsEmpty());
    bPassed &= TestFalse(TEXT("base overrides cleared"), Material->HasOverridenBaseProperties());
    FMHMaterialDocument Extracted;
    bPassed &= TestTrue(TEXT("clean library extracts"), MHExtractMaterialV4(*Material, *Settings, Extracted, Error));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialAdoptControllerTest,
    "Mimir.V4.Material.AdoptController",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialAdoptControllerTest::RunTest(const FString& Parameters)
{
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), TEXT("Mimir/AdoptRoot"));
    FMHMaterialAdoptTarget Target;
    Target.Folder = FPaths::Combine(Root, TEXT("materials/sub"));
    Target.LogicalName = TEXT("paint_red");
    FString Path;
    FString Relative;
    FString Error;
    bool bPassed = TestTrue(
        TEXT("folder and name accepted"),
        MHValidateMaterialAdoptTarget(Root, Target, Path, Relative, Error));
    bPassed &= TestEqual(TEXT("target suffix"), FPaths::GetCleanFilename(Path), FString(TEXT("paint_red.material")));
    bPassed &= TestEqual(TEXT("relative path"), Relative, FString(TEXT("materials/sub/paint_red.material")));

    Target.LogicalName = TEXT("PaintRed");
    bPassed &= TestFalse(TEXT("noncanonical name rejected"), MHValidateMaterialAdoptTarget(Root, Target, Path, Relative, Error));
    bPassed &= TestTrue(TEXT("name diagnostic"), ErrorStartsWith(Error, TEXT("MH_E_NONCANONICAL_RESOURCE_NAME")));

    Target.LogicalName = TEXT("paint_red");
    Target.Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), TEXT("OutsideAdoptRoot"));
    bPassed &= TestFalse(TEXT("outside folder rejected"), MHValidateMaterialAdoptTarget(Root, Target, Path, Relative, Error));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialManagedRoundTripTest,
    "Mimir.V4.Material.ManagedRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialAppliedParentReceiptsTest,
    "Mimir.V4.Material.AppliedParentReceipts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialTextureResolutionGatesTest,
    "Mimir.V4.Material.TextureResolutionGates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialTextureResolutionGatesTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir/MaterialTextureGates"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString MissingPath = FPaths::Combine(SourceRoot, TEXT("s2_missing_gate.material"));
    const FString AmbiguousPath = FPaths::Combine(SourceRoot, TEXT("s2_ambiguous_gate.material"));
    bool bPassed = TestTrue(
        TEXT("missing fixture written"),
        FFileHelper::SaveArrayToFile(
            Utf8(TEXT("{\"class\":\"simple\",\"textures\":{\"tex0\":\"s2_missing_texture\"}}")),
            *MissingPath));
    bPassed &= TestTrue(
        TEXT("ambiguous fixture written"),
        FFileHelper::SaveArrayToFile(
            Utf8(TEXT("{\"class\":\"simple\",\"textures\":{\"tex0\":\"s2_ambiguous_texture\"}}")),
            *AmbiguousPath));
    const TArray<uint8> DummyTexture = {1, 2, 3, 4};
    bPassed &= TestTrue(
        TEXT("first duplicate written"),
        FFileHelper::SaveArrayToFile(
            DummyTexture,
            *FPaths::Combine(SourceRoot, TEXT("s2_ambiguous_texture.png"))));
    bPassed &= TestTrue(
        TEXT("second duplicate written"),
        FFileHelper::SaveArrayToFile(
            DummyTexture,
            *FPaths::Combine(SourceRoot, TEXT("s2_ambiguous_texture.tga"))));
    const FString SnapshotTexturePath = FPaths::Combine(SourceRoot, TEXT("s2_snapshot_texture.png"));
    const FString SnapshotMaterialPath = FPaths::Combine(SourceRoot, TEXT("s2_snapshot_guard.material"));
    bPassed &= TestTrue(TEXT("snapshot PNG written"), WriteTestPng(SnapshotTexturePath));
    bPassed &= TestTrue(
        TEXT("snapshot material written"),
        FFileHelper::SaveArrayToFile(
            Utf8(TEXT("{\"class\":\"simple\",\"textures\":{\"tex0\":\"s2_snapshot_texture\"}}")),
            *SnapshotMaterialPath));

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    MakeParent(Settings->MasterRoot, TEXT("simple"));
    FMHPayloadScanResolver Resolver(SourceRoot);
    FString Error;
    bPassed &= TestTrue(TEXT("texture-gate source scans"), Resolver.Initialize(Error));

    FMHResourceKey MissingKey;
    MissingKey.Kind = EMHResourceKind::Material;
    MissingKey.LogicalName = TEXT("s2_missing_gate");
    const FMHResolveOutcome MissingResolved = Resolver.Resolve(MissingKey);
    const FMHMaterialOperationResult Missing = MHImportMaterialV4(
        MaterialEntry(TEXT("s2_missing_gate"), TEXT("s2_missing_gate.material"), MissingResolved),
        Resolver,
        SourceRoot,
        *Settings);
    bPassed &= TestFalse(TEXT("missing texture blocks material"), Missing.Succeeded());
    bPassed &= TestTrue(
        TEXT("missing texture exact code"),
        ErrorStartsWith(Missing.Error, TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE")));

    FMHResourceKey AmbiguousKey;
    AmbiguousKey.Kind = EMHResourceKind::Material;
    AmbiguousKey.LogicalName = TEXT("s2_ambiguous_gate");
    const FMHResolveOutcome AmbiguousResolved = Resolver.Resolve(AmbiguousKey);
    const FMHMaterialOperationResult Ambiguous = MHImportMaterialV4(
        MaterialEntry(TEXT("s2_ambiguous_gate"), TEXT("s2_ambiguous_gate.material"), AmbiguousResolved),
        Resolver,
        SourceRoot,
        *Settings);
    bPassed &= TestFalse(TEXT("ambiguous texture blocks material"), Ambiguous.Succeeded());
    bPassed &= TestTrue(
        TEXT("ambiguous texture exact code"),
        ErrorStartsWith(Ambiguous.Error, TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME")));

    FMHResourceKey SnapshotKey;
    SnapshotKey.Kind = EMHResourceKind::Material;
    SnapshotKey.LogicalName = TEXT("s2_snapshot_guard");
    const FMHResolveOutcome SnapshotResolved = Resolver.Resolve(SnapshotKey);
    bPassed &= TestTrue(
        TEXT("texture generation changes after resolver snapshot"),
        WriteTestPng(SnapshotTexturePath, FColor(229, 41, 73, 255)));
    const FMHMaterialOperationResult SnapshotRaced = MHImportMaterialV4(
        MaterialEntry(TEXT("s2_snapshot_guard"), TEXT("s2_snapshot_guard.material"), SnapshotResolved),
        Resolver,
        SourceRoot,
        *Settings);
    bPassed &= TestFalse(TEXT("texture generation race blocks material"), SnapshotRaced.Succeeded());
    bPassed &= TestTrue(
        TEXT("texture generation race exact code"),
        ErrorStartsWith(SnapshotRaced.Error, TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED")));
    const FString SnapshotPackageName = TEXT("/Game/MH/Generated/Materials/s2_snapshot_guard");
    const FString SnapshotObjectPath = SnapshotPackageName + TEXT(".s2_snapshot_guard");
    bPassed &= TestNull(
        TEXT("texture race creates no MIC ghost"),
        FindObject<UMaterialInstanceConstant>(nullptr, *SnapshotObjectPath));
    bPassed &= TestNull(
        TEXT("texture race creates no package ghost"),
        FindPackage(nullptr, *SnapshotPackageName));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialTextureImportPersistenceTest,
    "Mimir.V4.Material.TextureImportPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialTextureImportPersistenceTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir/MaterialTexturePersistence"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString TexturePath = FPaths::Combine(SourceRoot, TEXT("s2_persist_texture.png"));
    const FString MaterialPath = FPaths::Combine(SourceRoot, TEXT("s2_texture_valid.material"));
    bool bPassed = TestTrue(TEXT("real PNG written"), WriteTestPng(TexturePath));
    bPassed &= TestTrue(
        TEXT("textured material written"),
        FFileHelper::SaveArrayToFile(
            Utf8(TEXT("{\"class\":\"simple\",\"textures\":{\"tex0\":\"s2_persist_texture\"}}")),
            *MaterialPath));

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    MakeParent(Settings->MasterRoot, TEXT("simple"));
    FString Error;
    FMHPayloadScanResolver Resolver(SourceRoot);
    bPassed &= TestTrue(TEXT("real texture source scans"), Resolver.Initialize(Error));
    FMHResourceKey MaterialKey;
    MaterialKey.Kind = EMHResourceKind::Material;
    MaterialKey.LogicalName = TEXT("s2_texture_valid");
    const FMHResolveOutcome Resolved = Resolver.Resolve(MaterialKey);
    const FMHMaterialOperationResult Imported = MHImportMaterialV4(
        MaterialEntry(TEXT("s2_texture_valid"), TEXT("s2_texture_valid.material"), Resolved),
        Resolver,
        SourceRoot,
        *Settings);
    bPassed &= TestTrue(TEXT("real PNG material imports"), Imported.Succeeded());
    if (!Imported.Succeeded())
    {
        AddError(Imported.Error);
        return false;
    }

    const FString TexturePackageName = TEXT("/Game/MH/Generated/Textures/s2_persist_texture");
    const FString TextureObjectPath = TexturePackageName + TEXT(".s2_persist_texture");
    UTexture* Texture = LoadObject<UTexture>(nullptr, *TextureObjectPath);
    bPassed &= TestNotNull(TEXT("exact imported texture exists"), Texture);
    const FString TextureFilename = FPackageName::LongPackageNameToFilename(
        TexturePackageName,
        FPackageName::GetAssetPackageExtension());
    bPassed &= TestTrue(TEXT("texture package persisted"), IFileManager::Get().FileExists(*TextureFilename));
    if (Texture == nullptr)
    {
        return false;
    }
    UPackage* ReloadedPackage = ReloadPackage(Texture->GetOutermost(), LOAD_None);
    bPassed &= TestNotNull(TEXT("persisted texture package reloads"), ReloadedPackage);
    bPassed &= TestNotNull(TEXT("exact texture reloads"), LoadObject<UTexture>(nullptr, *TextureObjectPath));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialTextureStaleFailureTest,
    "Mimir.V4.Material.TextureStaleFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialTextureStaleFailureTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir/MaterialTextureStaleFailure"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString TexturePath = FPaths::Combine(SourceRoot, TEXT("s2_stale_texture.png"));
    const FString ValidPath = FPaths::Combine(SourceRoot, TEXT("s2_stale_valid.material"));
    bool bPassed = TestTrue(TEXT("stale fixture PNG written"), WriteTestPng(TexturePath));
    bPassed &= TestTrue(
        TEXT("stale fixture material written"),
        FFileHelper::SaveArrayToFile(
            Utf8(TEXT("{\"class\":\"simple\",\"textures\":{\"tex0\":\"s2_stale_texture\"}}")),
            *ValidPath));

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    MakeParent(Settings->MasterRoot, TEXT("simple"));
    FString Error;
    FMHPayloadScanResolver ValidResolver(SourceRoot);
    bPassed &= TestTrue(TEXT("stale fixture scans"), ValidResolver.Initialize(Error));
    FMHResourceKey ValidKey;
    ValidKey.Kind = EMHResourceKind::Material;
    ValidKey.LogicalName = TEXT("s2_stale_valid");
    const FMHMaterialOperationResult ValidImport = MHImportMaterialV4(
        MaterialEntry(
            TEXT("s2_stale_valid"),
            TEXT("s2_stale_valid.material"),
            ValidResolver.Resolve(ValidKey)),
        ValidResolver,
        SourceRoot,
        *Settings);
    bPassed &= TestTrue(TEXT("pre-existing texture is generated"), ValidImport.Succeeded());
    if (!ValidImport.Succeeded())
    {
        AddError(ValidImport.Error);
        return false;
    }

    bPassed &= TestTrue(
        TEXT("texture source replaced with invalid bytes"),
        FFileHelper::SaveArrayToFile(Utf8(TEXT("not_png")), *TexturePath));
    const FString InvalidPath = FPaths::Combine(SourceRoot, TEXT("s2_stale_invalid.material"));
    bPassed &= TestTrue(
        TEXT("stale failure material written"),
        FFileHelper::SaveArrayToFile(
            Utf8(TEXT("{\"class\":\"simple\",\"textures\":{\"tex0\":\"s2_stale_texture\"}}")),
            *InvalidPath));
    FMHPayloadScanResolver InvalidResolver(SourceRoot);
    bPassed &= TestTrue(TEXT("invalid changed source scans"), InvalidResolver.Initialize(Error));
    FMHResourceKey InvalidKey;
    InvalidKey.Kind = EMHResourceKind::Material;
    InvalidKey.LogicalName = TEXT("s2_stale_invalid");
    const FMHMaterialOperationResult InvalidImport = MHImportMaterialV4(
        MaterialEntry(
            TEXT("s2_stale_invalid"),
            TEXT("s2_stale_invalid.material"),
            InvalidResolver.Resolve(InvalidKey)),
        InvalidResolver,
        SourceRoot,
        *Settings);
    bPassed &= TestFalse(TEXT("failed task cannot reuse stale texture"), InvalidImport.Succeeded());
    bPassed &= TestTrue(
        TEXT("failed task exact code"),
        ErrorStartsWith(InvalidImport.Error, TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHMaterialAppliedStateAndRaceTest,
    "Mimir.V4.Material.AppliedStateAndRace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHMaterialAppliedStateAndRaceTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir/MaterialAppliedState"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString RawPath = FPaths::Combine(SourceRoot, TEXT("s2_raw_hash.material"));
    const TArray<uint8> RawBytes = Utf8(
        TEXT("{\"class\":\"simple\",\"params\":{\"roughness\":0.10000000001}}"));
    bool bPassed = TestTrue(TEXT("noncanonical raw source written"), FFileHelper::SaveArrayToFile(RawBytes, *RawPath));
    const FString RacePath = FPaths::Combine(SourceRoot, TEXT("s2_source_race_guard.material"));
    const TArray<uint8> RaceInitial = Utf8(TEXT("{\"class\":\"simple\",\"params\":{\"roughness\":0.25}}"));
    bPassed &= TestTrue(TEXT("race source written"), FFileHelper::SaveArrayToFile(RaceInitial, *RacePath));
    const FString ExistingRacePath = FPaths::Combine(SourceRoot, TEXT("s2_existing_race_guard.material"));
    const TArray<uint8> ExistingInitial = Utf8(
        TEXT("{\"class\":\"simple\",\"params\":{\"roughness\":0.2}}"));
    bPassed &= TestTrue(
        TEXT("existing-race source written"),
        FFileHelper::SaveArrayToFile(ExistingInitial, *ExistingRacePath));

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    MakeParent(Settings->MasterRoot, TEXT("simple"));
    FString Error;
    FMHPayloadScanResolver Resolver(SourceRoot);
    bPassed &= TestTrue(TEXT("applied-state source scans"), Resolver.Initialize(Error));

    FMHResourceKey RawKey;
    RawKey.Kind = EMHResourceKind::Material;
    RawKey.LogicalName = TEXT("s2_raw_hash");
    const FMHResolveOutcome RawResolved = Resolver.Resolve(RawKey);
    const FMHMaterialOperationResult Imported = MHImportMaterialV4(
        MaterialEntry(TEXT("s2_raw_hash"), TEXT("s2_raw_hash.material"), RawResolved),
        Resolver,
        SourceRoot,
        *Settings);
    bPassed &= TestTrue(TEXT("noncanonical raw source imports"), Imported.Succeeded());
    if (!Imported.Succeeded())
    {
        AddError(Imported.Error);
        return false;
    }
    const UMHMaterialSourceData* Data = Cast<UMHMaterialSourceData>(
        Imported.Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    bPassed &= TestNotNull(TEXT("applied receipt exists"), Data);
    if (Data == nullptr)
    {
        return false;
    }
    bPassed &= TestTrue(TEXT("applied receipt is cook-stripped"), Data->IsEditorOnly());
    bPassed &= TestEqual(TEXT("SourceHash is exact raw bytes"), Data->SourceHash, MHRawPayloadHash(RawBytes));
    bPassed &= TestNotEqual(TEXT("raw and canonical hashes differ"), Data->SourceHash, Data->AppliedHash);
    FMHMaterialDocument Actual;
    TArray<uint8> ActualBytes;
    bPassed &= TestTrue(TEXT("actual MI extracts"), MHExtractMaterialV4(*Imported.Material, *Settings, Actual, Error));
    bPassed &= TestTrue(TEXT("actual MI canonicalizes"), MHWriteCanonicalMaterialV4(Actual, ActualBytes, Error));
    bPassed &= TestEqual(
        TEXT("AppliedHash comes from actual compiled MI"),
        Data->AppliedHash,
        MHRawPayloadHash(ActualBytes));

    FMHResourceKey ExistingKey;
    ExistingKey.Kind = EMHResourceKind::Material;
    ExistingKey.LogicalName = TEXT("s2_existing_race_guard");
    const FMHMaterialOperationResult ExistingImported = MHImportMaterialV4(
        MaterialEntry(
            TEXT("s2_existing_race_guard"),
            TEXT("s2_existing_race_guard.material"),
            Resolver.Resolve(ExistingKey)),
        Resolver,
        SourceRoot,
        *Settings);
    bPassed &= TestTrue(TEXT("existing race fixture imports"), ExistingImported.Succeeded());
    if (!ExistingImported.Succeeded())
    {
        AddError(ExistingImported.Error);
        return false;
    }
    const UMHMaterialSourceData* ExistingData = Cast<UMHMaterialSourceData>(
        ExistingImported.Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    bPassed &= TestNotNull(TEXT("existing race receipt exists"), ExistingData);
    if (ExistingData == nullptr)
    {
        return false;
    }
    const FString ExistingSourceHash = ExistingData->SourceHash;
    const FString ExistingAppliedHash = ExistingData->AppliedHash;
    FMHMaterialDocument ExistingBeforeDocument;
    TArray<uint8> ExistingBeforeBytes;
    bPassed &= TestTrue(
        TEXT("existing fixture extracts before race"),
        MHExtractMaterialV4(*ExistingImported.Material, *Settings, ExistingBeforeDocument, Error));
    bPassed &= TestTrue(
        TEXT("existing fixture canonicalizes before race"),
        MHWriteCanonicalMaterialV4(ExistingBeforeDocument, ExistingBeforeBytes, Error));
    bPassed &= TestFalse(
        TEXT("existing package is clean before race"),
        ExistingImported.Material->GetOutermost()->IsDirty());

    FMHResourceKey RaceKey;
    RaceKey.Kind = EMHResourceKind::Material;
    RaceKey.LogicalName = TEXT("s2_source_race_guard");
    const FMHResolveOutcome RaceResolved = Resolver.Resolve(RaceKey);
    const TArray<uint8> RaceMutation = Utf8(
        TEXT("{\"class\":\"simple\",\"params\":{\"roughness\":0.75}}"));
    MHSetBeforeMaterialSourceCommitTestHook([RacePath, RaceMutation]()
    {
        FFileHelper::SaveArrayToFile(RaceMutation, *RacePath);
    });
    const FMHMaterialOperationResult Raced = MHImportMaterialV4(
        MaterialEntry(TEXT("s2_source_race_guard"), TEXT("s2_source_race_guard.material"), RaceResolved),
        Resolver,
        SourceRoot,
        *Settings);
    MHSetBeforeMaterialSourceCommitTestHook(TFunction<void()>());
    bPassed &= TestFalse(TEXT("late source mutation blocks commit"), Raced.Succeeded());
    bPassed &= TestTrue(
        TEXT("late source mutation exact code"),
        ErrorStartsWith(Raced.Error, TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED")));

    const FString GhostPackageName = TEXT("/Game/MH/Generated/Materials/s2_source_race_guard");
    const FString GhostObjectPath = GhostPackageName + TEXT(".s2_source_race_guard");
    bPassed &= TestNull(
        TEXT("new race creates no MIC ghost"),
        FindObject<UMaterialInstanceConstant>(nullptr, *GhostObjectPath));
    bPassed &= TestNull(TEXT("new race creates no package ghost"), FindPackage(nullptr, *GhostPackageName));
    const FAssetData GhostAsset = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
        .Get()
        .GetAssetByObjectPath(FSoftObjectPath(GhostObjectPath));
    bPassed &= TestFalse(TEXT("new race creates no registry ghost"), GhostAsset.IsValid());

    const TArray<uint8> ExistingTarget = Utf8(
        TEXT("{\"class\":\"simple\",\"params\":{\"roughness\":0.75}}"));
    bPassed &= TestTrue(
        TEXT("existing source target written"),
        FFileHelper::SaveArrayToFile(ExistingTarget, *ExistingRacePath));
    FMHPayloadScanResolver ExistingResolver(SourceRoot);
    bPassed &= TestTrue(TEXT("existing race target scans"), ExistingResolver.Initialize(Error));
    const TArray<uint8> ExistingMutation = Utf8(
        TEXT("{\"class\":\"simple\",\"params\":{\"roughness\":0.9}}"));
    MHSetBeforeMaterialSourceCommitTestHook([ExistingRacePath, ExistingMutation]()
    {
        FFileHelper::SaveArrayToFile(ExistingMutation, *ExistingRacePath);
    });
    const FMHMaterialOperationResult ExistingRaced = MHImportMaterialV4(
        MaterialEntry(
            TEXT("s2_existing_race_guard"),
            TEXT("s2_existing_race_guard.material"),
            ExistingResolver.Resolve(ExistingKey)),
        ExistingResolver,
        SourceRoot,
        *Settings);
    MHSetBeforeMaterialSourceCommitTestHook(TFunction<void()>());
    bPassed &= TestFalse(TEXT("existing race blocks reimport"), ExistingRaced.Succeeded());
    bPassed &= TestTrue(
        TEXT("existing race exact code"),
        ErrorStartsWith(ExistingRaced.Error, TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED")));
    FMHMaterialDocument ExistingAfterDocument;
    TArray<uint8> ExistingAfterBytes;
    bPassed &= TestTrue(
        TEXT("existing fixture extracts after race"),
        MHExtractMaterialV4(*ExistingImported.Material, *Settings, ExistingAfterDocument, Error));
    bPassed &= TestTrue(
        TEXT("existing fixture canonicalizes after race"),
        MHWriteCanonicalMaterialV4(ExistingAfterDocument, ExistingAfterBytes, Error));
    bPassed &= TestTrue(
        TEXT("existing MIC remains byte-identical"),
        ExistingAfterBytes == ExistingBeforeBytes);
    bPassed &= TestEqual(
        TEXT("existing SourceHash unchanged"),
        ExistingData->SourceHash,
        ExistingSourceHash);
    bPassed &= TestEqual(
        TEXT("existing AppliedHash unchanged"),
        ExistingData->AppliedHash,
        ExistingAppliedHash);
    bPassed &= TestFalse(
        TEXT("existing package remains clean"),
        ExistingImported.Material->GetOutermost()->IsDirty());
    return bPassed;
}

bool FMHMaterialManagedRoundTripTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir/MaterialRoundTrip"));
    const FString SourcePath = FPaths::Combine(SourceRoot, TEXT("round_trip.material"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);

    FMHMaterialDocument Initial;
    Initial.Parent = TEXT("simple");
    FMHMaterialParameter Roughness;
    Roughness.Scalar = 0.25f;
    Initial.Params.Add(TEXT("roughness"), Roughness);
    TArray<uint8> InitialBytes;
    FString Error;
    if (!MHWriteCanonicalMaterialV4(Initial, InitialBytes, Error) ||
        !FFileHelper::SaveArrayToFile(InitialBytes, *SourcePath))
    {
        AddError(Error.IsEmpty() ? TEXT("cannot create round-trip source") : Error);
        return false;
    }

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    MakeParent(Settings->MasterRoot, TEXT("simple"));

    FMHPayloadScanResolver Resolver(SourceRoot);
    bool bPassed = TestTrue(TEXT("round-trip source scan"), Resolver.Initialize(Error));
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Material;
    Key.LogicalName = TEXT("round_trip");
    const FMHResolveOutcome Resolved = Resolver.Resolve(Key);
    bPassed &= TestEqual(TEXT("round-trip resolve"), Resolved.Status, EMHResolveStatus::Resolved);
    FMHSourceAnalysisEntry Entry;
    Entry.Key = Key;
    Entry.PayloadPath = Resolved.PayloadPath;
    Entry.SourcePath = TEXT("round_trip.material");
    Entry.RawHash = Resolved.RawHash;
    Entry.Change = EMHSourceChange::Create;
    FMHMaterialOperationResult Imported = MHImportMaterialV4(Entry, Resolver, SourceRoot, *Settings);
    bPassed &= TestTrue(TEXT("class material imports"), Imported.Succeeded());
    if (!Imported.Succeeded())
    {
        AddError(Imported.Error);
        return false;
    }
    UMHMaterialSourceData* ImportedData = Cast<UMHMaterialSourceData>(
        Imported.Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    bPassed &= TestNotNull(TEXT("class receipt exists"), ImportedData);
    if (ImportedData == nullptr)
    {
        return false;
    }
    bPassed &= TestEqual(
        TEXT("class receipt uses tagged applied parent"),
        ImportedData->AppliedParent,
        FString(TEXT("class:simple")));

    const FAssetData AssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
        .Get()
        .GetAssetByObjectPath(FSoftObjectPath::ConstructFromObject(Imported.Material));
    FString TagValue;
    bPassed &= TestTrue(TEXT("managed material is indexed"), AssetData.IsValid());
    bPassed &= TestTrue(
        TEXT("kind registry tag"),
        AssetData.GetTagValue(FName(TEXT("MH.Kind")), TagValue) && TagValue == TEXT("material"));
    bPassed &= TestTrue(
        TEXT("logical-name registry tag"),
        AssetData.GetTagValue(FName(TEXT("MH.LogicalName")), TagValue) && TagValue == TEXT("round_trip"));
    bPassed &= TestTrue(
        TEXT("source-path registry tag"),
        AssetData.GetTagValue(FName(TEXT("MH.SourcePath")), TagValue) && TagValue == TEXT("round_trip.material"));
    bPassed &= TestTrue(
        TEXT("applied-hash registry tag"),
        AssetData.GetTagValue(FName(TEXT("MH.AppliedHash")), TagValue) &&
            TagValue == MHRawPayloadHash(InitialBytes));
    bPassed &= TestTrue(
        TEXT("managed registry tag"),
        AssetData.GetTagValue(FName(TEXT("MH.Managed")), TagValue) && TagValue == TEXT("True"));
    TSet<FName> MHTags;
    AssetData.TagsAndValues.ForEach([&MHTags](const TPair<FName, FAssetTagValueRef>& Pair)
    {
        if (Pair.Key.ToString().StartsWith(TEXT("MH."), ESearchCase::CaseSensitive))
        {
            MHTags.Add(Pair.Key);
        }
    });
    bPassed &= TestEqual(TEXT("asset registry exposes exactly five MH tags"), MHTags.Num(), 5);
    bPassed &= TestTrue(TEXT("tag set contains kind"), MHTags.Contains(FName(TEXT("MH.Kind"))));
    bPassed &= TestTrue(TEXT("tag set contains logical name"), MHTags.Contains(FName(TEXT("MH.LogicalName"))));
    bPassed &= TestTrue(TEXT("tag set contains source path"), MHTags.Contains(FName(TEXT("MH.SourcePath"))));
    bPassed &= TestTrue(TEXT("tag set contains applied hash"), MHTags.Contains(FName(TEXT("MH.AppliedHash"))));
    bPassed &= TestTrue(TEXT("tag set contains managed"), MHTags.Contains(FName(TEXT("MH.Managed"))));
    bPassed &= TestFalse(TEXT("AppliedParent is receipt-only, not a tag"), MHTags.Contains(FName(TEXT("MH.AppliedParent"))));

    Imported.Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("roughness")), 0.75f);
    FMHMaterialOperationResult Published = MHPublishMaterialV4(*Imported.Material, SourceRoot, *Settings);
    bPassed &= TestTrue(TEXT("managed material publishes"), Published.Succeeded());
    if (!Published.Succeeded())
    {
        AddError(Published.Error);
        return false;
    }
    TArray<uint8> PublishedBytes;
    bPassed &= TestTrue(TEXT("published source readable"), FFileHelper::LoadFileToArray(PublishedBytes, *SourcePath));
    FMHMaterialDocument PublishedDocument;
    bPassed &= TestTrue(TEXT("published source parses"), MHParseMaterialV4(PublishedBytes, PublishedDocument, Error));
    const FMHMaterialParameter* PublishedRoughness = PublishedDocument.Params.Find(TEXT("roughness"));
    bPassed &= TestTrue(TEXT("published edit present"), PublishedRoughness != nullptr && PublishedRoughness->Scalar == 0.75f);

    FMHPayloadScanResolver ReimportResolver(SourceRoot);
    bPassed &= TestTrue(TEXT("published source rescans"), ReimportResolver.Initialize(Error));
    const FMHResolveOutcome ReimportResolved = ReimportResolver.Resolve(Key);
    Entry.PayloadPath = ReimportResolved.PayloadPath;
    Entry.RawHash = ReimportResolved.RawHash;
    Entry.Change = EMHSourceChange::Reimport;
    FMHMaterialOperationResult Reimported = MHImportMaterialV4(Entry, ReimportResolver, SourceRoot, *Settings);
    bPassed &= TestTrue(TEXT("published source reimports"), Reimported.Succeeded());
    bPassed &= TestTrue(TEXT("reimport has no local-change warning"), Reimported.Warnings.IsEmpty());
    FMHMaterialDocument FinalExtract;
    TArray<uint8> FinalBytes;
    bPassed &= TestTrue(TEXT("final extract"), MHExtractMaterialV4(*Reimported.Material, *Settings, FinalExtract, Error));
    bPassed &= TestTrue(TEXT("final canonical"), MHWriteCanonicalMaterialV4(FinalExtract, FinalBytes, Error));
    bPassed &= TestTrue(TEXT("publish reimport is NO_CHANGE exact"), FinalBytes == PublishedBytes);
    return bPassed;
}

bool FMHMaterialAppliedParentReceiptsTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir(),
        TEXT("Mimir/MaterialAppliedParentReceipts"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString ManagedSourcePath = FPaths::Combine(SourceRoot, TEXT("s2_open9_library_managed.material"));
    FMHMaterialDocument LibrarySource;
    LibrarySource.Mode = EMHMaterialMode::Library;
    LibrarySource.Parent = TEXT("open9_library_parent");
    TArray<uint8> LibraryBytes;
    FString Error;
    bool bPassed = TestTrue(
        TEXT("library source canonicalizes"),
        MHWriteCanonicalMaterialV4(LibrarySource, LibraryBytes, Error));
    bPassed &= TestTrue(
        TEXT("library source written"),
        FFileHelper::SaveArrayToFile(LibraryBytes, *ManagedSourcePath));

    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    Settings->LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    UMaterial* ClassParent = MakeParent(Settings->MasterRoot, TEXT("open9_class_parent"));
    UMaterial* LibraryParent = MakeParent(Settings->LibraryRoot, TEXT("open9_library_parent"));

    const FString ManagedPackageName = TEXT("/Game/MH/Generated/Materials/s2_open9_library_managed");
    const FString ManagedObjectPath = ManagedPackageName + TEXT(".s2_open9_library_managed");
    UMaterialInstanceConstant* ExistingMaterial = LoadObject<UMaterialInstanceConstant>(nullptr, *ManagedObjectPath);
    if (ExistingMaterial == nullptr)
    {
        UPackage* ManagedPackage = CreatePackage(*ManagedPackageName);
        ExistingMaterial = NewObject<UMaterialInstanceConstant>(
            ManagedPackage,
            TEXT("s2_open9_library_managed"),
            RF_Public | RF_Standalone | RF_Transactional);
    }
    ExistingMaterial->RemoveUserDataOfClass(UMHMaterialSourceData::StaticClass());
    ExistingMaterial->SetParentEditorOnly(ClassParent);
    ExistingMaterial->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("scalar")), 1.0f);
    ExistingMaterial->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("vector")), FLinearColor::White);
    ExistingMaterial->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(TEXT("tex0")),
        MakeTexture(TEXT("open9_texture")));
    FMaterialInstanceBasePropertyOverrides Overrides;
    Overrides.bOverride_TwoSided = true;
    Overrides.TwoSided = true;
    ExistingMaterial->BasePropertyOverrides = Overrides;

    FMHPayloadScanResolver Resolver(SourceRoot);
    bPassed &= TestTrue(TEXT("library source scans"), Resolver.Initialize(Error));
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Material;
    Key.LogicalName = TEXT("s2_open9_library_managed");
    const FMHResolveOutcome Resolved = Resolver.Resolve(Key);
    const FMHMaterialOperationResult Imported = MHImportMaterialV4(
        MaterialEntry(Key.LogicalName, TEXT("s2_open9_library_managed.material"), Resolved),
        Resolver,
        SourceRoot,
        *Settings);
    bPassed &= TestTrue(TEXT("library material imports over an existing class MI"), Imported.Succeeded());
    if (!Imported.Succeeded())
    {
        AddError(Imported.Error);
        return false;
    }
    bPassed &= TestEqual(TEXT("library import is in-place"), Imported.Material, ExistingMaterial);
    bPassed &= TestEqual(
        TEXT("library import reparents from current settings root"),
        Imported.Material->Parent.Get(),
        static_cast<UMaterialInterface*>(LibraryParent));
    bPassed &= TestTrue(TEXT("library import clears scalar overrides"), Imported.Material->ScalarParameterValues.IsEmpty());
    bPassed &= TestTrue(TEXT("library import clears vector overrides"), Imported.Material->VectorParameterValues.IsEmpty());
    bPassed &= TestTrue(TEXT("library import clears texture overrides"), Imported.Material->TextureParameterValues.IsEmpty());
    bPassed &= TestFalse(TEXT("library import clears base overrides"), Imported.Material->HasOverridenBaseProperties());
    bPassed &= TestFalse(TEXT("library import clears static overrides"), Imported.Material->HasStaticParameters());

    UMHMaterialSourceData* LibraryData = Cast<UMHMaterialSourceData>(
        Imported.Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    bPassed &= TestNotNull(TEXT("library import receipt exists"), LibraryData);
    if (LibraryData == nullptr)
    {
        return false;
    }
    bPassed &= TestEqual(
        TEXT("library import persists tagged applied parent"),
        LibraryData->AppliedParent,
        FString(TEXT("library:open9_library_parent")));
    bPassed &= TestFalse(TEXT("library import package is persisted"), Imported.Material->GetOutermost()->IsDirty());

    LibraryData->AppliedParent = TEXT("class:stale_receipt");
    const FMHMaterialOperationResult Published = MHPublishMaterialV4(*Imported.Material, SourceRoot, *Settings);
    bPassed &= TestTrue(TEXT("library publish ignores stale AppliedParent"), Published.Succeeded());
    if (!Published.Succeeded())
    {
        AddError(Published.Error);
        return false;
    }
    bPassed &= TestEqual(
        TEXT("library publish refreshes tagged applied parent from live parent"),
        LibraryData->AppliedParent,
        FString(TEXT("library:open9_library_parent")));
    TArray<uint8> PublishedBytes;
    bPassed &= TestTrue(TEXT("library publish source readable"), FFileHelper::LoadFileToArray(PublishedBytes, *ManagedSourcePath));
    bPassed &= TestTrue(TEXT("library publish source remains canonical"), PublishedBytes == LibraryBytes);

    const FString PublishedSourceHash = LibraryData->SourceHash;
    const FString PublishedAppliedHash = LibraryData->AppliedHash;
    const FString PublishedAppliedParent = LibraryData->AppliedParent;
    Imported.Material->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("local_scalar")), 0.5f);
    const FMHMaterialOperationResult RejectedPublish = MHPublishMaterialV4(*Imported.Material, SourceRoot, *Settings);
    bPassed &= TestFalse(TEXT("library publish rejects local scalar override"), RejectedPublish.Succeeded());
    bPassed &= TestTrue(
        TEXT("library override publish exact diagnostic"),
        ErrorStartsWith(RejectedPublish.Error, TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE")));
    TArray<uint8> BytesAfterRejection;
    bPassed &= TestTrue(
        TEXT("library source remains readable after rejected publish"),
        FFileHelper::LoadFileToArray(BytesAfterRejection, *ManagedSourcePath));
    bPassed &= TestTrue(
        TEXT("rejected library publish does not write source"),
        BytesAfterRejection == PublishedBytes);
    bPassed &= TestEqual(
        TEXT("rejected library publish does not advance SourceHash"),
        LibraryData->SourceHash,
        PublishedSourceHash);
    bPassed &= TestEqual(
        TEXT("rejected library publish does not advance AppliedHash"),
        LibraryData->AppliedHash,
        PublishedAppliedHash);
    bPassed &= TestEqual(
        TEXT("rejected library publish does not advance AppliedParent"),
        LibraryData->AppliedParent,
        PublishedAppliedParent);
    bPassed &= TestTrue(
        TEXT("library fixture restores clean full-applied state"),
        MHApplyMaterialV4(*Imported.Material, *LibraryParent, LibrarySource, {}, Error));

    auto AdoptMaterial = [&](const FString& AssetName, UMaterialInterface* Parent, const FString& LogicalName, const FString& ExpectedReceipt)
    {
        const FString PackageName = TEXT("/Game/MH/Generated/Materials/") + AssetName;
        const FString ObjectPath = PackageName + TEXT(".") + AssetName;
        UMaterialInstanceConstant* Material = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
        if (Material == nullptr)
        {
            UPackage* NewPackage = CreatePackage(*PackageName);
            Material = NewObject<UMaterialInstanceConstant>(
                NewPackage,
                FName(*AssetName),
                RF_Public | RF_Standalone | RF_Transactional);
        }
        Material->RemoveUserDataOfClass(UMHMaterialSourceData::StaticClass());
        Material->ClearParameterValuesEditorOnly();
        Material->SetParentEditorOnly(Parent);
        FStaticParameterSet EmptyStaticParameters;
        FMaterialInstanceBasePropertyOverrides EmptyBaseOverrides;
        Material->UpdateStaticPermutation(EmptyStaticParameters, EmptyBaseOverrides);

        FMHMaterialAdoptTarget Target;
        Target.Folder = FPaths::Combine(SourceRoot, TEXT("adopted"));
        Target.LogicalName = LogicalName;
        const FMHMaterialOperationResult Adopted = MHPublishMaterialV4(*Material, SourceRoot, *Settings, &Target);
        bPassed &= TestTrue(*FString::Printf(TEXT("%s adopts"), *LogicalName), Adopted.Succeeded());
        if (!Adopted.Succeeded())
        {
            AddError(Adopted.Error);
            return;
        }
        const UMHMaterialSourceData* Data = Cast<UMHMaterialSourceData>(
            Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
        bPassed &= TestNotNull(*FString::Printf(TEXT("%s receipt exists"), *LogicalName), Data);
        if (Data != nullptr)
        {
            bPassed &= TestEqual(
                *FString::Printf(TEXT("%s tagged applied parent"), *LogicalName),
                Data->AppliedParent,
                ExpectedReceipt);
        }
        bPassed &= TestFalse(
            *FString::Printf(TEXT("%s package is persisted"), *LogicalName),
            Material->GetOutermost()->IsDirty());
    };
    AdoptMaterial(
        TEXT("s2_open9_class_adopt_asset"),
        ClassParent,
        TEXT("s2_open9_class_adopt"),
        TEXT("class:open9_class_parent"));
    AdoptMaterial(
        TEXT("s2_open9_library_adopt_asset"),
        LibraryParent,
        TEXT("s2_open9_library_adopt"),
        TEXT("library:open9_library_parent"));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
