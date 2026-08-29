#include "StaticMesh/MHStaticMeshImporter.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Geometry/MHSceneIR.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"
#include "StaticMeshResources.h"
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

namespace
{
FMHSceneGeometry OffsetTriangleGeometry(const float Size, const FVector3f& Offset)
{
    FMHSceneGeometry Geometry;
    Geometry.Positions = {
        Offset,
        Offset + FVector3f(Size, 0.0f, 0.0f),
        Offset + FVector3f(0.0f, Size, 0.0f)};
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

FMHSceneIRNode UnionRenderNode(
    const TCHAR* Name,
    const int32 LODLevel,
    const TCHAR* Slot,
    const FVector3f& Offset)
{
    FMHSceneIRNode Node;
    Node.Name = Name;
    Node.Attribute = EMHSceneNodeAttribute::Mesh;
    Node.Kind = EMHSceneNodeKind::Render;
    Node.LODLevel = LODLevel;
    Node.MaterialSlots = {Slot};
    Node.Geometry = OffsetTriangleGeometry(
        100.0f,
        Offset + FVector3f(0.0f, 0.0f, static_cast<float>(LODLevel) * 10.0f));
    return Node;
}

/** Assert the persisted section map and the built render sections agree. */
bool CheckSection(
    FAutomationTestBase& Test,
    UStaticMesh& StaticMesh,
    const TCHAR* Label,
    const int32 LODIndex,
    const int32 SectionIndex,
    const int32 ExpectedMaterialIndex)
{
    bool bSuccess = Test.TestEqual(
        FString::Printf(TEXT("%s: SectionInfoMap LOD%d section %d"), Label, LODIndex, SectionIndex),
        StaticMesh.GetSectionInfoMap().Get(LODIndex, SectionIndex).MaterialIndex,
        ExpectedMaterialIndex);
    const FStaticMeshRenderData* RenderData = StaticMesh.GetRenderData();
    if (RenderData == nullptr || !RenderData->LODResources.IsValidIndex(LODIndex))
    {
        Test.AddError(FString::Printf(TEXT("%s: no render data for LOD%d"), Label, LODIndex));
        return false;
    }
    const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
    if (!LOD.Sections.IsValidIndex(SectionIndex))
    {
        Test.AddError(FString::Printf(
            TEXT("%s: LOD%d has %d sections, expected section %d"),
            Label,
            LODIndex,
            LOD.Sections.Num(),
            SectionIndex));
        return false;
    }
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s: render LOD%d section %d material"), Label, LODIndex, SectionIndex),
        LOD.Sections[SectionIndex].MaterialIndex,
        ExpectedMaterialIndex);
    return bSuccess;
}

/**
 * The authored LOD must survive the build. LOD1+ source models are seeded with
 * the LOD group's auto-reduction defaults, which would otherwise regenerate the
 * LOD from LOD0 and destroy its own section inventory.
 */
bool CheckAuthoredLod(
    FAutomationTestBase& Test,
    UStaticMesh& StaticMesh,
    const TCHAR* Label,
    const int32 LODIndex,
    const int32 ExpectedSectionCount,
    const int32 ExpectedTriangleCount)
{
    bool bSuccess = Test.TestFalse(
        FString::Printf(TEXT("%s: LOD%d auto-reduction is disabled"), Label, LODIndex),
        StaticMesh.IsReductionActive(LODIndex));
    const FStaticMeshRenderData* RenderData = StaticMesh.GetRenderData();
    if (RenderData == nullptr || !RenderData->LODResources.IsValidIndex(LODIndex))
    {
        Test.AddError(FString::Printf(TEXT("%s: no render data for LOD%d"), Label, LODIndex));
        return false;
    }
    const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s: LOD%d section count"), Label, LODIndex),
        LOD.Sections.Num(),
        ExpectedSectionCount);
    bSuccess &= Test.TestEqual(
        FString::Printf(TEXT("%s: LOD%d keeps its authored triangles"), Label, LODIndex),
        LOD.GetNumTriangles(),
        ExpectedTriangleCount);
    return bSuccess;
}

