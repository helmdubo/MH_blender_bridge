#include "Source/MHFbxPassport.h"
#include "Source/MHPayloadHashes.h"

#include "Containers/StringConv.h"
#include "Misc/AutomationTest.h"

namespace UE::MimirComposite::Tests
{
namespace Private
{

const TCHAR* PythonPassportVector = TEXT(
    R"({"exporter":"mh4blend 0.5.0","geometry_hash":"xxh3:0123456789abcdef","kind":"static_mesh","lod_levels":[0,1],"lod_policy":"authored","material_slots":[{"material_name_hint":"M Stucco","material_uid":"22222222-2222-4222-8222-222222222222","slot_name":"surface"}],"name":"Wall A","properties":{"role":"wall"},"resource_uid":"11111111-1111-4111-8111-111111111111","schema":"mh.fbx_passport","schema_version":1})");

const TCHAR* PythonPassportRichVector = TEXT(
    R"({"exporter":"mh4blend 0.5.0","geometry_hash":"xxh3:0123456789abcdef","kind":"static_mesh","lod_levels":[0,1],"lod_policy":"authored","material_slots":[{"material_name_hint":"M Stucco","material_uid":"22222222-2222-4222-8222-222222222222","slot_name":"surface"}],"name":"Wall A","properties":{"A":1,"a":2,"float":1.0,"text":"Café\n","tiny":1e-07},"resource_uid":"11111111-1111-4111-8111-111111111111","schema":"mh.fbx_passport","schema_version":1})");

TArray<uint8> PassportTestUtf8(const ANSICHAR* Text)
{
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
    return Bytes;
}

} // namespace Private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHFbxPassportParityTest,
    "Mimir.C1.FbxPassportParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHFbxPassportParityTest::RunTest(const FString& Parameters)
{
    FMHFbxPassport Passport;
    FString Error;
    if (!TestTrue(
        TEXT("Python exact canonical_passport vector parses"),
        MHParseFbxPassportText(Private::PythonPassportVector, Passport, Error)))
    {
        AddError(Error);
        return false;
    }

    bool bPassed = TestEqual(
        TEXT("carrier bytes are retained"),
        Passport.CarrierText,
        Private::PythonPassportVector);
    bPassed &= TestEqual(
        TEXT("properties retain Python compact spelling"),
        Passport.PropertiesJson,
        TEXT("{\"role\":\"wall\"}"));
    bPassed &= TestEqual(
        TEXT("descriptor_hash matches Python sha256"),
        MHPassportDescriptorHash(Passport),
        TEXT("sha256:814282c42e4b768d7622f97ee9d8faa95c17f21a99d4304a29a70fcf64576610"));

    FMHFbxPassport RichPassport;
    FString RichError;
    if (!TestTrue(
        TEXT("case-distinct keys, Python floats, escaping, and UTF-8 parse"),
        MHParseFbxPassportText(Private::PythonPassportRichVector, RichPassport, RichError)))
    {
        AddError(RichError);
        bPassed = false;
    }
    else
    {
        bPassed &= TestEqual(
            TEXT("rich descriptor_hash matches Python sha256"),
            MHPassportDescriptorHash(RichPassport),
            TEXT("sha256:1d14db29a62380b7a75c41b85c6fcbc4685d24a37920e6c515ba13c10a3927d8"));
    }

    auto RejectNonCanonical = [this, &bPassed](const TCHAR* Label, FString Text)
    {
        FMHFbxPassport Rejected;
        FString RejectError;
        bPassed &= TestFalse(Label, MHParseFbxPassportText(Text, Rejected, RejectError));
        bPassed &= TestTrue(
            TEXT("noncanonical carrier has MH_E diagnostic"),
            RejectError.StartsWith(TEXT("MH_E_PASSPORT_INVALID:")));
    };

    FString Whitespace = Private::PythonPassportVector;
    Whitespace.ReplaceInline(TEXT(",\"geometry_hash\""), TEXT(", \"geometry_hash\""));
    RejectNonCanonical(TEXT("insignificant whitespace is rejected"), MoveTemp(Whitespace));

    FString NonNfc = Private::PythonPassportRichVector;
    NonNfc.ReplaceInline(TEXT("Café"), TEXT("Cafe\u0301"));
    RejectNonCanonical(TEXT("non-NFC carrier text is rejected"), MoveTemp(NonNfc));

    FString EscapedUnicode = Private::PythonPassportRichVector;
    EscapedUnicode.ReplaceInline(TEXT("Café"), TEXT("Caf\\u00e9"));
    RejectNonCanonical(TEXT("ensure_ascii escaping is rejected"), MoveTemp(EscapedUnicode));

    FString NonCanonicalFloat = Private::PythonPassportRichVector;
    NonCanonicalFloat.ReplaceInline(TEXT("\"float\":1.0"), TEXT("\"float\":1e0"));
    RejectNonCanonical(TEXT("non-Python float spelling is rejected"), MoveTemp(NonCanonicalFloat));

    const TArray<uint8> PayloadBytes = Private::PassportTestUtf8("Mimir payload bytes\n");
    bPassed &= TestEqual(
        TEXT("raw payload fingerprint matches Python sha256"),
        MHPayloadFingerprint(PayloadBytes),
        TEXT("sha256:91cfde6707cb1556e965131020483482467ae7b9aa9849d27912024a099c93db"));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
