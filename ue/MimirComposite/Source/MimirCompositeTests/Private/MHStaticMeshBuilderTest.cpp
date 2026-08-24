#include "StaticMesh/MHStaticMeshImporter.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Geometry/MHSceneIR.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{
FMHSceneGeometry TriangleGeometry(const float Size, const float Z)
{
    FMHSceneGeometry Geometry;
    Geometry.Positions = {
        FVector3f(0.0f, 0.0f, Z),
        FVector3f(Size, 0.0f, Z),
        FVector3f(0.0f, Size, Z)};
    FMHSceneTriangle& Triangle = Geometry.Triangles.AddDefaulted_GetRef();
    Triangle.PositionIndices = TStaticArray<int32, 3>(0, 1, 2);
    Triangle.CornerNormals = TStaticArray<FVector3f, 3>(
        FVector3f::ZAxisVector,
        FVector3f::ZAxisVector,
        FVector3f::ZAxisVector);
    Triangle.CornerUV0 = TStaticArray<FVector2f, 3>(
        FVector2f(0.0f, 0.0f),
        FVector2f(1.0f, 0.0f),
        FVector2f(0.0f, 1.0f));
    Triangle.MaterialSlotIndex = 0;
    return Geometry;
}

FMHSceneGeometry HullGeometry(const float Size, const FVector3f& Offset)
{
    FMHSceneGeometry Geometry;
    Geometry.Positions = {
        Offset,
        Offset + FVector3f(Size, 0.0f, 0.0f),
        Offset + FVector3f(0.0f, Size, 0.0f),
        Offset + FVector3f(0.0f, 0.0f, Size)};
    return Geometry;
}

FMHSceneIRNode RenderNode(
    const TCHAR* Name,
    const int32 LODLevel,
    const TCHAR* Slot,
    const float Size)
{
    FMHSceneIRNode Node;
    Node.Name = Name;
    Node.Attribute = EMHSceneNodeAttribute::Mesh;
    Node.Kind = EMHSceneNodeKind::Render;
    Node.LODLevel = LODLevel;
    Node.MaterialSlots = {Slot};
    Node.Geometry = TriangleGeometry(Size, static_cast<float>(LODLevel) * 10.0f);
    return Node;
}

FMHSceneIRNode CollisionNode(
    const TCHAR* Name,
    const EMHSceneCollisionMode Mode,
    const float Size,
    const FVector3f& Offset)
{
    FMHSceneIRNode Node;
    Node.Name = Name;
    Node.Attribute = EMHSceneNodeAttribute::Mesh;
    Node.Kind = EMHSceneNodeKind::Collision;
    Node.CollisionMode = Mode;
    Node.Geometry = HullGeometry(Size, Offset);
    return Node;
}

FMHSceneIRNode SocketNode(const TCHAR* TransportName, const TCHAR* DerivedName, const FVector& Location)
{
    FMHSceneIRNode Node;
    Node.Name = TransportName;
    Node.Attribute = EMHSceneNodeAttribute::Null;
    Node.Kind = EMHSceneNodeKind::Socket;
    Node.SocketName = DerivedName;
    Node.GlobalTransform = FTransform(FQuat::Identity, Location, FVector::OneVector);
    return Node;
}

FMHSceneIR FirstScene()
{
    FMHSceneIR Scene;
    Scene.ResourceName = TEXT("vehicle");
    Scene.MaterialNames = {TEXT("body")};
    Scene.LODLevels = {0, 1};
    Scene.bUsesExplicitLODs = true;
    Scene.Nodes = {
        RenderNode(TEXT("body_lod00"), 0, TEXT("body"), 100.0f),
        RenderNode(TEXT("body_lod01"), 1, TEXT("body"), 50.0f),
        CollisionNode(
            TEXT("body_cls_trace"),
            EMHSceneCollisionMode::QueryOnly,
            30.0f,
            FVector3f::ZeroVector),
        SocketNode(TEXT("SOCKET_mount"), TEXT("mount"), FVector(10.0, 20.0, 30.0))};
    return Scene;
}

