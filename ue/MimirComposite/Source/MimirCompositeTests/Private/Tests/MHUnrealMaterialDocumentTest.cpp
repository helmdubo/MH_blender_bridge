#include "Material/MHUnrealMaterialDocument.h"

#include "Canonical/MHCanonical.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<uint8> UnrealDocumentUtf8(const FString& Text)
{
    const FTCHARToUTF8 Bytes(*Text, Text.Len());
    return TArray<uint8>(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
}

bool ParseUnrealText(const FString& Text, FMHMaterialDocument& Document, FString& Error)
{
    return MHParseMaterialV4(UnrealDocumentUtf8(Text), Document, Error);
}

FString NativeJson(const FString& Tail = FString())
{
    return TEXT("{\"ue_instance\":{\"version\":1,\"parent\":\"/Game/MH/CodecParent.CodecParent\"") + Tail + TEXT("}}");
}

FString ScalarRow(const FString& Name, const FString& Value = TEXT("0.3"), const int32 Association = 2, const int32 Index = -1)
{
    return FString::Printf(TEXT("{\"name\":\"%s\",\"association\":%d,\"index\":%d,\"value\":%s}"),
        *Name, Association, Index, *Value);
}

UMaterial* CodecParent(const FString& Name)
{
    UPackage* Package = CreatePackage(*(TEXT("/Game/MH/UnrealDocumentTests/") + Name));
    return NewObject<UMaterial>(Package, FName(*Name), RF_Public | RF_Standalone);
}

UMaterialInstanceConstant* CodecInstance(const FString& Name, UMaterialInterface* Parent)
{
    UPackage* Package = CreatePackage(*(TEXT("/Game/MH/UnrealDocumentTests/") + Name));
    UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package, FName(*Name), RF_Public | RF_Standalone);
    Instance->SetParentEditorOnly(Parent);
    return Instance;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHUnrealMaterialDocumentCodecTest,
    "Mimir.V5.Material.UnrealDocument.Codec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHUnrealMaterialDocumentCodecTest::RunTest(const FString& Parameters)
{
    FMHMaterialDocument Document;
    FString Error;
    const auto Reject = [&](const TCHAR* Label, const FString& Json)
    {
        Error.Reset();
        const bool bRejected = !ParseUnrealText(Json, Document, Error);
        TestTrue(Label, bRejected);
        TestTrue(FString(Label) + TEXT(" reports an error"), !Error.IsEmpty());
    };
    Reject(TEXT("null root rejected"), TEXT("null"));
    Reject(TEXT("null payload rejected"), TEXT("{\"ue_instance\":null}"));
    Reject(TEXT("unknown native field rejected"), NativeJson(TEXT(",\"extra\":true")));
    Reject(TEXT("missing version rejected"), TEXT("{\"ue_instance\":{\"parent\":\"/Game/MH/P.P\"}}"));
    Reject(TEXT("unknown version rejected"), TEXT("{\"ue_instance\":{\"version\":2,\"parent\":\"/Game/MH/P.P\"}}"));
    Reject(TEXT("boolean version rejected"), TEXT("{\"ue_instance\":{\"version\":true,\"parent\":\"/Game/MH/P.P\"}}"));
    Reject(TEXT("null parent rejected"), TEXT("{\"ue_instance\":{\"version\":1,\"parent\":null}}"));
    Reject(TEXT("mixed class native rejected"), TEXT("{\"class\":\"simple\",\"ue_instance\":{\"version\":1,\"parent\":\"/Game/MH/P.P\"}}"));
    Reject(TEXT("null scalar array rejected"), NativeJson(TEXT(",\"scalars\":null")));
    Reject(TEXT("null base rejected"), NativeJson(TEXT(",\"base_overrides\":null")));
    Reject(TEXT("unknown base field rejected"), NativeJson(TEXT(",\"base_overrides\":{\"NotAProperty\":true}")));
    Reject(TEXT("association 256 rejected without wrapping"), NativeJson(TEXT(",\"scalars\":[") + ScalarRow(TEXT("roughness"), TEXT("0.3"), 256, 0) + TEXT("]")));
    Reject(TEXT("global parameter index rejected"), NativeJson(TEXT(",\"scalars\":[") + ScalarRow(TEXT("roughness"), TEXT("0.3"), 2, 0) + TEXT("]")));
    Reject(TEXT("negative layer index rejected"), NativeJson(TEXT(",\"scalars\":[") + ScalarRow(TEXT("roughness"), TEXT("0.3"), 0, -1) + TEXT("]")));
    Reject(TEXT("case-insensitive duplicate identity rejected"), NativeJson(TEXT(",\"scalars\":[") +
        ScalarRow(TEXT("Roughness")) + TEXT(",") + ScalarRow(TEXT("roughness")) + TEXT("]")));
    Reject(TEXT("NUL parameter name rejected"), NativeJson(TEXT(",\"scalars\":[") + ScalarRow(TEXT("a\\u0000b")) + TEXT("]")));
    Reject(TEXT("float32 overflow rejected"), NativeJson(TEXT(",\"scalars\":[") + ScalarRow(TEXT("roughness"), TEXT("1e39")) + TEXT("]")));
    Reject(TEXT("nonfinite literal rejected"), NativeJson(TEXT(",\"scalars\":[") + ScalarRow(TEXT("roughness"), TEXT("NaN")) + TEXT("]")));

    const FString First = NativeJson(TEXT(",\"scalars\":[") + ScalarRow(TEXT("Z Parameter")) + TEXT(",") + ScalarRow(TEXT("A Parameter"), TEXT("-0.0")) + TEXT("]"));
    if (!TestTrue(TEXT("normal nonempty names parse"), ParseUnrealText(First, Document, Error))) return false;
    TestEqual(TEXT("native mode"), Document.Mode, EMHMaterialMode::UnrealInstance);
    TestEqual(TEXT("exact scalar float32 preserved"), Document.UnrealInstance->Scalars[0].Value, 0.3f);
    TArray<uint8> Canonical;
    if (!TestTrue(TEXT("native canonical write"), MHWriteCanonicalMaterialV4(Document, Canonical, Error))) return false;
    FMHMaterialDocument ReadBack;
    TArray<uint8> Rewritten;
    TestTrue(TEXT("canonical parse"), MHParseMaterialV4(Canonical, ReadBack, Error));
    TestTrue(TEXT("canonical rewrite"), MHWriteCanonicalMaterialV4(ReadBack, Rewritten, Error));
    TestTrue(TEXT("float32 canonical bytes roundtrip"), Rewritten == Canonical);
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Canonical.GetData()), Canonical.Num());
    const FString CanonicalText(Converted.Length(), Converted.Get());
    TestTrue(TEXT("shortest float32 decimal"), CanonicalText.Contains(TEXT("\"value\": 0.3")));
    TestTrue(TEXT("parameter identities sorted"), CanonicalText.Find(TEXT("A Parameter")) < CanonicalText.Find(TEXT("Z Parameter")));

    TSharedPtr<FJsonValue> Root;
    if (!TestTrue(TEXT("canonical root available"), MHParseJsonUtf8(Canonical, Root).bSuccess)) return false;
    const auto Payload = Root->AsObject()->GetObjectField(TEXT("ue_instance"));
    const auto Base = Payload->GetObjectField(TEXT("base_overrides"));
    Base->SetNumberField(TEXT("BlendMode"), static_cast<int32>(BLEND_MAX));
    TestFalse(TEXT("BlendMode sentinel rejected"), MHParseUnrealMaterialV1(Payload, ReadBack, Error));
    Base->SetNumberField(TEXT("BlendMode"), 256);
    TestFalse(TEXT("BlendMode out-of-range rejected"), MHParseUnrealMaterialV1(Payload, ReadBack, Error));
    Base->SetNumberField(TEXT("BlendMode"), static_cast<int32>(BLEND_Opaque));
    Base->SetNumberField(TEXT("ShadingModel"), static_cast<int32>(MSM_NUM));
    TestFalse(TEXT("ShadingModel count sentinel rejected"), MHParseUnrealMaterialV1(Payload, ReadBack, Error));
    Base->SetNumberField(TEXT("ShadingModel"), static_cast<int32>(MSM_MAX));
    TestFalse(TEXT("ShadingModel max sentinel rejected"), MHParseUnrealMaterialV1(Payload, ReadBack, Error));

    Document.Mode = EMHMaterialMode::Class;
    Document.Parent = TEXT("simple");
    TestFalse(TEXT("legacy DTO cannot silently discard native data"), MHWriteCanonicalMaterialV4(Document, Rewritten, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHUnrealMaterialDocumentApplyGuardsTest,
    "Mimir.V5.Material.UnrealDocument.ApplyGuards",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHUnrealMaterialDocumentApplyGuardsTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    UMaterial* OldParent = CodecParent(TEXT("OldParent_") + Suffix);
    UMaterial* NewParent = CodecParent(TEXT("NewParent_") + Suffix);
    UMaterialInstanceConstant* Target = CodecInstance(TEXT("Target_") + Suffix, OldParent);
    Target->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Preserved Scalar")), 0.625f);
    Target->BasePropertyOverrides.bOverride_TwoSided = true;
    Target->BasePropertyOverrides.TwoSided = true;
    FMHMaterialDocument Before;
    TArray<uint8> BeforeBytes;
    FString Error;
    if (!TestTrue(TEXT("existing state captures"), MHExtractUnrealMaterialV1(*Target, Before, Error)) ||
        !TestTrue(TEXT("existing state canonical"), MHWriteUnrealMaterialV1(Before, BeforeBytes, Error))) return false;
    const auto Unchanged = [&]()
    {
        FMHMaterialDocument After;
        TArray<uint8> AfterBytes;
        TestTrue(TEXT("failed apply preserves target parent"), Target->Parent == OldParent);
        TestTrue(TEXT("failed apply state extracts"), MHExtractUnrealMaterialV1(*Target, After, Error));
        TestTrue(TEXT("failed apply state writes"), MHWriteUnrealMaterialV1(After, AfterBytes, Error));
        TestTrue(TEXT("failed apply leaves complete target unchanged"), BeforeBytes == AfterBytes);
    };
    FMHMaterialDocument Source;
    Source.Mode = EMHMaterialMode::UnrealInstance;
    Source.Parent = NewParent->GetPathName();
    Source.UnrealInstance = MakeShared<FMHUnrealMaterialInstanceData>();
    Source.UnrealInstance->Parent = FSoftObjectPath(NewParent);
    // An existing object of the wrong class is not a resolvable texture dependency;
    // unlike a missing package this fixture does not produce unrelated loader logs.
    Source.UnrealInstance->Textures.Emplace(FMaterialParameterInfo(TEXT("Albedo Texture")), FSoftObjectPath(OldParent));
    TestFalse(TEXT("missing texture dependency rejects before mutation"), MHApplyUnrealMaterialV1(*Target, *NewParent, Source, Error));
    Unchanged();
    Source.UnrealInstance->Textures.Reset();
    UMaterialInstanceConstant* Child = CodecInstance(TEXT("Child_") + Suffix, Target);
    Source.Parent = Child->GetPathName();
    Source.UnrealInstance->Parent = FSoftObjectPath(Child);
    TestFalse(TEXT("indirect parent cycle rejects before mutation"), MHApplyUnrealMaterialV1(*Target, *Child, Source, Error));
    Unchanged();
    Source.Parent = Target->GetPathName();
    Source.UnrealInstance->Parent = FSoftObjectPath(Target);
    TestFalse(TEXT("direct parent cycle rejects before mutation"), MHApplyUnrealMaterialV1(*Target, *Target, Source, Error));
    Unchanged();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHUnrealMaterialDocumentUnsupportedTerrainTest,
    "Mimir.V5.Material.UnrealDocument.UnsupportedTerrain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHUnrealMaterialDocumentUnsupportedTerrainTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
    UMaterial* Parent = CodecParent(TEXT("TerrainParent_") + Suffix);
    UMaterialInstanceConstant* Donor = CodecInstance(TEXT("TerrainDonor_") + Suffix, Parent);
    FStaticParameterSet Static = Donor->GetStaticParameters();
    FStaticTerrainLayerWeightParameter Terrain;
    Terrain.LayerName = FName(TEXT("Landscape Detail"));
    Terrain.WeightmapIndex = 0;
    Static.EditorOnly.TerrainLayerWeightParameters.Add(Terrain);
    {
        FMaterialUpdateContext UpdateContext;
        Donor->UpdateStaticPermutation(Static, Donor->BasePropertyOverrides);
        Donor->PostEditChange();
        UpdateContext.AddMaterialInstance(Donor);
    }
    if (!TestEqual(TEXT("fixture owns one terrain layer binding"),
            Donor->GetStaticParameters().EditorOnly.TerrainLayerWeightParameters.Num(), 1)) return false;
    FMHMaterialDocument Document;
    FString Error;
    TestFalse(TEXT("terrain layer bindings reject snapshot admission"),
        MHExtractUnrealMaterialV1(*Donor, Document, Error));
    TestTrue(TEXT("diagnostic identifies terrain state"), Error.Contains(TEXT("terrain layer weight parameters")));
    TestFalse(TEXT("failed admission produces no native payload"), Document.UnrealInstance.IsValid());
    TestEqual(TEXT("rejected donor retains terrain bindings"),
        Donor->GetStaticParameters().EditorOnly.TerrainLayerWeightParameters.Num(), 1);
    return true;
}

} // namespace UE::MimirComposite::Tests
