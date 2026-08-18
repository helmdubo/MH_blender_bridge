#include "Geometry/MHGeometryBackends.h"

#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
constexpr int32 ExpectedSourceVertices = 9;
constexpr int32 ExpectedSourcePolygons = 7;
constexpr int32 ExpectedMeshDescriptionPolygons = 13;
constexpr int32 ExpectedRenderVertices = 9;
constexpr int32 ExpectedRenderTriangles = 13;
constexpr double PositionToleranceCm = 0.1;

const FVector ExpectedLocalCm(37.0, -11.0, 193.0);
const FVector ExpectedWorldCm(223.3146, 219.4280, 242.7456);
const FVector ActorLocationCm(125.0, 250.0, 75.0);
const FQuat ActorRotation(-0.038135, 0.189308, -0.239298, 0.951549);

double FindNearestDistance(const TArray<FVector3f>& Positions, const FVector& Expected, FVector& OutNearest)
{
    double BestDistance = TNumericLimits<double>::Max();
    for (const FVector3f& Position : Positions)
    {
        const FVector Candidate(Position);
        const double Distance = FVector::Distance(Candidate, Expected);
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            OutNearest = Candidate;
        }
    }
    return BestDistance;
}

bool VerifyBackend(
    FAutomationTestBase& Test,
    const TCHAR* Label,
    const FMHGeometryProbeResult& Result)
{
    bool bSuccess = true;
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s source vertex count"), Label),
        Result.SourceVertexCount,
        ExpectedSourceVertices);
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s source polygon count"), Label),
        Result.SourcePolygonCount,
        ExpectedSourcePolygons);
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s pre-build MeshDescription vertex count"), Label),
        Result.MeshDescriptionVertexCount,
        ExpectedSourceVertices);
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s pre-build MeshDescription polygon count"), Label),
        Result.MeshDescriptionPolygonCount,
        ExpectedMeshDescriptionPolygons);
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s post-build render vertex count"), Label),
        Result.RenderVertexCount,
        ExpectedRenderVertices);
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s post-build render triangle count"), Label),
        Result.RenderTriangleCount,
        ExpectedRenderTriangles);

    FVector NearestLocal = FVector::ZeroVector;
    const double LocalDistance = FindNearestDistance(Result.RenderPositions, ExpectedLocalCm, NearestLocal);
    bSuccess &= Test.TestTrue(
        FString::Printf(
            TEXT("%s local control vertex: expected %s, nearest %s, delta %.6f cm"),
            Label,
            *ExpectedLocalCm.ToString(),
            *NearestLocal.ToString(),
            LocalDistance),
        LocalDistance <= PositionToleranceCm);

    const FTransform Placement(ActorRotation.GetNormalized(), ActorLocationCm, FVector::OneVector);
    const FVector WorldPosition = Placement.TransformPosition(NearestLocal);
    const double WorldDistance = FVector::Distance(WorldPosition, ExpectedWorldCm);
    bSuccess &= Test.TestTrue(
        FString::Printf(
            TEXT("%s world control vertex: expected %s, actual %s, delta %.6f cm"),
            Label,
            *ExpectedWorldCm.ToString(),
            *WorldPosition.ToString(),
            WorldDistance),
        WorldDistance <= PositionToleranceCm);

    return bSuccess;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHR1AxisProbeBackendsTest,
    "Mimir.C0.R1.AxisProbeBackends",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHR1AxisProbeBackendsTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!UE::MimirComposite::Tests::ResolveGoldenRoot(*this, GoldenRoot))
    {
        return false;
    }

    const FString AxisProbe = FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"));
    if (!FPaths::FileExists(AxisProbe))
    {
        AddError(FString::Printf(TEXT("Axis probe fixture not found: %s"), *AxisProbe));
        return false;
    }

    FMHGeometryProbeResult DirectResult;
    FString Error;
    if (!FMHFbxBackend::ImportAxisProbe(AxisProbe, DirectResult, Error))
    {
        AddError(FString::Printf(TEXT("FMHFbxBackend failed: %s"), *Error));
        return false;
    }

    FMHGeometryProbeResult LegacyResult;
    if (!FMHLegacyFbxBackend::ImportAxisProbe(AxisProbe, LegacyResult, Error))
    {
        AddError(FString::Printf(TEXT("FMHLegacyFbxBackend failed: %s"), *Error));
        return false;
    }

    bool bSuccess = true;
    bSuccess &= VerifyBackend(*this, TEXT("FMHFbxBackend"), DirectResult);
    bSuccess &= VerifyBackend(*this, TEXT("FMHLegacyFbxBackend"), LegacyResult);
    bSuccess &= TestEqual(
        TEXT("backend post-build render vertex parity"),
        DirectResult.RenderVertexCount,
        LegacyResult.RenderVertexCount);
    bSuccess &= TestEqual(
        TEXT("backend post-build render triangle parity"),
        DirectResult.RenderTriangleCount,
        LegacyResult.RenderTriangleCount);

    const FRotator Rotator = ActorRotation.GetNormalized().Rotator();
    bSuccess &= TestEqual(TEXT("actor pitch"), Rotator.Pitch, -20.0, 0.001);
    bSuccess &= TestEqual(TEXT("actor yaw"), Rotator.Yaw, -30.0, 0.001);
    bSuccess &= TestEqual(TEXT("actor roll"), Rotator.Roll, 10.0, 0.001);
    return bSuccess;
}