FMHSceneIR ReplacementScene()
{
    FMHSceneIR Scene;
    Scene.ResourceName = TEXT("vehicle");
    Scene.MaterialNames = {TEXT("trim")};
    Scene.LODLevels = {0};
    Scene.Nodes = {
        RenderNode(TEXT("replacement"), 0, TEXT("trim"), 75.0f),
        CollisionNode(
            TEXT("replacement_cls_phys"),
            EMHSceneCollisionMode::PhysicsOnly,
            40.0f,
            FVector3f(5.0f, 6.0f, 7.0f)),
        SocketNode(TEXT("SOCKET_muzzle"), TEXT("muzzle"), FVector(-5.0, 6.0, 70.0))};
    return Scene;
}

bool VerifyFirstBuild(FAutomationTestBase& Test, UStaticMesh& StaticMesh, UMaterialInstanceConstant* Material)
{
    bool bSuccess = true;
    bSuccess &= Test.TestEqual(TEXT("first build has dense LOD0/1"), StaticMesh.GetNumSourceModels(), 2);
    bSuccess &= Test.TestNotNull(TEXT("LOD0 MeshDescription exists"), StaticMesh.GetMeshDescription(0));
    bSuccess &= Test.TestNotNull(TEXT("LOD1 MeshDescription exists"), StaticMesh.GetMeshDescription(1));
    if (const FMeshDescription* LOD0 = StaticMesh.GetMeshDescription(0))
    {
        bSuccess &= Test.TestEqual(TEXT("LOD0 polygon count"), LOD0->Polygons().Num(), 1);
    }
    if (const FMeshDescription* LOD1 = StaticMesh.GetMeshDescription(1))
    {
        bSuccess &= Test.TestEqual(TEXT("LOD1 polygon count"), LOD1->Polygons().Num(), 1);
    }

    bSuccess &= Test.TestEqual(TEXT("first material count"), StaticMesh.GetStaticMaterials().Num(), 1);
    if (!StaticMesh.GetStaticMaterials().IsEmpty())
    {
        const FStaticMaterial& Slot = StaticMesh.GetStaticMaterials()[0];
        bSuccess &= Test.TestTrue(TEXT("body MI bound"), Slot.MaterialInterface.Get() == Material);
        bSuccess &= Test.TestEqual(TEXT("body slot name"), Slot.MaterialSlotName, FName(TEXT("body")));
    }

    bSuccess &= Test.TestEqual(TEXT("first socket count"), StaticMesh.Sockets.Num(), 1);
    if (!StaticMesh.Sockets.IsEmpty())
    {
        bSuccess &= Test.TestEqual(TEXT("SOCKET_ prefix removed"), StaticMesh.Sockets[0]->SocketName, FName(TEXT("mount")));
        bSuccess &= Test.TestEqual(TEXT("socket transform applied"), StaticMesh.Sockets[0]->RelativeLocation, FVector(10.0, 20.0, 30.0));
    }

    const UBodySetup* BodySetup = StaticMesh.GetBodySetup();
    bSuccess &= Test.TestNotNull(TEXT("first BodySetup exists"), BodySetup);
    if (BodySetup != nullptr)
    {
        bSuccess &= Test.TestEqual(TEXT("first collision hull count"), BodySetup->AggGeom.ConvexElems.Num(), 1);
        bSuccess &= Test.TestEqual(TEXT("collision trace flag pinned"), BodySetup->CollisionTraceFlag, CTF_UseDefault);
        if (!BodySetup->AggGeom.ConvexElems.IsEmpty())
        {
            const FKConvexElem& Hull = BodySetup->AggGeom.ConvexElems[0];
            bSuccess &= Test.TestEqual(TEXT("first hull point count"), Hull.VertexData.Num(), 4);
            bSuccess &= Test.TestEqual(TEXT("trace hull is query-only"), Hull.GetCollisionEnabled(), ECollisionEnabled::QueryOnly);
        }
    }
    return bSuccess;
}

