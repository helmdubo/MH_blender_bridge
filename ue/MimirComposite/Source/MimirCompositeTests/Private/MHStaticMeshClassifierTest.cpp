#include "Geometry/MHSceneIR.h"

#include "Misc/AutomationTest.h"

using namespace UE::MimirComposite;

namespace
{
FMHSceneIRNode Node(
    const TCHAR* Name,
    const EMHSceneNodeAttribute Attribute,
    const int32 ParentIndex = INDEX_NONE,
    std::initializer_list<const TCHAR*> Slots = {})
{
    FMHSceneIRNode Result;
    Result.Name = Name;
    Result.Attribute = Attribute;
    Result.ParentIndex = ParentIndex;
    for (const TCHAR* Slot : Slots)
    {
        Result.MaterialSlots.Add(Slot);
    }
    return Result;
}

bool HasCode(const FString& Error, const TCHAR* Code)
{
    return Error.StartsWith(FString(Code) + TEXT(":"), ESearchCase::CaseSensitive);
}

bool Rejects(FMHSceneIR Scene, const TCHAR* Code)
{
    if (Scene.ResourceName.IsEmpty())
    {
        Scene.ResourceName = TEXT("test_mesh");
    }
    FString Error;
    return !MHClassifySceneIR(Scene, Error) && HasCode(Error, Code);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshClassifierValidSceneTest,
    "Mimir.V4.StaticMesh.Classifier.ValidScene",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshClassifierValidSceneTest::RunTest(const FString& Parameters)
{
    FMHSceneIR Scene;
    Scene.ResourceName = TEXT("vehicle");
    Scene.Nodes = {
        Node(TEXT("root"), EMHSceneNodeAttribute::Null),
        Node(TEXT("body_lod00"), EMHSceneNodeAttribute::Mesh, 0, {TEXT("body"), TEXT("glass")}),
        Node(TEXT("body_lod01"), EMHSceneNodeAttribute::Mesh, 0, {TEXT("body")}),
        Node(TEXT("UCX_body"), EMHSceneNodeAttribute::Mesh, 0),
        Node(TEXT("query_cls_trace"), EMHSceneNodeAttribute::Mesh, 0),
        Node(TEXT("SOCKET_driver"), EMHSceneNodeAttribute::Null, 0)
    };

    FString Error;
    bool bSuccess = TestTrue(TEXT("valid SceneIR classifies"), MHClassifySceneIR(Scene, Error));
    bSuccess &= TestTrue(TEXT("valid SceneIR has no error"), Error.IsEmpty());
    bSuccess &= TestEqual(TEXT("group classified"), Scene.Nodes[0].Kind, EMHSceneNodeKind::Group);
    bSuccess &= TestEqual(TEXT("LOD0 classified"), Scene.Nodes[1].LODLevel, 0);
    bSuccess &= TestEqual(TEXT("LOD1 classified"), Scene.Nodes[2].LODLevel, 1);
    bSuccess &= TestEqual(
        TEXT("UCX maps to query and physics"),
        Scene.Nodes[3].CollisionMode,
        EMHSceneCollisionMode::QueryAndPhysics);
    bSuccess &= TestEqual(
        TEXT("trace marker maps to query only"),
        Scene.Nodes[4].CollisionMode,
        EMHSceneCollisionMode::QueryOnly);
    bSuccess &= TestEqual(TEXT("socket prefix removed"), Scene.Nodes[5].SocketName, FString(TEXT("driver")));
    bSuccess &= TestTrue(TEXT("explicit LOD convention recorded"), Scene.bUsesExplicitLODs);
    bSuccess &= TestEqual(TEXT("two dense levels"), Scene.LODLevels.Num(), 2);
    bSuccess &= TestEqual(TEXT("material first-use order retained"), Scene.MaterialNames.Num(), 2);
    bSuccess &= TestEqual(TEXT("first material"), Scene.MaterialNames[0], FString(TEXT("body")));
    bSuccess &= TestEqual(TEXT("second material"), Scene.MaterialNames[1], FString(TEXT("glass")));
    return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshClassifierMarkerTest,
    "Mimir.V4.StaticMesh.Classifier.MarkerFailures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshClassifierMarkerTest::RunTest(const FString& Parameters)
{
    bool bSuccess = true;
    const TArray<FString> InvalidMeshNames = {
        TEXT("SOCKET_bad"), TEXT("UCX_bad_cls_both"), TEXT("UCX_bad_lod00")};
    for (const FString& Name : InvalidMeshNames)
    {
        FMHSceneIR Scene;
        Scene.Nodes = {Node(*Name, EMHSceneNodeAttribute::Mesh), Node(TEXT("render"), EMHSceneNodeAttribute::Mesh)};
        bSuccess &= TestTrue(
            FString::Printf(TEXT("invalid mesh marker rejected: %s"), *Name),
            Rejects(Scene, TEXT("MH_E_INVALID_NODE_MARKERS")));
    }

    const TArray<FString> InvalidNullNames = {
        TEXT("UCX_bad"), TEXT("bad_cls_trace"), TEXT("bad_lod00")};
    for (const FString& Name : InvalidNullNames)
    {
        FMHSceneIR Scene;
        Scene.Nodes = {Node(*Name, EMHSceneNodeAttribute::Null), Node(TEXT("render"), EMHSceneNodeAttribute::Mesh)};
        bSuccess &= TestTrue(
            FString::Printf(TEXT("invalid null marker rejected: %s"), *Name),
            Rejects(Scene, TEXT("MH_E_INVALID_NODE_MARKERS")));
    }

    FMHSceneIR EmptySocket;
    EmptySocket.Nodes = {Node(TEXT("SOCKET_"), EMHSceneNodeAttribute::Null), Node(TEXT("render"), EMHSceneNodeAttribute::Mesh)};
    bSuccess &= TestTrue(TEXT("empty socket name rejected"), Rejects(EmptySocket, TEXT("MH_E_INVALID_NODE_MARKERS")));

    FMHSceneIR SocketWithChild;
    SocketWithChild.Nodes = {
        Node(TEXT("SOCKET_driver"), EMHSceneNodeAttribute::Null),
        Node(TEXT("render"), EMHSceneNodeAttribute::Mesh, 0)};
    bSuccess &= TestTrue(TEXT("socket child rejected"), Rejects(SocketWithChild, TEXT("MH_E_INVALID_NODE_MARKERS")));

    FMHSceneIR DuplicateSocket;
    DuplicateSocket.Nodes = {
        Node(TEXT("SOCKET_driver"), EMHSceneNodeAttribute::Null),
        Node(TEXT("SOCKET_driver"), EMHSceneNodeAttribute::Null),
        Node(TEXT("render"), EMHSceneNodeAttribute::Mesh)};
    bSuccess &= TestTrue(
        TEXT("duplicate socket name uses owner-ratified source error"),
        Rejects(DuplicateSocket, TEXT("MH_E_INVALID_RESOURCE_SOURCE")));
    return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshClassifierGraphTest,
    "Mimir.V4.StaticMesh.Classifier.GraphFailures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshClassifierGraphTest::RunTest(const FString& Parameters)
{
    bool bSuccess = true;

    FMHSceneIR Duplicate;
    Duplicate.Nodes = {Node(TEXT("same"), EMHSceneNodeAttribute::Mesh), Node(TEXT("same"), EMHSceneNodeAttribute::Mesh)};
    bSuccess &= TestTrue(TEXT("duplicate Model names rejected"), Rejects(Duplicate, TEXT("MH_E_IMPORT_TARGET_OCCUPIED")));

    FMHSceneIR Outside;
    Outside.Nodes = {Node(TEXT("render"), EMHSceneNodeAttribute::Mesh, 7)};
    bSuccess &= TestTrue(TEXT("outside parent rejected"), Rejects(Outside, TEXT("MH_E_PARENT_OUTSIDE_RESOURCE")));

    FMHSceneIR Cycle;
    Cycle.Nodes = {Node(TEXT("a"), EMHSceneNodeAttribute::Mesh, 1), Node(TEXT("b"), EMHSceneNodeAttribute::Null, 0)};
    bSuccess &= TestTrue(TEXT("parent cycle rejected"), Rejects(Cycle, TEXT("MH_E_PARENT_CYCLE")));

    FMHSceneIR Unsupported;
    Unsupported.Nodes = {Node(TEXT("bone"), EMHSceneNodeAttribute::Unsupported)};
    bSuccess &= TestTrue(TEXT("unsupported attribute rejected"), Rejects(Unsupported, TEXT("MH_E_UNSUPPORTED_NODE_KIND")));

    FMHSceneIR NullWithSlots;
    NullWithSlots.Nodes = {
        Node(TEXT("group"), EMHSceneNodeAttribute::Null, INDEX_NONE, {TEXT("body")}),
        Node(TEXT("render"), EMHSceneNodeAttribute::Mesh)};
    bSuccess &= TestTrue(
        TEXT("null node payload rejected"),
        Rejects(NullWithSlots, TEXT("MH_E_UNSUPPORTED_NODE_KIND")));

    FMHSceneIR NoRender;
    NoRender.Nodes = {Node(TEXT("root"), EMHSceneNodeAttribute::Null), Node(TEXT("UCX_body"), EMHSceneNodeAttribute::Mesh)};
    bSuccess &= TestTrue(TEXT("scene without render rejected"), Rejects(NoRender, TEXT("MH_E_EMPTY_RESOURCE_COLLECTION")));
    return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshClassifierLodAndSlotTest,
    "Mimir.V4.StaticMesh.Classifier.LodAndSlotFailures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshClassifierLodAndSlotTest::RunTest(const FString& Parameters)
{
    bool bSuccess = true;

    FMHSceneIR Mixed;
    Mixed.Nodes = {Node(TEXT("body_lod00"), EMHSceneNodeAttribute::Mesh), Node(TEXT("wheel"), EMHSceneNodeAttribute::Mesh)};
    bSuccess &= TestTrue(TEXT("mixed explicit/plain LOD rejected"), Rejects(Mixed, TEXT("MH_E_INVALID_LOD_HIERARCHY")));

    FMHSceneIR Sparse;
    Sparse.Nodes = {Node(TEXT("body_lod00"), EMHSceneNodeAttribute::Mesh), Node(TEXT("body_lod02"), EMHSceneNodeAttribute::Mesh)};
    bSuccess &= TestTrue(TEXT("sparse LOD rejected"), Rejects(Sparse, TEXT("MH_E_LOD_LEVELS_SPARSE")));

    FMHSceneIR MissingBaseSlot;
    MissingBaseSlot.Nodes = {
        Node(TEXT("body_lod00"), EMHSceneNodeAttribute::Mesh, INDEX_NONE, {TEXT("body")}),
        Node(TEXT("body_lod01"), EMHSceneNodeAttribute::Mesh, INDEX_NONE, {TEXT("glass")})};
    bSuccess &= TestTrue(TEXT("higher-only slot rejected"), Rejects(MissingBaseSlot, TEXT("MH_E_LOD_SLOT_NOT_IN_BASE")));

    FMHSceneIR Noncanonical;
    Noncanonical.Nodes = {Node(TEXT("body"), EMHSceneNodeAttribute::Mesh, INDEX_NONE, {TEXT("Body")})};
    bSuccess &= TestTrue(TEXT("noncanonical slot rejected"), Rejects(Noncanonical, TEXT("MH_E_NONCANONICAL_RESOURCE_NAME")));

    FMHSceneIR NoncanonicalResource;
    NoncanonicalResource.ResourceName = TEXT("BadMesh");
    NoncanonicalResource.Nodes = {Node(TEXT("body"), EMHSceneNodeAttribute::Mesh)};
    bSuccess &= TestTrue(
        TEXT("noncanonical resource rejected"),
        Rejects(NoncanonicalResource, TEXT("MH_E_NONCANONICAL_RESOURCE_NAME")));

    FMHSceneIR Atomic;
    Atomic.Nodes = {Node(TEXT("body_lod01"), EMHSceneNodeAttribute::Mesh)};
    Atomic.MaterialNames = {TEXT("sentinel")};
    FString Error;
    bSuccess &= TestFalse(TEXT("lod01 without lod00 fails"), MHClassifySceneIR(Atomic, Error));
    bSuccess &= TestEqual(TEXT("failed classification preserves input"), Atomic.MaterialNames[0], FString(TEXT("sentinel")));
    bSuccess &= TestEqual(TEXT("failed classification leaves kind untouched"), Atomic.Nodes[0].Kind, EMHSceneNodeKind::Unclassified);
    return bSuccess;
}
