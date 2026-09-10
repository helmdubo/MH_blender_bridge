#include "Geometry/MHFbxSceneTranslator.h"
#include "Geometry/MHSceneIR.h"
#include "StaticMesh/MHStaticMeshImporter.h"

#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Math/Color.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RHIGlobals.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace UE::MimirComposite::Tests
{
namespace
{

FbxMesh* CreateTriangle(FbxScene& Scene, const char* Name, const double XOffset)
{
    FbxMesh* Mesh = FbxMesh::Create(&Scene, Name);
    Mesh->InitControlPoints(3);
    Mesh->GetControlPoints()[0] = FbxVector4(XOffset, 0.0, 0.0);
    Mesh->GetControlPoints()[1] = FbxVector4(XOffset + 10.0, 0.0, 0.0);
    Mesh->GetControlPoints()[2] = FbxVector4(XOffset, 10.0, 0.0);
    Mesh->BeginPolygon();
    Mesh->AddPolygon(0);
    Mesh->AddPolygon(1);
    Mesh->AddPolygon(2);
    Mesh->EndPolygon();

    FbxGeometryElementNormal* Normals = Mesh->CreateElementNormal();
    Normals->SetMappingMode(FbxGeometryElement::eByControlPoint);
    Normals->SetReferenceMode(FbxGeometryElement::eDirect);
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Normals->GetDirectArray().Add(FbxVector4(0.0, 0.0, 1.0));
    }
    return Mesh;
}

void AddAuthoredAttributes(FbxMesh& Mesh)
{
    FbxGeometryElementUV* UV0 = Mesh.CreateElementUV("UVMap");
    UV0->SetMappingMode(FbxGeometryElement::eByPolygonVertex);
    UV0->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    UV0->GetDirectArray().Add(FbxVector2(0.10, 0.20));
    UV0->GetDirectArray().Add(FbxVector2(0.30, 0.40));
    UV0->GetDirectArray().Add(FbxVector2(0.50, 0.60));
    UV0->GetIndexArray().Add(0);
    UV0->GetIndexArray().Add(1);
    UV0->GetIndexArray().Add(2);

    // Dagor pivot-wind address from bush_beech_small_b: after FBX->UE V flip,
    // (0.015625, 0.9609375) becomes (0.015625, 0.0390625), the center of an
    // addressed texel in the 32x64 pivot atlas.
    FbxGeometryElementUV* UV1 = Mesh.CreateElementUV("UVMap.001");
    UV1->SetMappingMode(FbxGeometryElement::eByControlPoint);
    UV1->SetReferenceMode(FbxGeometryElement::eDirect);
    UV1->GetDirectArray().Add(FbxVector2(0.015625, 0.9609375));
    UV1->GetDirectArray().Add(FbxVector2(0.046875, 0.9609375));
    UV1->GetDirectArray().Add(FbxVector2(0.015625, 0.9296875));

    FbxGeometryElementUV* UV2 = Mesh.CreateElementUV("UVMap.002");
    UV2->SetMappingMode(FbxGeometryElement::eByPolygon);
    UV2->SetReferenceMode(FbxGeometryElement::eDirect);
    UV2->GetDirectArray().Add(FbxVector2(0.625, 0.25));

    FbxGeometryElementUV* UV3 = Mesh.CreateElementUV("UVMap.003");
    UV3->SetMappingMode(FbxGeometryElement::eAllSame);
    UV3->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    UV3->GetDirectArray().Add(FbxVector2(0.875, 0.125));
    UV3->GetIndexArray().Add(0);

    FbxGeometryElementVertexColor* Colors = Mesh.CreateElementVertexColor();
    Colors->SetName("wind_color");
    Colors->SetMappingMode(FbxGeometryElement::eByControlPoint);
    Colors->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    Colors->GetDirectArray().Add(FbxColor(1.0, 0.0, 0.0, 1.0));
    Colors->GetDirectArray().Add(FbxColor(0.0, 1.0, 0.0, 1.0));
    Colors->GetDirectArray().Add(FbxColor(0.5, 0.25, 0.75, 0.5));
    Colors->GetIndexArray().Add(2);
    Colors->GetIndexArray().Add(0);
    Colors->GetIndexArray().Add(1);
    if (Mesh.GetLayerCount() == 0)
    {
        Mesh.CreateLayer();
    }
    // FBX geometry elements must also be bound to a layer to be serialized as
    // authored data. Without this binding the SDK writes an empty eNone
    // placeholder and drops the populated element on export.
    Mesh.GetLayer(0)->SetVertexColors(Colors);
}

void AddFallbackUV0(FbxMesh& Mesh)
{
    FbxGeometryElementUV* UV0 = Mesh.CreateElementUV("UVMap");
    UV0->SetMappingMode(FbxGeometryElement::eAllSame);
    UV0->SetReferenceMode(FbxGeometryElement::eDirect);
    UV0->GetDirectArray().Add(FbxVector2(0.25, 0.75));
}

bool ExportAttributeFixture(const FString& Path, FString& OutError)
{
    OutError.Reset();
    FbxManager* Manager = FbxManager::Create();
    if (Manager == nullptr)
    {
        OutError = TEXT("FbxManager::Create failed");
        return false;
    }
    FbxIOSettings* IOSettings = FbxIOSettings::Create(Manager, IOSROOT);
    Manager->SetIOSettings(IOSettings);
    FbxScene* Scene = FbxScene::Create(Manager, "AttributeTransport");
    const FbxAxisSystem::EFrontVector CanonicalFront =
        static_cast<FbxAxisSystem::EFrontVector>(-FbxAxisSystem::eParityEven);
    const FbxAxisSystem CanonicalAxes(
        FbxAxisSystem::eZAxis,
        CanonicalFront,
        FbxAxisSystem::eRightHanded);
    Scene->GetGlobalSettings().SetAxisSystem(CanonicalAxes);
    Scene->GetGlobalSettings().SetOriginalUpAxis(CanonicalAxes);
    Scene->GetGlobalSettings().SetSystemUnit(FbxSystemUnit::cm);

    FbxMesh* AuthoredMesh = CreateTriangle(*Scene, "authored_geometry", 0.0);
    AddAuthoredAttributes(*AuthoredMesh);
    FbxNode* AuthoredNode = FbxNode::Create(Scene, "authored_crown");
    AuthoredNode->SetNodeAttribute(AuthoredMesh);
    Scene->GetRootNode()->AddChild(AuthoredNode);

    // A second render node authors only UV0 and no color. The aggregate build
    // must retain four channels, zero-fill only its missing UV1..3, and use
    // white for its absent vertex colors.
    FbxMesh* SparseMesh = CreateTriangle(*Scene, "sparse_geometry", 100.0);
    AddFallbackUV0(*SparseMesh);
    FbxNode* SparseNode = FbxNode::Create(Scene, "sparse_crown");
    SparseNode->SetNodeAttribute(SparseMesh);
    Scene->GetRootNode()->AddChild(SparseNode);

    // Match Blender's canonical axis_forward='X' root-node conversion expected
    // by the production translator. Attribute values are unaffected.
    for (int32 Index = 0; Index < Scene->GetRootNode()->GetChildCount(); ++Index)
    {
        Scene->GetRootNode()->GetChild(Index)->LclRotation.Set(FbxDouble3(0.0, 0.0, -90.0));
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    FbxExporter* Exporter = FbxExporter::Create(Manager, "AttributeExporter");
    const int32 WriterId = Manager->GetIOPluginRegistry()->GetNativeWriterFormat();
    if (!Exporter->Initialize(TCHAR_TO_UTF8(*Path), WriterId, IOSettings) ||
        !Exporter->Export(Scene))
    {
        OutError = UTF8_TO_TCHAR(Exporter->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }
    Manager->Destroy();
    return true;
}

const FMHSceneIRNode* FindNode(const FMHSceneIR& Scene, const TCHAR* Name)
{
    return Scene.Nodes.FindByPredicate(
        [Name](const FMHSceneIRNode& Node) { return Node.Name == Name; });
}

bool Near(const FVector2f& Actual, const FVector2f& Expected)
{
    return Actual.Equals(Expected, 1.0e-6f);
}

bool Near(const FVector4f& Actual, const FVector4f& Expected)
{
    return Actual.Equals(Expected, 1.0e-6f);
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshAttributeTransportTest,
    "Mimir.V4.StaticMesh.Attributes.AuthoredUVsAndVertexColors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshAttributeTransportTest::RunTest(const FString& Parameters)
{
    const FString FixturePath = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests"),
        FString::Printf(
            TEXT("attribute_transport_%s.fbx"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*FixturePath, false, true, true);
    };

    FString Error;
    if (!TestTrue(TEXT("synthetic multi-attribute FBX exports"), ExportAttributeFixture(FixturePath, Error)))
    {
        AddError(Error);
        return false;
    }
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("synthetic FBX reads"), FFileHelper::LoadFileToArray(Bytes, *FixturePath)))
    {
        return false;
    }

    FMHSceneIR Scene;
    FMHFbxSceneTranslator Translator;
    if (!TestTrue(
            TEXT("synthetic FBX translates"),
            Translator.Translate(TEXT("attribute_transport"), Bytes, Scene, Error)))
    {
        AddError(Error);
        return false;
    }
    if (!TestTrue(TEXT("translated scene classifies"), MHClassifySceneIR(Scene, Error)))
    {
        AddError(Error);
        return false;
    }

    bool bPassed = true;
    const FMHSceneIRNode* Authored = FindNode(Scene, TEXT("authored_crown"));
    const FMHSceneIRNode* Sparse = FindNode(Scene, TEXT("sparse_crown"));
    bPassed &= TestNotNull(TEXT("authored render node exists"), Authored);
    bPassed &= TestNotNull(TEXT("sparse render node exists"), Sparse);
    if (Authored == nullptr || Sparse == nullptr || !Authored->Geometry.IsSet() || !Sparse->Geometry.IsSet())
    {
        return false;
    }

    const FMHSceneTriangle& Triangle = Authored->Geometry->Triangles[0];
    bPassed &= TestEqual(TEXT("all four authored UV sets reach IR"), Triangle.AdditionalCornerUVs.Num() + 1, 4);
    bPassed &= TestTrue(TEXT("UV0 is preserved and V-flipped"), Near(Triangle.CornerUV0[0], FVector2f(0.10f, 0.80f)));
    bPassed &= TestTrue(
        TEXT("UV1 pivot texel center is preserved and V-flipped"),
        Near(Triangle.AdditionalCornerUVs[0][0], FVector2f(0.015625f, 0.0390625f)));
    bPassed &= TestTrue(
        TEXT("UV1 addresses the 32x64 atlas texel center"),
        Near(Triangle.AdditionalCornerUVs[0][0] * FVector2f(32.0f, 64.0f), FVector2f(0.5f, 2.5f)));
    bPassed &= TestTrue(TEXT("UV2 by-polygon mapping survives"), Near(Triangle.AdditionalCornerUVs[1][0], FVector2f(0.625f, 0.75f)));
    bPassed &= TestTrue(TEXT("UV3 all-same mapping survives"), Near(Triangle.AdditionalCornerUVs[2][0], FVector2f(0.875f, 0.875f)));
    bPassed &= TestTrue(TEXT("first vertex-color set is explicit"), Authored->Geometry->bHasVertexColors);
    bPassed &= TestTrue(
        TEXT("vertex-color index-to-direct mapping retains raw FBX midtones in IR"),
        Near(Triangle.CornerColors[0], FVector4f(0.5f, 0.25f, 0.75f, 0.5f)));
    bPassed &= TestEqual(
        TEXT("sparse node retains only its authored UV0"),
        Sparse->Geometry->Triangles[0].AdditionalCornerUVs.Num(),
        0);
    bPassed &= TestFalse(TEXT("sparse node has no authored color set"), Sparse->Geometry->bHasVertexColors);

    UStaticMesh* StaticMesh = NewObject<UStaticMesh>(
        GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("MH_AttributeTransport")),
        RF_Transient | RF_Transactional);
    FMHStaticMeshBuildPlan Plan;
    Plan.Scene = &Scene;
    if (!TestTrue(TEXT("translated attributes build into a static mesh"), FMHStaticMeshBuilder::Rebuild(*StaticMesh, Plan, Error)))
    {
        AddError(Error);
        return false;
    }
    bPassed &= TestFalse(
        TEXT("build does not generate over authored UV channels"),
        StaticMesh->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs);

    const FMeshDescription* Description = StaticMesh->GetMeshDescription(0);
    bPassed &= TestNotNull(TEXT("built LOD0 MeshDescription exists"), Description);
    if (Description != nullptr)
    {
        const FStaticMeshConstAttributes Attributes(*Description);
        const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
        const TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
        bPassed &= TestEqual(TEXT("MeshDescription retains four UV channels"), UVs.GetNumChannels(), 4);
        bool bFoundPivot = false;
        bool bFoundSparseZeroFill = false;
        bool bFoundAuthoredColor = false;
        bool bFoundSparseWhite = false;
        const FVector4f ExpectedImportedMidtone(FLinearColor(FColor(127, 63, 191, 127)));
        for (const FVertexInstanceID Instance : Description->VertexInstances().GetElementIDs())
        {
            bFoundPivot |= Near(UVs.Get(Instance, 1), FVector2f(0.015625f, 0.0390625f));
            bFoundSparseZeroFill |= Near(UVs.Get(Instance, 0), FVector2f(0.25f, 0.25f)) &&
                Near(UVs.Get(Instance, 1), FVector2f::ZeroVector) &&
                Near(UVs.Get(Instance, 2), FVector2f::ZeroVector) &&
                Near(UVs.Get(Instance, 3), FVector2f::ZeroVector);
            bFoundAuthoredColor |= Near(Colors[Instance], ExpectedImportedMidtone);
            bFoundSparseWhite |= Near(UVs.Get(Instance, 0), FVector2f(0.25f, 0.25f)) &&
                Near(Colors[Instance], FVector4f(1.0f, 1.0f, 1.0f, 1.0f));
        }
        bPassed &= TestTrue(TEXT("built UV1 still contains pivot address"), bFoundPivot);
        bPassed &= TestTrue(TEXT("missing higher channels are zero-filled per node"), bFoundSparseZeroFill);
        bPassed &= TestTrue(TEXT("MeshDescription stores native FBX sRGB-byte round trip"), bFoundAuthoredColor);
        bPassed &= TestTrue(TEXT("node without colors receives white"), bFoundSparseWhite);
    }

    const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
    bPassed &= TestNotNull(TEXT("actual static-mesh render data exists"), RenderData);
    if (RenderData != nullptr && RenderData->LODResources.IsValidIndex(0))
    {
        const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
        bPassed &= TestEqual(
            TEXT("render vertex buffer retains four UV channels"),
            static_cast<int32>(LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()),
            4);
        bPassed &= TestTrue(
            TEXT("render color buffer is present"),
            (GUsingNullRHI || LOD.VertexBuffers.ColorVertexBuffer.IsInitialized()) &&
                LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() > 0);
        bool bFoundMidtoneBytes = false;
        for (uint32 VertexIndex = 0;
             VertexIndex < LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices();
             ++VertexIndex)
        {
            bFoundMidtoneBytes |=
                LOD.VertexBuffers.ColorVertexBuffer.VertexColor(VertexIndex) == FColor(127, 63, 191, 127);
        }
        bPassed &= TestTrue(
            TEXT("render color buffer preserves authored midtone RGBA bytes"),
            bFoundMidtoneBytes);
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