bool VerifyReplacement(FAutomationTestBase& Test, UStaticMesh& StaticMesh, UMaterialInstanceConstant* Material)
{
    bool bSuccess = true;
    bSuccess &= Test.TestEqual(TEXT("replacement removed old LOD1"), StaticMesh.GetNumSourceModels(), 1);
    bSuccess &= Test.TestNotNull(TEXT("replacement LOD0 exists"), StaticMesh.GetMeshDescription(0));

    bSuccess &= Test.TestEqual(TEXT("replacement material count"), StaticMesh.GetStaticMaterials().Num(), 1);
    if (!StaticMesh.GetStaticMaterials().IsEmpty())
    {
        const FStaticMaterial& Slot = StaticMesh.GetStaticMaterials()[0];
        bSuccess &= Test.TestTrue(TEXT("trim MI replaced body MI"), Slot.MaterialInterface.Get() == Material);
        bSuccess &= Test.TestEqual(TEXT("trim slot replaced body slot"), Slot.MaterialSlotName, FName(TEXT("trim")));
    }

    bSuccess &= Test.TestEqual(TEXT("replacement socket count"), StaticMesh.Sockets.Num(), 1);
    if (!StaticMesh.Sockets.IsEmpty())
    {
        bSuccess &= Test.TestEqual(TEXT("old socket replaced"), StaticMesh.Sockets[0]->SocketName, FName(TEXT("muzzle")));
        bSuccess &= Test.TestEqual(TEXT("replacement socket transform"), StaticMesh.Sockets[0]->RelativeLocation, FVector(-5.0, 6.0, 70.0));
    }

    const UBodySetup* BodySetup = StaticMesh.GetBodySetup();
    bSuccess &= Test.TestNotNull(TEXT("replacement BodySetup exists"), BodySetup);
    if (BodySetup != nullptr)
    {
        bSuccess &= Test.TestEqual(TEXT("old collision replaced, not appended"), BodySetup->AggGeom.ConvexElems.Num(), 1);
        if (!BodySetup->AggGeom.ConvexElems.IsEmpty())
        {
            const FKConvexElem& Hull = BodySetup->AggGeom.ConvexElems[0];
            bSuccess &= Test.TestEqual(TEXT("replacement hull point count"), Hull.VertexData.Num(), 4);
            bSuccess &= Test.TestEqual(TEXT("replacement hull is physics-only"), Hull.GetCollisionEnabled(), ECollisionEnabled::PhysicsOnly);
            bSuccess &= Test.TestTrue(
                TEXT("replacement collision vertex inventory replaced old hull"),
                Hull.VertexData.Contains(FVector(5.0, 6.0, 7.0)));
        }
    }
    return bSuccess;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshBuilderRebuildTest,
    "Mimir.V4.StaticMesh.Builder.FullInPlaceRebuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshBuilderRebuildTest::RunTest(const FString& Parameters)
{
    UStaticMesh* StaticMesh = NewObject<UStaticMesh>(
        GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("MH_StaticMeshBuilder_Test")),
        RF_Transient | RF_Transactional);
    UMaterialInstanceConstant* BodyMaterial = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
    UMaterialInstanceConstant* TrimMaterial = NewObject<UMaterialInstanceConstant>(GetTransientPackage());

    FMHSceneIR First = FirstScene();
    FMHStaticMeshBuildPlan FirstPlan;
    FirstPlan.Scene = &First;
    FirstPlan.Materials.Add(TEXT("body"), BodyMaterial);
    FString Error;
    if (!FMHStaticMeshBuilder::Rebuild(*StaticMesh, FirstPlan, Error))
    {
        AddError(FString::Printf(TEXT("first rebuild failed: %s"), *Error));
        return false;
    }

    bool bSuccess = TestTrue(TEXT("first rebuild reports no error"), Error.IsEmpty());
    bSuccess &= VerifyFirstBuild(*this, *StaticMesh, BodyMaterial);
    UStaticMesh* const OriginalPointer = StaticMesh;

    FMHSceneIR Replacement = ReplacementScene();
    FMHStaticMeshBuildPlan ReplacementPlan;
    ReplacementPlan.Scene = &Replacement;
    ReplacementPlan.Materials.Add(TEXT("trim"), TrimMaterial);
    if (!FMHStaticMeshBuilder::Rebuild(*StaticMesh, ReplacementPlan, Error))
    {
        AddError(FString::Printf(TEXT("replacement rebuild failed: %s"), *Error));
        return false;
    }

    bSuccess &= TestTrue(TEXT("replacement rebuild reports no error"), Error.IsEmpty());
    bSuccess &= TestEqual(TEXT("rebuild preserves UObject identity"), StaticMesh, OriginalPointer);
    bSuccess &= VerifyReplacement(*this, *StaticMesh, TrimMaterial);
    return bSuccess;
}

} // namespace UE::MimirComposite::Tests
