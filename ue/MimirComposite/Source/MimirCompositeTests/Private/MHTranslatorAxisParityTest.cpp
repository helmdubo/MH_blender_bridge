#include "Geometry/MHFbxSceneTranslator.h"
#include "Geometry/MHSceneIR.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHTranslatorAxisParityTest,
    "Mimir.C0.R1.TranslatorAxisParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHTranslatorAxisParityTest::RunTest(const FString& Parameters)
{
    // Field defect 2026-08-31: the production FMHFbxSceneTranslator baked the
    // node transforms raw, so the exporter's canonical axis conversion
    // (RotZ(-90) authored by axis_forward='X') leaked into every vertex and
    // every mesh spawned rotated/mirrored against its composite node TRS.
    // The R1 contract fixes the truth: for the committed axis_probe fixture
    // the control vertex authored at Blender (0.37, 0.11, 1.93) m must land
    // at UE (37, -11, 193) cm - the same expectation MHAxisProbeTest pins for
    // the diagnostic backends. This test pins the PRODUCTION translator.
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    const FString AxisProbe = FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"));
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("axis probe fixture reads"), FFileHelper::LoadFileToArray(Bytes, *AxisProbe)))
    {
        return false;
    }
    FMHFbxSceneTranslator Translator;
    FMHSceneIR Scene;
    FString Error;
    if (!TestTrue(TEXT("axis probe translates"), Translator.Translate(TEXT("axis_probe"), Bytes, Scene, Error)))
    {
        AddError(Error);
        return false;
    }
    const FVector3f ExpectedLocalCm(37.0f, -11.0f, 193.0f);
    double BestDistance = TNumericLimits<double>::Max();
    FVector3f Nearest = FVector3f::ZeroVector;
    int32 GeometryNodes = 0;
    for (const FMHSceneIRNode& Node : Scene.Nodes)
    {
        if (!Node.Geometry.IsSet()) continue;
        ++GeometryNodes;
        for (const FVector3f& Position : Node.Geometry.GetValue().Positions)
        {
            const double Distance = FVector3f::Distance(Position, ExpectedLocalCm);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                Nearest = Position;
            }
        }
    }
    bool bPassed = TestTrue(TEXT("axis probe has geometry"), GeometryNodes > 0);
    bPassed &= TestTrue(
        *FString::Printf(
            TEXT("production translator control vertex: expected %s, nearest %s, delta %.4f cm"),
            *ExpectedLocalCm.ToString(), *Nearest.ToString(), BestDistance),
        BestDistance <= 0.1);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