TArray<FName> SlotNames(const UStaticMesh& StaticMesh)
{
    TArray<FName> Names;
    for (const FStaticMaterial& Slot : StaticMesh.GetStaticMaterials())
    {
        Names.Add(Slot.MaterialSlotName);
    }
    return Names;
}

/**
 * LOD0 owns body/glass; LOD1 replaces body with its own simplified shader and
 * reuses glass. The union list is [body, glass, body_simple] and LOD1 sections
 * must address slots 2 and 1, not their own positional order.
 */
FMHSceneIR UnionScene()
{
    FMHSceneIR Scene;
    Scene.ResourceName = TEXT("gaz53");
    Scene.MaterialNames = {TEXT("body"), TEXT("glass"), TEXT("body_simple")};
    Scene.LODLevels = {0, 1};
    Scene.bUsesExplicitLODs = true;
    Scene.Nodes = {
        UnionRenderNode(TEXT("hull_lod00"), 0, TEXT("body"), FVector3f::ZeroVector),
        UnionRenderNode(TEXT("glass_lod00"), 0, TEXT("glass"), FVector3f(300.0f, 0.0f, 0.0f)),
        UnionRenderNode(TEXT("hull_lod01"), 1, TEXT("body_simple"), FVector3f::ZeroVector),
        UnionRenderNode(TEXT("glass_lod01"), 1, TEXT("glass"), FVector3f(300.0f, 0.0f, 0.0f))};
    return Scene;
}

/** Legacy subset shape: LOD1 keeps only the second LOD0 slot. */
FMHSceneIR SubsetScene()
{
    FMHSceneIR Scene;
    Scene.ResourceName = TEXT("gaz53");
    Scene.MaterialNames = {TEXT("body"), TEXT("glass")};
    Scene.LODLevels = {0, 1};
    Scene.bUsesExplicitLODs = true;
    Scene.Nodes = {
        UnionRenderNode(TEXT("hull_lod00"), 0, TEXT("body"), FVector3f::ZeroVector),
        UnionRenderNode(TEXT("glass_lod00"), 0, TEXT("glass"), FVector3f(300.0f, 0.0f, 0.0f)),
        UnionRenderNode(TEXT("glass_lod01"), 1, TEXT("glass"), FVector3f(300.0f, 0.0f, 0.0f))};
    return Scene;
}

