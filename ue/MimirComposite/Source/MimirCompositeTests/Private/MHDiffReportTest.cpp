#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Source/MHSourceAnalyzer.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace UE::MimirComposite;

namespace
{

const TCHAR* MeshUid = TEXT("10000000-0000-4000-8000-000000000001");
const TCHAR* CompositeUid = TEXT("20000000-0000-4000-8000-000000000001");
const TCHAR* CreatedUid = TEXT("30000000-0000-4000-8000-000000000001");
const TCHAR* RemovedUid = TEXT("40000000-0000-4000-8000-000000000001");
const TCHAR* StableUid = TEXT("50000000-0000-4000-8000-000000000001");
const TCHAR* CaseMaterialUid = TEXT("60000000-0000-4000-8000-000000000001");
const TCHAR* CaseCompositeUid = TEXT("70000000-0000-4000-8000-000000000001");

const TCHAR* ChangedNodeUid = TEXT("21000000-0000-4000-8000-000000000001");
const TCHAR* CreatedNodeUid = TEXT("22000000-0000-4000-8000-000000000001");
const TCHAR* RemovedNodeUid = TEXT("23000000-0000-4000-8000-000000000001");
const TCHAR* StableNodeUid = TEXT("24000000-0000-4000-8000-000000000001");
const TCHAR* CaseNodeUid = TEXT("71000000-0000-4000-8000-000000000001");

FMHDiffResourceValue MakeResource(
    const TCHAR* Uid,
    const EMHResourceKind Kind,
    const TCHAR* Name,
    const TCHAR* SourcePath,
    const TCHAR* SemanticValue)
{
    FMHDiffResourceValue Value;
    Value.ResourceUid = Uid;
    Value.Kind = Kind;
    Value.Name = Name;
    Value.SourcePath = SourcePath;
    Value.CanonicalSemanticValue = SemanticValue;
    Value.bHasValidatedSemanticValue = true;
    if (Kind == EMHResourceKind::StaticMesh)
    {
        Value.GeometryHash = TEXT("xxh3:0000000000000001");
        Value.DescriptorHash = TEXT("xxh3:0000000000000011");
    }
    if (Kind == EMHResourceKind::Composite)
    {
        Value.bHasValidatedNodeValues = true;
    }
    return Value;
}

FMHDiffNodeValue MakeNode(
    const TCHAR* Uid,
    const TCHAR* DisplayName,
    const TCHAR* Transform,
    const TCHAR* Properties,
    const TCHAR* ParentUid,
    const TCHAR* ResourceUid,
    const TCHAR* Kind)
{
    FMHDiffNodeValue Value;
    Value.NodeUid = Uid;
    Value.DisplayName = DisplayName;
    Value.CanonicalTransformValue = Transform;
    Value.CanonicalPropertiesValue = Properties;
    Value.ParentUid = ParentUid;
    Value.ResourceUid = ResourceUid;
    Value.Kind = Kind;
    return Value;
}

bool CheckFlags(
    FAutomationTestBase& Test,
    const FString& Label,
    const TArray<EMHDiffFlag>& Actual,
    const TArray<EMHDiffFlag>& Expected)
{
    bool bPassed = Test.TestEqual(Label + TEXT(" count"), Actual.Num(), Expected.Num());
    const int32 SharedCount = FMath::Min(Actual.Num(), Expected.Num());
    for (int32 Index = 0; Index < SharedCount; ++Index)
    {
        bPassed &= Test.TestEqual(
            FString::Printf(TEXT("%s flag %d"), *Label, Index),
            static_cast<int32>(Actual[Index]),
            static_cast<int32>(Expected[Index]));
    }
    return bPassed;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDiffReportParityModelTest,
    "Mimir.C1.DiffReport.ParityModel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDiffReportParityModelTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FMHDiffSnapshotValue OldSnapshot;
    FMHDiffSnapshotValue NewSnapshot;

    FMHDiffResourceValue OldMesh = MakeResource(
        MeshUid,
        EMHResourceKind::StaticMesh,
        TEXT("mesh_old"),
        TEXT("old/mesh.mesh.fbx"),
        TEXT("{mesh_semantic:old}"));
    FMHDiffResourceValue NewMesh = MakeResource(
        MeshUid,
        EMHResourceKind::StaticMesh,
        TEXT("mesh_new"),
        TEXT("new/mesh.mesh.fbx"),
        TEXT("{mesh_semantic:new}"));
    NewMesh.GeometryHash = TEXT("xxh3:0000000000000002");
    NewMesh.DescriptorHash = TEXT("xxh3:0000000000000012");
    OldSnapshot.Resources.Add(MeshUid, OldMesh);
    NewSnapshot.Resources.Add(MeshUid, NewMesh);

    FMHDiffResourceValue OldComposite = MakeResource(
        CompositeUid,
        EMHResourceKind::Composite,
        TEXT("composite"),
        TEXT("composites/composite.composite"),
        TEXT("{resource_properties:old}"));
    FMHDiffResourceValue NewComposite = MakeResource(
        CompositeUid,
        EMHResourceKind::Composite,
        TEXT("composite"),
        TEXT("composites/composite.composite"),
        TEXT("{resource_properties:new}"));

    OldComposite.Nodes.Add(MakeNode(
        ChangedNodeUid,
        TEXT("node_old"),
        TEXT("transform:old"),
        TEXT("properties:old"),
        TEXT("parent_old"),
        TEXT("resource_old"),
        TEXT("group")));
    NewComposite.Nodes.Add(MakeNode(
        ChangedNodeUid,
        TEXT("node_new"),
        TEXT("transform:new"),
        TEXT("properties:new"),
        TEXT("parent_new"),
        TEXT("resource_new"),
        TEXT("mesh")));
    OldComposite.Nodes.Add(MakeNode(
        RemovedNodeUid,
        TEXT("removed"),
        TEXT("transform:identity"),
        TEXT("{}"),
        TEXT(""),
        TEXT(""),
        TEXT("group")));
    NewComposite.Nodes.Add(MakeNode(
        CreatedNodeUid,
        TEXT("created"),
        TEXT("transform:identity"),
        TEXT("{}"),
        TEXT(""),
        TEXT(""),
        TEXT("group")));
    const FMHDiffNodeValue StableNode = MakeNode(
        StableNodeUid,
        TEXT("stable"),
        TEXT("transform:identity"),
        TEXT("{}"),
        TEXT(""),
        TEXT(""),
        TEXT("group"));
    OldComposite.Nodes.Add(StableNode);
    NewComposite.Nodes.Add(StableNode);
    OldSnapshot.Resources.Add(CompositeUid, OldComposite);
    NewSnapshot.Resources.Add(CompositeUid, NewComposite);

    OldSnapshot.Resources.Add(
        RemovedUid,
        MakeResource(
            RemovedUid,
            EMHResourceKind::Material,
            TEXT("removed"),
            TEXT("materials/removed.material"),
            TEXT("{material:removed}")));
    NewSnapshot.Resources.Add(
        CreatedUid,
        MakeResource(
            CreatedUid,
            EMHResourceKind::Material,
            TEXT("created"),
            TEXT("materials/created.material"),
            TEXT("{material:created}")));

    const FMHDiffResourceValue Stable = MakeResource(
        StableUid,
        EMHResourceKind::Material,
        TEXT("stable"),
        TEXT("materials/stable.material"),
        TEXT("{material:stable}"));
    OldSnapshot.Resources.Add(StableUid, Stable);
    NewSnapshot.Resources.Add(StableUid, Stable);

    FMHDiffReport Report;
    FString Error;
    if (!TestTrue(TEXT("validated snapshots build"), MHBuildDiffReport(
            OldSnapshot,
            NewSnapshot,
            Report,
            Error)))
    {
        AddError(Error);
        return false;
    }

    bool bPassed = true;
    const FMHDiffResourceOp* MeshOp = Report.FindResource(MeshUid);
    bPassed &= TestTrue(TEXT("mesh resource operation exists"), MeshOp != nullptr);
    if (MeshOp != nullptr)
    {
        bPassed &= CheckFlags(
            *this,
            TEXT("mesh independent flags"),
            MeshOp->Flags,
            {EMHDiffFlag::Rename,
             EMHDiffFlag::UpdateGeometry,
             EMHDiffFlag::UpdateProperties,
             EMHDiffFlag::Move});
    }
    const FMHDiffResourceOp* CompositeOp = Report.FindResource(CompositeUid);
    bPassed &= TestTrue(TEXT("composite properties operation exists"), CompositeOp != nullptr);
    if (CompositeOp != nullptr)
    {
        bPassed &= CheckFlags(
            *this,
            TEXT("composite resource flags"),
            CompositeOp->Flags,
            {EMHDiffFlag::UpdateProperties});
    }
    const FMHDiffResourceOp* CreatedOp = Report.FindResource(CreatedUid);
    const FMHDiffResourceOp* RemovedOp = Report.FindResource(RemovedUid);
    bPassed &= TestTrue(TEXT("create resource operation exists"), CreatedOp != nullptr);
    bPassed &= TestTrue(TEXT("remove resource operation exists"), RemovedOp != nullptr);
    if (CreatedOp != nullptr)
    {
        bPassed &= CheckFlags(*this, TEXT("create is exclusive"), CreatedOp->Flags, {EMHDiffFlag::Create});
    }
    if (RemovedOp != nullptr)
    {
        bPassed &= CheckFlags(*this, TEXT("remove is exclusive"), RemovedOp->Flags, {EMHDiffFlag::Remove});
    }
    bPassed &= TestTrue(TEXT("unchanged resource omitted"), Report.FindResource(StableUid) == nullptr);

    const FMHDiffCompositeNodeOps* CompositeNodes = Report.FindCompositeNodes(CompositeUid);
    bPassed &= TestTrue(TEXT("composite node operation map exists"), CompositeNodes != nullptr);
    if (CompositeNodes != nullptr)
    {
        const FMHDiffNodeOp* ChangedNode = CompositeNodes->Find(ChangedNodeUid);
        const FMHDiffNodeOp* CreatedNode = CompositeNodes->Find(CreatedNodeUid);
        const FMHDiffNodeOp* RemovedNode = CompositeNodes->Find(RemovedNodeUid);
        bPassed &= TestTrue(TEXT("changed node exists"), ChangedNode != nullptr);
        bPassed &= TestTrue(TEXT("created node exists"), CreatedNode != nullptr);
        bPassed &= TestTrue(TEXT("removed node exists"), RemovedNode != nullptr);
        bPassed &= TestTrue(TEXT("unchanged node omitted"), CompositeNodes->Find(StableNodeUid) == nullptr);
        if (ChangedNode != nullptr)
        {
            bPassed &= CheckFlags(
                *this,
                TEXT("node flags use contract order"),
                ChangedNode->Flags,
                {EMHDiffFlag::Rename,
                 EMHDiffFlag::UpdateTransform,
                 EMHDiffFlag::UpdateProperties,
                 EMHDiffFlag::Reparent,
                 EMHDiffFlag::UpdateResource,
                 EMHDiffFlag::UpdateKind});
        }
        if (CreatedNode != nullptr)
        {
            bPassed &= CheckFlags(*this, TEXT("node create is exclusive"), CreatedNode->Flags, {EMHDiffFlag::Create});
        }
        if (RemovedNode != nullptr)
        {
            bPassed &= CheckFlags(*this, TEXT("node remove is exclusive"), RemovedNode->Flags, {EMHDiffFlag::Remove});
        }
    }

    FString Json;
    if (!TestTrue(TEXT("report serializes"), MHDiffReportToJson(Report, Json, Error)))
    {
        AddError(Error);
        return false;
    }
    const FString ExpectedJson = TEXT(
        "{\n"
        "  \"schema\": \"mh.diff_report\",\n"
        "  \"schema_version\": 1,\n"
        "  \"resources\": {\n"
        "    \"10000000-0000-4000-8000-000000000001\": [\n"
        "      \"RENAME\",\n"
        "      \"UPDATE_GEOMETRY\",\n"
        "      \"UPDATE_PROPERTIES\",\n"
        "      \"MOVE\"\n"
        "    ],\n"
        "    \"20000000-0000-4000-8000-000000000001\": [\n"
        "      \"UPDATE_PROPERTIES\"\n"
        "    ],\n"
        "    \"30000000-0000-4000-8000-000000000001\": [\n"
        "      \"CREATE\"\n"
        "    ],\n"
        "    \"40000000-0000-4000-8000-000000000001\": [\n"
        "      \"REMOVE\"\n"
        "    ]\n"
        "  },\n"
        "  \"nodes\": {\n"
        "    \"20000000-0000-4000-8000-000000000001\": {\n"
        "      \"21000000-0000-4000-8000-000000000001\": [\n"
        "        \"RENAME\",\n"
        "        \"UPDATE_TRANSFORM\",\n"
        "        \"UPDATE_PROPERTIES\",\n"
        "        \"REPARENT\",\n"
        "        \"UPDATE_RESOURCE\",\n"
        "        \"UPDATE_KIND\"\n"
        "      ],\n"
        "      \"22000000-0000-4000-8000-000000000001\": [\n"
        "        \"CREATE\"\n"
        "      ],\n"
        "      \"23000000-0000-4000-8000-000000000001\": [\n"
        "        \"REMOVE\"\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n");
    bPassed &= TestEqual(TEXT("Python-compatible report bytes"), Json, ExpectedJson);

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    bPassed &= TestTrue(TEXT("serialized report parses"), FJsonSerializer::Deserialize(Reader, Root));
    if (Root.IsValid())
    {
        bPassed &= TestEqual(TEXT("report schema"), Root->GetStringField(TEXT("schema")), TEXT("mh.diff_report"));
        bPassed &= TestEqual(TEXT("report schema version"), Root->GetIntegerField(TEXT("schema_version")), 1);
        bPassed &= TestTrue(TEXT("resources object emitted"), Root->HasTypedField<EJson::Object>(TEXT("resources")));
        bPassed &= TestTrue(TEXT("nodes object emitted"), Root->HasTypedField<EJson::Object>(TEXT("nodes")));
    }

    bPassed &= TestEqual(
        TEXT("scalar execution priority remains unchanged"),
        FString(MHSourceChangeLabel(EMHSourceChange::UpdateGeometry)),
        FString(TEXT("UPDATE_GEOMETRY")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDiffReportCaseSensitiveTest,
    "Mimir.C1.DiffReport.CaseSensitiveParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDiffReportCaseSensitiveTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FMHDiffSnapshotValue OldSnapshot;
    FMHDiffSnapshotValue NewSnapshot;

    OldSnapshot.Resources.Add(
        CaseMaterialUid,
        MakeResource(
            CaseMaterialUid,
            EMHResourceKind::Material,
            TEXT("MaterialCase"),
            TEXT("Materials/Case.material"),
            TEXT("{\"shader_class\":\"Opaque\"}")));
    NewSnapshot.Resources.Add(
        CaseMaterialUid,
        MakeResource(
            CaseMaterialUid,
            EMHResourceKind::Material,
            TEXT("materialCase"),
            TEXT("materials/Case.material"),
            TEXT("{\"shader_class\":\"opaque\"}")));

    FMHDiffResourceValue OldComposite = MakeResource(
        CaseCompositeUid,
        EMHResourceKind::Composite,
        TEXT("case_composite"),
        TEXT("composites/case.composite"),
        TEXT("{\"properties\":{}}"));
    FMHDiffResourceValue NewComposite = OldComposite;
    OldComposite.Nodes.Add(MakeNode(
        CaseNodeUid,
        TEXT("NodeCase"),
        TEXT("{\"space\":\"Local\"}"),
        TEXT("{\"label\":\"Value\"}"),
        TEXT(""),
        TEXT(""),
        TEXT("group")));
    NewComposite.Nodes.Add(MakeNode(
        CaseNodeUid,
        TEXT("nodeCase"),
        TEXT("{\"space\":\"local\"}"),
        TEXT("{\"label\":\"value\"}"),
        TEXT(""),
        TEXT(""),
        TEXT("group")));
    OldSnapshot.Resources.Add(CaseCompositeUid, MoveTemp(OldComposite));
    NewSnapshot.Resources.Add(CaseCompositeUid, MoveTemp(NewComposite));

    FMHDiffReport Report;
    FString Error;
    if (!TestTrue(
            TEXT("case-only changes build"),
            MHBuildDiffReport(OldSnapshot, NewSnapshot, Report, Error)))
    {
        AddError(Error);
        return false;
    }

    bool bPassed = true;
    const FMHDiffResourceOp* MaterialOp = Report.FindResource(CaseMaterialUid);
    bPassed &= TestTrue(TEXT("case-only material resource operation exists"), MaterialOp != nullptr);
    if (MaterialOp != nullptr)
    {
        bPassed &= CheckFlags(
            *this,
            TEXT("case-only name semantic and path changes"),
            MaterialOp->Flags,
            {EMHDiffFlag::Rename, EMHDiffFlag::UpdateProperties, EMHDiffFlag::Move});
    }

    const FMHDiffCompositeNodeOps* CompositeNodes = Report.FindCompositeNodes(CaseCompositeUid);
    bPassed &= TestTrue(TEXT("case-only composite node operation exists"), CompositeNodes != nullptr);
    if (CompositeNodes != nullptr)
    {
        const FMHDiffNodeOp* NodeOp = CompositeNodes->Find(CaseNodeUid);
        bPassed &= TestTrue(TEXT("case-only node operation exists"), NodeOp != nullptr);
        if (NodeOp != nullptr)
        {
            bPassed &= CheckFlags(
                *this,
                TEXT("case-only node fields change"),
                NodeOp->Flags,
                {EMHDiffFlag::Rename,
                 EMHDiffFlag::UpdateTransform,
                 EMHDiffFlag::UpdateProperties});
        }
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHDiffReportUnavailableBaseTest,
    "Mimir.C1.DiffReport.UnavailableBaseFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHDiffReportUnavailableBaseTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FMHDiffResourceValue OldComposite = MakeResource(
        CompositeUid,
        EMHResourceKind::Composite,
        TEXT("composite"),
        TEXT("composites/composite.composite"),
        TEXT(""));
    OldComposite.bHasValidatedSemanticValue = false;
    OldComposite.bHasValidatedNodeValues = false;

    FMHDiffResourceValue NewComposite = MakeResource(
        CompositeUid,
        EMHResourceKind::Composite,
        TEXT("composite_renamed"),
        TEXT("composites/composite.composite"),
        TEXT("{resource_properties:new}"));

    FMHDiffSnapshotValue OldSnapshot;
    FMHDiffSnapshotValue NewSnapshot;
    OldSnapshot.Resources.Add(CompositeUid, MoveTemp(OldComposite));
    NewSnapshot.Resources.Add(CompositeUid, MoveTemp(NewComposite));

    FMHDiffReport Report;
    FMHDiffResourceOp StaleOp;
    StaleOp.ResourceUid = TEXT("stale");
    StaleOp.Flags.Add(EMHDiffFlag::Rename);
    Report.Resources.Add(MoveTemp(StaleOp));
    FString Error;

    bool bPassed = TestFalse(
        TEXT("Ledger-like incomplete base is rejected"),
        MHBuildDiffReport(OldSnapshot, NewSnapshot, Report, Error));
    bPassed &= TestTrue(
        TEXT("failure is a source snapshot diagnostic"),
        Error.StartsWith(TEXT("MH_E_SOURCE_INDEX_INVALID:")));
    bPassed &= TestEqual(TEXT("failed build leaves no resource operations"), Report.Resources.Num(), 0);
    bPassed &= TestEqual(TEXT("failed build leaves no node operations"), Report.Nodes.Num(), 0);
    return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
