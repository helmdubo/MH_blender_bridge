#include "Geometry/MHFbxSceneTranslator.h"

#include "Geometry/MHSceneIR.h"
#include "HAL/FileManager.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace UE::MimirComposite::Tests
{
namespace
{

FVector3f TransportTestReflect(const FbxVector4& Value)
{
    return FVector3f(
        static_cast<float>(Value[0]),
        static_cast<float>(-Value[1]),
        static_cast<float>(Value[2]));
}

const FMHSceneIRNode* TransportTestFindNode(const FMHSceneIR& Scene, const TCHAR* Name)
{
    return Scene.Nodes.FindByPredicate([Name](const FMHSceneIRNode& Node)
    {
        return Node.Name == Name;
    });
}

struct FMHTransportTestFixture
{
    FbxManager* Manager = nullptr;
    FbxScene* Scene = nullptr;
    FString Directory;

    FMHTransportTestFixture()
    {
        Directory = FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests"),
            TEXT("FbxTransport_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        IFileManager::Get().MakeDirectory(*Directory, true);
        Manager = FbxManager::Create();
        if (Manager != nullptr)
        {
            Manager->SetIOSettings(FbxIOSettings::Create(Manager, IOSROOT));
            Scene = FbxScene::Create(Manager, "MHTransportParity");
            Scene->GetGlobalSettings().SetAxisSystem(FbxAxisSystem::MayaZUp);
            Scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::cm);
        }
    }

    ~FMHTransportTestFixture()
    {
        if (Manager != nullptr)
        {
            Manager->Destroy();
        }
        IFileManager::Get().DeleteDirectory(*Directory, false, true);
    }

    FbxNode* AddNull(FbxNode& Parent, const char* Name)
    {
        FbxNode* Node = FbxNode::Create(Scene, Name);
        Node->SetNodeAttribute(FbxNull::Create(Scene, Name));
        Parent.AddChild(Node);
        return Node;
    }

    FbxNode* AddMesh(FbxNode& Parent, const char* Name, const bool bHull)
    {
        FbxNode* Node = FbxNode::Create(Scene, Name);
        FbxMesh* Mesh = FbxMesh::Create(Scene, Name);
        Mesh->InitControlPoints(bHull ? 4 : 3);
        Mesh->GetControlPoints()[0] = FbxVector4(3.0, 5.0, 7.0);
        Mesh->GetControlPoints()[1] = FbxVector4(33.0, 5.0, 7.0);
        Mesh->GetControlPoints()[2] = FbxVector4(3.0, 45.0, 7.0);
        if (bHull)
        {
            Mesh->GetControlPoints()[3] = FbxVector4(3.0, 5.0, 57.0);
        }
        constexpr int32 Corners[4][3] = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
        for (int32 Polygon = 0; Polygon < (bHull ? 4 : 1); ++Polygon)
        {
            Mesh->BeginPolygon();
            for (const int32 Corner : Corners[Polygon])
            {
                Mesh->AddPolygon(Corner);
            }
            Mesh->EndPolygon();
        }
        if (!bHull)
        {
            FbxGeometryElementNormal* Normals = Mesh->CreateElementNormal();
            Normals->SetMappingMode(FbxGeometryElement::eByControlPoint);
            Normals->SetReferenceMode(FbxGeometryElement::eDirect);
            for (int32 Point = 0; Point < 3; ++Point)
            {
                Normals->GetDirectArray().Add(FbxVector4(0.0, 0.0, 1.0, 0.0));
            }
        }
        Node->SetNodeAttribute(Mesh);
        Parent.AddChild(Node);
        return Node;
    }

    void EncodeCanonicalTransport()
    {
        const FbxAxisSystem TransportAxis(
            FbxAxisSystem::eZAxis,
            static_cast<FbxAxisSystem::EFrontVector>(-FbxAxisSystem::eParityEven),
            FbxAxisSystem::eRightHanded);
        TransportAxis.ConvertScene(Scene);
        Scene->GetAnimationEvaluator()->Reset();
    }

    bool Export(TArray<uint8>& OutBytes, FString& OutError)
    {
        const FString Path = FPaths::Combine(Directory, TEXT("transport.mesh.fbx"));
        FbxExporter* Exporter = FbxExporter::Create(Manager, "TransportParityExporter");
        if (!Exporter->Initialize(
                TCHAR_TO_UTF8(*Path), Manager->GetIOPluginRegistry()->GetNativeWriterFormat(),
                Manager->GetIOSettings()) || !Exporter->Export(Scene))
        {
            OutError = UTF8_TO_TCHAR(Exporter->GetStatus().GetErrorString());
            return false;
        }
        Exporter->Destroy();
        return FFileHelper::LoadFileToArray(OutBytes, *Path);
    }
};

FbxAMatrix TransportTestGlobalGeometry(FbxNode& Node)
{
    FbxAMatrix Geometry;
    Geometry.SetT(Node.GetGeometricTranslation(FbxNode::eSourcePivot));
    Geometry.SetR(Node.GetGeometricRotation(FbxNode::eSourcePivot));
    Geometry.SetS(Node.GetGeometricScaling(FbxNode::eSourcePivot));
    return Node.EvaluateGlobalTransform() * Geometry;
}

TArray<FVector3f> TransportTestAuthoredPositions(FbxNode& Node)
{
    TArray<FVector3f> Result;
    const FbxAMatrix Global = TransportTestGlobalGeometry(Node);
    for (int32 Point = 0; Point < Node.GetMesh()->GetControlPointsCount(); ++Point)
    {
        Result.Add(TransportTestReflect(Global.MultT(Node.GetMesh()->GetControlPointAt(Point))));
    }
    return Result;
}

bool TransportTestVerifyPositions(
    FAutomationTestBase& Test, const FMHSceneIRNode& Node, const TArray<FVector3f>& Expected)
{
    if (!Test.TestTrue(TEXT("translated node has geometry"), Node.Geometry.IsSet()))
    {
        return false;
    }
    const TArray<FVector3f>& Actual = Node.Geometry.GetValue().Positions;
    if (!Test.TestEqual(TEXT("transport preserves control-point count"), Actual.Num(), Expected.Num()))
    {
        return false;
    }
    bool bPassed = true;
    for (int32 Point = 0; Point < Actual.Num(); ++Point)
    {
        bPassed &= Test.TestTrue(
            FString::Printf(TEXT("%s point %d preserves authored world transform"), *Node.Name, Point),
            FVector3f::Distance(Actual[Point], Expected[Point]) < 0.001f);
    }
    return bPassed;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHFbxTransportFrozenAxisProbeTest,
    "Mimir.Fbx.Transport.FrozenAxisProbeProductionTranslator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHFbxTransportFrozenAxisProbeTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot))
    {
        return false;
    }
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("load the unchanged R1 FBX"), FFileHelper::LoadFileToArray(
            Bytes, *FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx")))))
    {
        return false;
    }
    FMHFbxSceneTranslator Translator;
    FMHSceneIR Scene;
    FString Error;
    if (!TestTrue(TEXT("production translator accepts frozen R1 transport"),
            Translator.Translate(TEXT("axis_probe"), Bytes, Scene, Error)))
    {
        AddError(Error);
        return false;
    }
    const FMHSceneIRNode* Render = Scene.Nodes.FindByPredicate([](const FMHSceneIRNode& Node)
    {
        return Node.Kind == EMHSceneNodeKind::Render;
    });
    if (!TestNotNull(TEXT("R1 render node"), Render) || !Render->Geometry.IsSet())
    {
        return false;
    }
    const TArray<FVector3f>& Positions = Render->Geometry.GetValue().Positions;
    bool bPassed = TestEqual(TEXT("R1 control point count"), Positions.Num(), 9);
    bPassed &= TestTrue(TEXT("production translator obeys ratified R1 (37,-11,193) cm"),
        Positions.ContainsByPredicate([](const FVector3f& Position)
        {
            return FVector3f::Distance(Position, FVector3f(37.0f, -11.0f, 193.0f)) < 0.001f;
        }));
    bPassed &= TestFalse(TEXT("transport-only quarter turn is not baked into asset geometry"),
        Positions.ContainsByPredicate([](const FVector3f& Position)
        {
            return FVector3f::Distance(Position, FVector3f(11.0f, 37.0f, 193.0f)) < 0.001f;
        }));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHFbxTransportHierarchyParityTest,
    "Mimir.Fbx.Transport.HierarchyNormalsSocketsCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHFbxTransportHierarchyParityTest::RunTest(const FString& Parameters)
{
    FMHTransportTestFixture Fixture;
    if (!TestNotNull(TEXT("FBX fixture scene"), Fixture.Scene))
    {
        return false;
    }
    FbxNode* Group = Fixture.AddNull(*Fixture.Scene->GetRootNode(), "authored_group");
    Group->LclTranslation.Set(FbxDouble3(100.0, 50.0, 20.0));
    Group->LclRotation.Set(FbxDouble3(0.0, 0.0, 35.0));
    FbxNode* Render = Fixture.AddMesh(*Group, "body_lod00", false);
    Render->LclTranslation.Set(FbxDouble3(10.0, 20.0, 30.0));
    Render->LclRotation.Set(FbxDouble3(25.0, 0.0, 0.0));
    Render->LclScaling.Set(FbxDouble3(2.0, 3.0, 4.0));
    Render->SetGeometricTranslation(FbxNode::eSourcePivot, FbxVector4(7.0, 11.0, 13.0));
    Render->SetGeometricRotation(FbxNode::eSourcePivot, FbxVector4(0.0, 10.0, 0.0));
    FbxNode* Collision = Fixture.AddMesh(*Group, "UCX_body", true);
    Collision->LclTranslation.Set(FbxDouble3(-15.0, 25.0, 45.0));
    Collision->LclRotation.Set(FbxDouble3(0.0, 0.0, -20.0));
    FbxNode* Socket = Fixture.AddNull(*Group, "SOCKET_mount");
    Socket->LclTranslation.Set(FbxDouble3(13.0, 17.0, 19.0));
    Socket->LclRotation.Set(FbxDouble3(20.0, 15.0, 10.0));

    // Capture the authored scene before transport, including a rotated parent,
    // a nonuniform mesh/geometry transform and the socket's local orientation.
    // The separate frozen R1 test supplies an independent, non-SDK oracle.
    const TArray<FVector3f> ExpectedRender = TransportTestAuthoredPositions(*Render);
    const TArray<FVector3f> ExpectedCollision = TransportTestAuthoredPositions(*Collision);
    const FbxAMatrix NormalMatrix = TransportTestGlobalGeometry(*Render).Inverse().Transpose();
    const FVector3f ExpectedNormal = TransportTestReflect(
        NormalMatrix.MultT(FbxVector4(0.0, 0.0, 1.0, 0.0))).GetSafeNormal();
    const FbxAMatrix SocketMatrix = Socket->EvaluateGlobalTransform();
    const FVector3f ExpectedSocketOrigin = TransportTestReflect(
        SocketMatrix.MultT(FbxVector4(0.0, 0.0, 0.0, 1.0)));
    const FVector3f ExpectedSocketX = TransportTestReflect(
        SocketMatrix.MultT(FbxVector4(1.0, 0.0, 0.0, 1.0)));
    const FVector3f ExpectedSocketY = TransportTestReflect(
        SocketMatrix.MultT(FbxVector4(0.0, -1.0, 0.0, 1.0)));

    Fixture.EncodeCanonicalTransport();
    TArray<uint8> Bytes;
    FString Error;
    if (!TestTrue(TEXT("export hierarchy transport fixture"), Fixture.Export(Bytes, Error)))
    {
        AddError(Error);
        return false;
    }
    FMHFbxSceneTranslator Translator;
    FMHSceneIR Scene;
    if (!TestTrue(TEXT("translate complete hierarchy"),
            Translator.Translate(TEXT("transport"), Bytes, Scene, Error)))
    {
        AddError(Error);
        return false;
    }
    const FMHSceneIRNode* ActualRender = TransportTestFindNode(Scene, TEXT("body_lod00"));
    const FMHSceneIRNode* ActualCollision = TransportTestFindNode(Scene, TEXT("UCX_body"));
    const FMHSceneIRNode* ActualSocket = TransportTestFindNode(Scene, TEXT("SOCKET_mount"));
    if (!TestNotNull(TEXT("render child"), ActualRender) ||
        !TestNotNull(TEXT("collision child"), ActualCollision) ||
        !TestNotNull(TEXT("socket child"), ActualSocket))
    {
        return false;
    }
    if (!TestTrue(TEXT("translated render has normal-bearing geometry"),
            ActualRender->Geometry.IsSet() && !ActualRender->Geometry.GetValue().Triangles.IsEmpty()))
    {
        return false;
    }
    bool bPassed = TransportTestVerifyPositions(*this, *ActualRender, ExpectedRender);
    bPassed &= TransportTestVerifyPositions(*this, *ActualCollision, ExpectedCollision);
    bPassed &= TestEqual(TEXT("group hierarchy survives normalization"), Scene.Nodes.Num(), 4);
    bPassed &= TestEqual(TEXT("render is still parented to authored group"), ActualRender->ParentIndex, 0);
    bPassed &= TestEqual(TEXT("collision is still parented to authored group"), ActualCollision->ParentIndex, 0);
    bPassed &= TestEqual(TEXT("socket is still parented to authored group"), ActualSocket->ParentIndex, 0);
    bPassed &= TestTrue(TEXT("render authored split normal follows the complete transform"),
        FVector3f::Distance(
            ActualRender->Geometry.GetValue().Triangles[0].CornerNormals[0], ExpectedNormal) < 0.0001f);
    bPassed &= TestTrue(TEXT("socket origin shares render/collision conversion"),
        FVector::Distance(ActualSocket->GlobalTransform.GetLocation(), FVector(ExpectedSocketOrigin)) < 0.001);
    bPassed &= TestTrue(TEXT("socket authored X orientation survives transport"),
        FVector::Distance(ActualSocket->GlobalTransform.TransformPosition(FVector::XAxisVector),
            FVector(ExpectedSocketX)) < 0.001);
    bPassed &= TestTrue(TEXT("socket authored Y orientation survives handedness seam"),
        FVector::Distance(ActualSocket->GlobalTransform.TransformPosition(FVector::YAxisVector),
            FVector(ExpectedSocketY)) < 0.001);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHFbxTransportAdmissionTest,
    "Mimir.Fbx.Transport.RejectsNoncanonicalAxisAndUnits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHFbxTransportAdmissionTest::RunTest(const FString& Parameters)
{
    bool bPassed = true;
    for (const bool bWrongUnits : {false, true})
    {
        FMHTransportTestFixture Fixture;
        if (!TestNotNull(TEXT("FBX rejection fixture scene"), Fixture.Scene))
        {
            return false;
        }
        Fixture.AddMesh(*Fixture.Scene->GetRootNode(), "body", false);
        if (bWrongUnits)
        {
            Fixture.EncodeCanonicalTransport();
            Fixture.Scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::m);
        }
        TArray<uint8> Bytes;
        FString Error;
        if (!TestTrue(TEXT("export invalid metadata fixture"), Fixture.Export(Bytes, Error)))
        {
            AddError(Error);
            return false;
        }
        FMHFbxSceneTranslator Translator;
        FMHSceneIR Scene;
        bPassed &= TestFalse(TEXT("normalization does not broaden admitted transport"),
            Translator.Translate(TEXT("transport"), Bytes, Scene, Error));
        bPassed &= TestTrue(TEXT("noncanonical metadata has transport diagnostic"),
            Error.StartsWith(TEXT("MH_E_FBX_TRANSPORT_FAILED:")));
        bPassed &= TestTrue(TEXT("failure exposes no partially translated hierarchy"), Scene.Nodes.IsEmpty());
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