UStaticMesh* NewTestStaticMesh()
{
    return NewObject<UStaticMesh>(
        GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("MH_StaticMeshUnion_Test")),
        RF_Transient | RF_Transactional);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshBuilderLodMaterialUnionTest,
    "Mimir.V4.StaticMesh.Builder.LodMaterialUnionSections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshBuilderLodMaterialUnionTest::RunTest(const FString& Parameters)
{
    UMaterialInstanceConstant* Body = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
    UMaterialInstanceConstant* Glass = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
    UMaterialInstanceConstant* BodySimple = NewObject<UMaterialInstanceConstant>(GetTransientPackage());

    // (a) LOD1 carries a material that LOD0 never uses.
    UStaticMesh* UnionMesh = NewTestStaticMesh();
    FMHSceneIR Union = UnionScene();
    FMHStaticMeshBuildPlan UnionPlan;
    UnionPlan.Scene = &Union;
    UnionPlan.Materials.Add(TEXT("body"), Body);
    UnionPlan.Materials.Add(TEXT("glass"), Glass);
    UnionPlan.Materials.Add(TEXT("body_simple"), BodySimple);
    FString Error;
    if (!FMHStaticMeshBuilder::Rebuild(*UnionMesh, UnionPlan, Error))
    {
        AddError(FString::Printf(TEXT("union rebuild failed: %s"), *Error));
        return false;
    }

    bool bSuccess = TestEqual(TEXT("union slot count"), UnionMesh->GetStaticMaterials().Num(), 3);
    const TArray<FName> Expected = {FName(TEXT("body")), FName(TEXT("glass")), FName(TEXT("body_simple"))};
    bSuccess &= TestTrue(TEXT("union slot order is LOD-major"), SlotNames(*UnionMesh) == Expected);
    if (UnionMesh->GetStaticMaterials().Num() == 3)
    {
        bSuccess &= TestTrue(
            TEXT("LOD1-only slot binds its own MI"),
            UnionMesh->GetStaticMaterials()[2].MaterialInterface.Get() == BodySimple);
        bSuccess &= TestEqual(
            TEXT("LOD1-only imported slot name"),
            UnionMesh->GetStaticMaterials()[2].ImportedMaterialSlotName,
            FName(TEXT("body_simple")));
    }
    bSuccess &= CheckAuthoredLod(*this, *UnionMesh, TEXT("union"), 0, 2, 2);
    bSuccess &= CheckAuthoredLod(*this, *UnionMesh, TEXT("union"), 1, 2, 2);
    // LOD0 sections keep their identity mapping.
    bSuccess &= CheckSection(*this, *UnionMesh, TEXT("union"), 0, 0, 0);
    bSuccess &= CheckSection(*this, *UnionMesh, TEXT("union"), 0, 1, 1);
    // LOD1 section 0 is body_simple (slot 2), section 1 is glass (slot 1).
    bSuccess &= CheckSection(*this, *UnionMesh, TEXT("union"), 1, 0, 2);
    bSuccess &= CheckSection(*this, *UnionMesh, TEXT("union"), 1, 1, 1);

    // (d) Reimport determinism: an unchanged scene rebuilds to the same order.
    FMHSceneIR Again = UnionScene();
    FMHStaticMeshBuildPlan AgainPlan;
    AgainPlan.Scene = &Again;
    AgainPlan.Materials = UnionPlan.Materials;
    if (!FMHStaticMeshBuilder::Rebuild(*UnionMesh, AgainPlan, Error))
    {
        AddError(FString::Printf(TEXT("union re-rebuild failed: %s"), *Error));
        return false;
    }
    bSuccess &= TestTrue(TEXT("reimport preserves slot order"), SlotNames(*UnionMesh) == Expected);
    bSuccess &= CheckSection(*this, *UnionMesh, TEXT("reimport"), 0, 0, 0);
    bSuccess &= CheckSection(*this, *UnionMesh, TEXT("reimport"), 0, 1, 1);
    bSuccess &= CheckSection(*this, *UnionMesh, TEXT("reimport"), 1, 0, 2);
    bSuccess &= CheckSection(*this, *UnionMesh, TEXT("reimport"), 1, 1, 1);

    // (b) Subset shape: LOD1 uses only the second LOD0 slot and must still
    // address it by name, never by its own section ordinal.
    UStaticMesh* SubsetMesh = NewTestStaticMesh();
    FMHSceneIR Subset = SubsetScene();
    FMHStaticMeshBuildPlan SubsetPlan;
    SubsetPlan.Scene = &Subset;
    SubsetPlan.Materials.Add(TEXT("body"), Body);
    SubsetPlan.Materials.Add(TEXT("glass"), Glass);
    if (!FMHStaticMeshBuilder::Rebuild(*SubsetMesh, SubsetPlan, Error))
    {
        AddError(FString::Printf(TEXT("subset rebuild failed: %s"), *Error));
        return false;
    }
    const TArray<FName> ExpectedSubset = {FName(TEXT("body")), FName(TEXT("glass"))};
    bSuccess &= TestTrue(TEXT("subset slot order unchanged"), SlotNames(*SubsetMesh) == ExpectedSubset);
    bSuccess &= CheckAuthoredLod(*this, *SubsetMesh, TEXT("subset"), 0, 2, 2);
    bSuccess &= CheckAuthoredLod(*this, *SubsetMesh, TEXT("subset"), 1, 1, 1);
    bSuccess &= CheckSection(*this, *SubsetMesh, TEXT("subset"), 0, 0, 0);
    bSuccess &= CheckSection(*this, *SubsetMesh, TEXT("subset"), 0, 1, 1);
    bSuccess &= CheckSection(*this, *SubsetMesh, TEXT("subset"), 1, 0, 1);
    return bSuccess;
}

} // namespace UE::MimirComposite::Tests
