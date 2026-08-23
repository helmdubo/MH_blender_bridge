#include "Source/MHPayloadHashes.h"

#include "Containers/StringConv.h"
#include "Misc/AutomationTest.h"

namespace UE::MimirComposite::Tests
{
namespace Private
{

TArray<uint8> PayloadHashUtf8(const FString& Text)
{
    FTCHARToUTF8 Utf8(*Text, Text.Len());
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return Bytes;
}

} // namespace Private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPayloadHashParityTest,
    "Mimir.C1.PayloadHashParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPayloadHashParityTest::RunTest(const FString& Parameters)
{
    bool bPassed = true;

    // Fixed expected values come from the Python reference implementations
    // material_content_hash() and composite_content_hash().

    const FString MaterialUpper = TEXT(R"json({
        "schema":"mh.material",
        "schema_version":1,
        "uid":"11111111-1111-1111-1111-111111111111",
        "name":"CaseKeys",
        "shader_class":"Mimir.Test",
        "params":{"label":"Upper","nested":{"A":1.25,"a":2.5}},
        "textures":{}
    })json");
    const FString MaterialLower = MaterialUpper.Replace(TEXT("\"Upper\""), TEXT("\"upper\""));

    FString MaterialUpperHash;
    FString MaterialLowerHash;
    FString Error;
    bPassed &= TestTrue(
        TEXT("material A/a vector hashes"),
        MHMaterialSemanticHash(
            Private::PayloadHashUtf8(MaterialUpper), MaterialUpperHash, Error));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("material case-only value vector hashes"),
        MHMaterialSemanticHash(
            Private::PayloadHashUtf8(MaterialLower), MaterialLowerHash, Error));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    bPassed &= TestEqual(
        TEXT("material Python A/a hash"),
        MaterialUpperHash,
        TEXT("xxh3:a99b527fc231994d"));
    bPassed &= TestEqual(
        TEXT("material Python case-only value hash"),
        MaterialLowerHash,
        TEXT("xxh3:86196129aa0af686"));
    bPassed &= TestNotEqual(
        TEXT("material value comparison remains case-sensitive"),
        MaterialUpperHash,
        MaterialLowerHash);

    const FString CompositeUpper = TEXT(R"json({
        "schema":"mh.composite",
        "schema_version":2,
        "uid":"22222222-2222-2222-2222-222222222222",
        "name":"CaseKeys",
        "properties":{"label":"Upper","nested":{"A":1.25,"a":2.5}},
        "nodes":[]
    })json");
    const FString CompositeLower = CompositeUpper.Replace(TEXT("\"Upper\""), TEXT("\"upper\""));

    FString CompositeUpperHash;
    FString CompositeLowerHash;
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("composite A/a vector hashes"),
        MHCompositeSemanticHash(
            Private::PayloadHashUtf8(CompositeUpper), CompositeUpperHash, Error));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("composite case-only value vector hashes"),
        MHCompositeSemanticHash(
            Private::PayloadHashUtf8(CompositeLower), CompositeLowerHash, Error));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    bPassed &= TestEqual(
        TEXT("composite Python A/a hash"),
        CompositeUpperHash,
        TEXT("xxh3:41e8bad952fc52e2"));
    bPassed &= TestEqual(
        TEXT("composite Python case-only value hash"),
        CompositeLowerHash,
        TEXT("xxh3:b29ec82949669b5a"));
    bPassed &= TestNotEqual(
        TEXT("composite value comparison remains case-sensitive"),
        CompositeUpperHash,
        CompositeLowerHash);

    const FString MaterialNfcCollision = MaterialUpper.Replace(
        TEXT("\"A\":1.25,\"a\":2.5"),
        TEXT("\"\\u00e9\":1.25,\"e\\u0301\":2.5"));
    FString IgnoredHash;
    Error.Reset();
    bPassed &= TestFalse(
        TEXT("material NFC-colliding keys fail closed"),
        MHMaterialSemanticHash(
            Private::PayloadHashUtf8(MaterialNfcCollision), IgnoredHash, Error));
    bPassed &= TestTrue(TEXT("material collision is diagnosed"), Error.Contains(TEXT("collide")));

    const FString CompositeNfcCollision = CompositeUpper.Replace(
        TEXT("\"A\":1.25,\"a\":2.5"),
        TEXT("\"\\u00e9\":1.25,\"e\\u0301\":2.5"));
    Error.Reset();
    bPassed &= TestFalse(
        TEXT("composite NFC-colliding keys fail closed"),
        MHCompositeSemanticHash(
            Private::PayloadHashUtf8(CompositeNfcCollision), IgnoredHash, Error));
    bPassed &= TestTrue(TEXT("composite collision is diagnosed"), Error.Contains(TEXT("collide")));

    return bPassed;
}

} // namespace UE::MimirComposite::Tests
