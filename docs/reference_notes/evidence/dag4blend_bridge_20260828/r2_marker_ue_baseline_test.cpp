#include "Composite/MHCompositeProtocol.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<uint8> MarkerTestUtf8(const TCHAR* Text)
{
    const FTCHARToUTF8 Utf8(Text);
    return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeMarkerCodecTest,
    "Mimir.V5.Composite.Marker.CodecRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeMarkerCodecTest::RunTest(const FString& Parameters)
{
    const TCHAR* Documents[] = {
        TEXT(R"({"v":5,"nodes":[{"kind":"marker","resource":"dummy_volumetric_box","name":"Authored marker","transform":{"translation_cm":[100,20,30]}},{"kind":"group","children":[{"kind":"marker","resource":"loot_box","transform":{"translation_cm":[25,0,0]}}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","name":"Authored choice","options":[{"kind":"marker","resource":"loot_box","weight":1},{"kind":"empty","weight":0}]}]})")
    };
    bool bPassed = true;
    for (const TCHAR* Json : Documents)
    {
        FMHCompositeDocument Document;
        FString Error;
        const bool bParsed = MHParseCompositeV5(MarkerTestUtf8(Json), Document, Error);
        bPassed &= TestTrue(TEXT("marker is admitted as an ordinary node or weighted option"), bParsed);
        if (!bParsed)
        {
            AddInfo(TEXT("MARKER_BASELINE_PARSE: ") + Error);
            continue;
        }
        TArray<uint8> Canonical;
        if (!TestTrue(TEXT("marker canonical writer succeeds"), MHWriteCanonicalCompositeV5(Document, Canonical, Error)))
        {
            AddError(Error);
            bPassed = false;
            continue;
        }
        FMHCompositeDocument Reparsed;
        TArray<uint8> Rewritten;
        bPassed &= TestTrue(TEXT("canonical marker parses"), MHParseCompositeV5(Canonical, Reparsed, Error));
        bPassed &= TestTrue(TEXT("canonical marker rewrites"), MHWriteCanonicalCompositeV5(Reparsed, Rewritten, Error));
        bPassed &= TestTrue(TEXT("marker canonical bytes round-trip exactly"), Canonical == Rewritten);
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
