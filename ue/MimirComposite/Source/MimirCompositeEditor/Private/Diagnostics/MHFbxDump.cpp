#include "Diagnostics/MHFbxDump.h"

#include "Canonical/MHCanonical.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMath.h"
#include "Misc/Paths.h"

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace
{
struct FDumpSummary
{
    int32 NodeCount = 0;
    int32 MeshNodeCount = 0;
    int64 ControlPointCount = 0;
    int64 PolygonCount = 0;
    int64 PolygonVertexCount = 0;
    int32 InvalidLodMeshNodeCount = 0;
    TMap<int32, int32> MeshNodesPerLod;
};

struct FLodLevel
{
    bool bExplicit = false;
    bool bValid = true;
    int32 Value = 0;
    FString FbxType = TEXT("none");
    TSharedPtr<FJsonValue> RawValue = MakeShared<FJsonValueNull>();
};

struct FDumpContext
{
    bool bFull = false;
    FString Error;
    FDumpSummary Summary;
    TArray<TSharedPtr<FJsonValue>> Nodes;
};

FString FromFbx(const char* Value)
{
    return Value != nullptr ? UTF8_TO_TCHAR(Value) : FString();
}

TSharedPtr<FJsonValue> JsonInteger(const int64 Value)
{
    return MakeShared<FJsonValueNumberString>(LexToString(Value));
}

bool Quantize(const double Value, int64& OutTick, FString& OutError)
{
    const UE::MimirComposite::FMHCanonicalResult Result =
        UE::MimirComposite::MHQuantize(Value, MH::FbxDump::QuantizationDigits, OutTick);
    if (!Result.bSuccess)
    {
        OutError = FString::Printf(
            TEXT("MH_E_FBX_DUMP_INVALID_NUMBER: %s"),
            *Result.Error);
        return false;
    }
    return true;
}

TSharedPtr<FJsonValue> QuantizedNumber(const double Value, FDumpContext& Context)
{
    int64 Tick = 0;
    if (!Quantize(Value, Tick, Context.Error))
    {
        return MakeShared<FJsonValueNull>();
    }
    return JsonInteger(Tick);
}

TArray<TSharedPtr<FJsonValue>> QuantizedVector(
    const double X,
    const double Y,
    const double Z,
    FDumpContext& Context)
{
    return {
        QuantizedNumber(X, Context),
        QuantizedNumber(Y, Context),
        QuantizedNumber(Z, Context)};
}

TArray<TSharedPtr<FJsonValue>> QuantizedVector(const FbxDouble3& Value, FDumpContext& Context)
{
    return QuantizedVector(Value[0], Value[1], Value[2], Context);
}

TArray<TSharedPtr<FJsonValue>> QuantizedVector(const FbxVector4& Value, FDumpContext& Context)
{
    return QuantizedVector(Value[0], Value[1], Value[2], Context);
}

FString PropertyTypeName(const EFbxType Type)
{
    switch (Type)
    {
    case eFbxChar: return TEXT("char");
    case eFbxUChar: return TEXT("uchar");
    case eFbxShort: return TEXT("short");
    case eFbxUShort: return TEXT("ushort");
    case eFbxUInt: return TEXT("uint");
    case eFbxLongLong: return TEXT("long_long");
    case eFbxULongLong: return TEXT("ulong_long");
    case eFbxHalfFloat: return TEXT("half_float");
    case eFbxBool: return TEXT("bool");
    case eFbxInt: return TEXT("int");
    case eFbxFloat: return TEXT("float");
    case eFbxDouble: return TEXT("double");
    case eFbxDouble2: return TEXT("double2");
    case eFbxDouble3: return TEXT("double3");
    case eFbxDouble4: return TEXT("double4");
    case eFbxDouble4x4: return TEXT("double4x4");
    case eFbxEnum: return TEXT("enum");
    case eFbxEnumM: return TEXT("enum_multi");
    case eFbxString: return TEXT("string");
    case eFbxTime: return TEXT("time");
    case eFbxReference: return TEXT("reference");
    case eFbxBlob: return TEXT("blob");
    case eFbxDistance: return TEXT("distance");
    case eFbxDateTime: return TEXT("date_time");
    case eFbxUndefined:
    default:
        return TEXT("undefined");
    }
}

TSharedPtr<FJsonValue> PropertyValue(const FbxProperty& Property, FDumpContext& Context)
{
    switch (Property.GetPropertyDataType().GetType())
    {
    case eFbxChar:
        return JsonInteger(Property.Get<FbxChar>());
    case eFbxUChar:
        return JsonInteger(Property.Get<FbxUChar>());
    case eFbxShort:
        return JsonInteger(Property.Get<FbxShort>());
    case eFbxUShort:
        return JsonInteger(Property.Get<FbxUShort>());
    case eFbxUInt:
        return JsonInteger(Property.Get<FbxUInt>());
    case eFbxLongLong:
        return MakeShared<FJsonValueString>(LexToString(Property.Get<FbxLongLong>()));
    case eFbxULongLong:
        return MakeShared<FJsonValueString>(LexToString(Property.Get<FbxULongLong>()));
    case eFbxBool:
        return MakeShared<FJsonValueBoolean>(Property.Get<FbxBool>());
    case eFbxInt:
    case eFbxEnum:
    case eFbxEnumM:
        return JsonInteger(Property.Get<FbxInt>());
    case eFbxFloat:
        return QuantizedNumber(Property.Get<FbxFloat>(), Context);
    case eFbxDouble:
        return QuantizedNumber(Property.Get<FbxDouble>(), Context);
    case eFbxDouble2:
    {
        const FbxDouble2 Value = Property.Get<FbxDouble2>();
        return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            QuantizedNumber(Value[0], Context),
            QuantizedNumber(Value[1], Context)});
    }
    case eFbxDouble3:
    {
        const FbxDouble3 Value = Property.Get<FbxDouble3>();
        return MakeShared<FJsonValueArray>(QuantizedVector(Value, Context));
    }
    case eFbxDouble4:
    {
        const FbxDouble4 Value = Property.Get<FbxDouble4>();
        return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            QuantizedNumber(Value[0], Context),
            QuantizedNumber(Value[1], Context),
            QuantizedNumber(Value[2], Context),
            QuantizedNumber(Value[3], Context)});
    }
    case eFbxString:
        return MakeShared<FJsonValueString>(FromFbx(Property.Get<FbxString>().Buffer()));
    case eFbxTime:
        return QuantizedNumber(Property.Get<FbxTime>().GetSecondDouble(), Context);
    default:
        return MakeShared<FJsonValueNull>();
    }
}

bool Utf8OrdinalLess(const FString& Left, const FString& Right)
{
    const FTCHARToUTF8 LeftUtf8(*Left);
    const FTCHARToUTF8 RightUtf8(*Right);
    const int32 CommonLength = FMath::Min(LeftUtf8.Length(), RightUtf8.Length());
    const int32 Comparison = CommonLength > 0
        ? FMemory::Memcmp(LeftUtf8.Get(), RightUtf8.Get(), CommonLength)
        : 0;
    return Comparison < 0 || (Comparison == 0 && LeftUtf8.Length() < RightUtf8.Length());
}

bool HasPassportSchema(const FbxProperty& Property)
{
    if (Property.GetPropertyDataType().GetType() != eFbxString)
    {
        return false;
    }

    const FString RawValue = FromFbx(Property.Get<FbxString>().Buffer());
    const FTCHARToUTF8 Utf8(*RawValue);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

    TSharedPtr<FJsonValue> Parsed;
    const UE::MimirComposite::FMHCanonicalResult ParseResult =
        UE::MimirComposite::MHParseJsonUtf8(Bytes, Parsed);
    if (!ParseResult.bSuccess || !Parsed.IsValid() || Parsed->Type != EJson::Object)
    {
        return false;
    }

    FString Schema;
    return Parsed->AsObject()->TryGetStringField(TEXT("schema"), Schema)
        && Schema == TEXT("mh.fbx_passport");
}

void CollectProperties(
    FbxNode* Node,
    FDumpContext& Context,
    TArray<TSharedPtr<FJsonValue>>& OutProperties,
    FLodLevel& OutLod,
    bool& bOutPassportPresent)
{
    struct FPropertyEntry
    {
        FString Name;
        FbxProperty Property;
    };

    TArray<FPropertyEntry> Properties;
    for (FbxProperty Property = Node->GetFirstProperty(); Property.IsValid();
         Property = Node->GetNextProperty(Property))
    {
        const FString RawName = FromFbx(Property.GetName());
        if (RawName.StartsWith(TEXT("mh_")))
        {
            FString Name;
            const UE::MimirComposite::FMHCanonicalResult NormalizeResult =
                UE::MimirComposite::MHNormalizeNfc(RawName, Name);
            if (!NormalizeResult.bSuccess)
            {
                Context.Error = NormalizeResult.Error;
                return;
            }
            Properties.Add({MoveTemp(Name), Property});
        }
    }
    Properties.Sort([](const FPropertyEntry& Left, const FPropertyEntry& Right)
    {
        return Utf8OrdinalLess(Left.Name, Right.Name);
    });

    for (const FPropertyEntry& Entry : Properties)
    {
        const EFbxType Type = Entry.Property.GetPropertyDataType().GetType();
        TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
        PropertyObject->SetStringField(TEXT("name"), Entry.Name);
        PropertyObject->SetStringField(TEXT("fbx_type"), PropertyTypeName(Type));
        PropertyObject->SetBoolField(
            TEXT("user_defined"),
            Entry.Property.GetFlag(FbxPropertyFlags::eUserDefined));
        const TSharedPtr<FJsonValue> Value = PropertyValue(Entry.Property, Context);
        PropertyObject->SetField(TEXT("value"), Value);
        OutProperties.Add(MakeShared<FJsonValueObject>(PropertyObject));

        if (Entry.Name == TEXT("mh_lod_level"))
        {
            OutLod.bExplicit = true;
            OutLod.FbxType = PropertyTypeName(Type);
            OutLod.RawValue = Value;

            const int64 LodValue = Type == eFbxInt ? Entry.Property.Get<FbxInt>() : -1;
            OutLod.bValid = Type == eFbxInt && LodValue >= 0;
            if (OutLod.bValid)
            {
                OutLod.Value = static_cast<int32>(LodValue);
            }
        }

        // TODO(QUESTION-13): the passport property key is provisional. It is
        // exposed by fbxdump for diagnostics only and has no mapper authority.
        if (Entry.Name == TEXT("mh_fbx_passport") || HasPassportSchema(Entry.Property))
        {
            bOutPassportPresent = true;
        }
    }
}

FString AttributeTypeName(const FbxNodeAttribute::EType Type)
{
    switch (Type)
    {
    case FbxNodeAttribute::eNull: return TEXT("null");
    case FbxNodeAttribute::eMarker: return TEXT("marker");
    case FbxNodeAttribute::eSkeleton: return TEXT("skeleton");
    case FbxNodeAttribute::eMesh: return TEXT("mesh");
    case FbxNodeAttribute::eNurbs: return TEXT("nurbs");
    case FbxNodeAttribute::ePatch: return TEXT("patch");
    case FbxNodeAttribute::eCamera: return TEXT("camera");
    case FbxNodeAttribute::eCameraStereo: return TEXT("camera_stereo");
    case FbxNodeAttribute::eCameraSwitcher: return TEXT("camera_switcher");
    case FbxNodeAttribute::eLight: return TEXT("light");
    case FbxNodeAttribute::eOpticalReference: return TEXT("optical_reference");
    case FbxNodeAttribute::eOpticalMarker: return TEXT("optical_marker");
    case FbxNodeAttribute::eNurbsCurve: return TEXT("nurbs_curve");
    case FbxNodeAttribute::eTrimNurbsSurface: return TEXT("trim_nurbs_surface");
    case FbxNodeAttribute::eBoundary: return TEXT("boundary");
    case FbxNodeAttribute::eNurbsSurface: return TEXT("nurbs_surface");
    case FbxNodeAttribute::eShape: return TEXT("shape");
    case FbxNodeAttribute::eLODGroup: return TEXT("lod_group");
    case FbxNodeAttribute::eSubDiv: return TEXT("subdiv");
    case FbxNodeAttribute::eCachedEffect: return TEXT("cached_effect");
    case FbxNodeAttribute::eLine: return TEXT("line");
    case FbxNodeAttribute::eUnknown:
    default:
        return TEXT("unknown");
    }
}

int32 PolygonMaterialIndex(FbxMesh* Mesh, const int32 PolygonIndex)
{
    FbxGeometryElementMaterial* MaterialElement = Mesh->GetElementMaterial(0);
    if (MaterialElement == nullptr)
    {
        return -1;
    }

    const FbxLayerElementArrayTemplate<int>& Indices = MaterialElement->GetIndexArray();
    switch (MaterialElement->GetMappingMode())
    {
    case FbxLayerElement::eAllSame:
        return Indices.GetCount() > 0 ? Indices.GetAt(0) : -1;
    case FbxLayerElement::eByPolygon:
        return PolygonIndex < Indices.GetCount() ? Indices.GetAt(PolygonIndex) : -1;
    default:
        return -1;
    }
}

TSharedRef<FJsonObject> BuildMeshObject(FbxNode* Node, FbxMesh* Mesh, FDumpContext& Context)
{
    TSharedRef<FJsonObject> MeshObject = MakeShared<FJsonObject>();
    const int32 ControlPointCount = Mesh->GetControlPointsCount();
    const int32 PolygonCount = Mesh->GetPolygonCount();
    const int32 PolygonVertexCount = Mesh->GetPolygonVertexCount();
    MeshObject->SetField(TEXT("control_point_count"), JsonInteger(ControlPointCount));
    MeshObject->SetField(TEXT("polygon_count"), JsonInteger(PolygonCount));
    MeshObject->SetField(TEXT("polygon_vertex_count"), JsonInteger(PolygonVertexCount));

    TArray<TSharedPtr<FJsonValue>> MaterialSlots;
    for (int32 MaterialIndex = 0; MaterialIndex < Node->GetMaterialCount(); ++MaterialIndex)
    {
        TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
        Slot->SetField(TEXT("index"), JsonInteger(MaterialIndex));
        const FbxSurfaceMaterial* Material = Node->GetMaterial(MaterialIndex);
        Slot->SetStringField(TEXT("name"), Material != nullptr ? FromFbx(Material->GetName()) : FString());
        MaterialSlots.Add(MakeShared<FJsonValueObject>(Slot));
    }
    MeshObject->SetArrayField(TEXT("material_slots"), MoveTemp(MaterialSlots));

    if (Context.bFull)
    {
        TArray<TSharedPtr<FJsonValue>> ControlPoints;
        const FbxVector4* RawControlPoints = Mesh->GetControlPoints();
        for (int32 PointIndex = 0; PointIndex < ControlPointCount; ++PointIndex)
        {
            ControlPoints.Add(MakeShared<FJsonValueArray>(
                QuantizedVector(RawControlPoints[PointIndex], Context)));
        }
        MeshObject->SetArrayField(TEXT("control_points_q6"), MoveTemp(ControlPoints));

        TArray<TSharedPtr<FJsonValue>> Polygons;
        for (int32 PolygonIndex = 0; PolygonIndex < PolygonCount; ++PolygonIndex)
        {
            TSharedRef<FJsonObject> Polygon = MakeShared<FJsonObject>();
            Polygon->SetField(
                TEXT("material_index"),
                JsonInteger(PolygonMaterialIndex(Mesh, PolygonIndex)));
            TArray<TSharedPtr<FJsonValue>> Indices;
            for (int32 CornerIndex = 0; CornerIndex < Mesh->GetPolygonSize(PolygonIndex); ++CornerIndex)
            {
                Indices.Add(JsonInteger(Mesh->GetPolygonVertex(PolygonIndex, CornerIndex)));
            }
            Polygon->SetArrayField(TEXT("control_point_indices"), MoveTemp(Indices));
            Polygons.Add(MakeShared<FJsonValueObject>(Polygon));
        }
        MeshObject->SetArrayField(TEXT("polygons"), MoveTemp(Polygons));
    }

    Context.Summary.MeshNodeCount += 1;
    Context.Summary.ControlPointCount += ControlPointCount;
    Context.Summary.PolygonCount += PolygonCount;
    Context.Summary.PolygonVertexCount += PolygonVertexCount;
    return MeshObject;
}

int32 BuildNode(FbxNode* Node, const int32 ParentIndex, FDumpContext& Context)
{
    const int32 NodeIndex = Context.Nodes.Num();
    Context.Nodes.Add(MakeShared<FJsonValueNull>());
    Context.Summary.NodeCount += 1;

    TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
    NodeObject->SetField(TEXT("index"), JsonInteger(NodeIndex));
    NodeObject->SetField(TEXT("parent_index"), JsonInteger(ParentIndex));
    NodeObject->SetStringField(TEXT("name"), FromFbx(Node->GetName()));

    TArray<TSharedPtr<FJsonValue>> AttributeTypes;
    for (int32 AttributeIndex = 0; AttributeIndex < Node->GetNodeAttributeCount(); ++AttributeIndex)
    {
        const FbxNodeAttribute* Attribute = Node->GetNodeAttributeByIndex(AttributeIndex);
        if (Attribute != nullptr)
        {
            AttributeTypes.Add(MakeShared<FJsonValueString>(AttributeTypeName(Attribute->GetAttributeType())));
        }
    }
    NodeObject->SetArrayField(TEXT("attribute_types"), MoveTemp(AttributeTypes));

    NodeObject->SetArrayField(
        TEXT("local_translation_q6"),
        QuantizedVector(Node->LclTranslation.Get(), Context));
    NodeObject->SetArrayField(
        TEXT("local_rotation_q6"),
        QuantizedVector(Node->LclRotation.Get(), Context));
    NodeObject->SetArrayField(
        TEXT("local_scaling_q6"),
        QuantizedVector(Node->LclScaling.Get(), Context));

    FLodLevel Lod;
    bool bPassportPresent = false;
    TArray<TSharedPtr<FJsonValue>> Properties;
    CollectProperties(Node, Context, Properties, Lod, bPassportPresent);
    NodeObject->SetArrayField(TEXT("custom_properties"), MoveTemp(Properties));
    NodeObject->SetBoolField(TEXT("mh_fbx_passport_present"), bPassportPresent);

    TSharedRef<FJsonObject> LodObject = MakeShared<FJsonObject>();
    LodObject->SetBoolField(TEXT("explicit"), Lod.bExplicit);
    LodObject->SetBoolField(TEXT("valid"), Lod.bValid);
    LodObject->SetStringField(TEXT("fbx_type"), Lod.FbxType);
    LodObject->SetField(TEXT("value"), Lod.RawValue);
    LodObject->SetField(
        TEXT("effective"),
        Lod.bValid ? JsonInteger(Lod.Value) : MakeShared<FJsonValueNull>());
    NodeObject->SetObjectField(TEXT("mh_lod_level"), LodObject);

    if (FbxMesh* Mesh = Node->GetMesh())
    {
        NodeObject->SetObjectField(TEXT("mesh"), BuildMeshObject(Node, Mesh, Context));
        if (Lod.bValid)
        {
            Context.Summary.MeshNodesPerLod.FindOrAdd(Lod.Value) += 1;
        }
        else
        {
            Context.Summary.InvalidLodMeshNodeCount += 1;
        }
    }

    TArray<TSharedPtr<FJsonValue>> ChildIndices;
    for (int32 ChildIndex = 0; ChildIndex < Node->GetChildCount(); ++ChildIndex)
    {
        ChildIndices.Add(JsonInteger(BuildNode(Node->GetChild(ChildIndex), NodeIndex, Context)));
    }
    NodeObject->SetArrayField(TEXT("child_indices"), MoveTemp(ChildIndices));

    Context.Nodes[NodeIndex] = MakeShared<FJsonValueObject>(NodeObject);
    return NodeIndex;
}

FString AxisName(const FbxAxisSystem::EUpVector Axis)
{
    switch (Axis)
    {
    case FbxAxisSystem::eXAxis: return TEXT("X");
    case FbxAxisSystem::eYAxis: return TEXT("Y");
    case FbxAxisSystem::eZAxis: return TEXT("Z");
    default: return TEXT("unknown");
    }
}

FString FrontParityName(const FbxAxisSystem::EFrontVector Front)
{
    switch (Front)
    {
    case FbxAxisSystem::eParityEven: return TEXT("parity_even");
    case FbxAxisSystem::eParityOdd: return TEXT("parity_odd");
    default: return TEXT("unknown");
    }
}

FString HandednessName(const FbxAxisSystem::ECoordSystem CoordinateSystem)
{
    switch (CoordinateSystem)
    {
    case FbxAxisSystem::eRightHanded: return TEXT("right");
    case FbxAxisSystem::eLeftHanded: return TEXT("left");
    default: return TEXT("unknown");
    }
}

TSharedRef<FJsonObject> BuildSceneMetadata(
    FbxScene* Scene,
    const FString& HeaderCreator,
    const int32 FileMajor,
    const int32 FileMinor,
    const int32 FileRevision,
    FDumpContext& Context)
{
    TSharedRef<FJsonObject> SceneObject = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Version = MakeShared<FJsonObject>();
    Version->SetField(TEXT("major"), JsonInteger(FileMajor));
    Version->SetField(TEXT("minor"), JsonInteger(FileMinor));
    Version->SetField(TEXT("revision"), JsonInteger(FileRevision));
    SceneObject->SetObjectField(TEXT("file_version"), Version);

    const FbxAxisSystem AxisSystem = Scene->GetGlobalSettings().GetAxisSystem();
    int UpSign = 0;
    int FrontSign = 0;
    const FbxAxisSystem::EUpVector Up = AxisSystem.GetUpVector(UpSign);
    const FbxAxisSystem::EFrontVector Front = AxisSystem.GetFrontVector(FrontSign);
    TSharedRef<FJsonObject> Axis = MakeShared<FJsonObject>();
    Axis->SetStringField(TEXT("up"), AxisName(Up));
    Axis->SetField(TEXT("up_sign"), JsonInteger(UpSign));
    Axis->SetStringField(TEXT("front"), FrontParityName(Front));
    Axis->SetField(TEXT("front_sign"), JsonInteger(FrontSign));
    Axis->SetStringField(TEXT("handedness"), HandednessName(AxisSystem.GetCoorSystem()));
    SceneObject->SetObjectField(TEXT("axis"), Axis);

    const FbxSystemUnit Unit = Scene->GetGlobalSettings().GetSystemUnit();
    TSharedRef<FJsonObject> UnitObject = MakeShared<FJsonObject>();
    UnitObject->SetStringField(TEXT("name"), FromFbx(Unit.GetScaleFactorAsString(false).Buffer()));
    UnitObject->SetStringField(TEXT("abbreviation"), FromFbx(Unit.GetScaleFactorAsString(true).Buffer()));
    UnitObject->SetField(TEXT("scale_factor_q6"), QuantizedNumber(Unit.GetScaleFactor(), Context));
    UnitObject->SetField(TEXT("multiplier_q6"), QuantizedNumber(Unit.GetMultiplier(), Context));
    SceneObject->SetObjectField(TEXT("unit"), UnitObject);

    TSharedRef<FJsonObject> Exporter = MakeShared<FJsonObject>();
    Exporter->SetStringField(TEXT("creator"), HeaderCreator);
    if (FbxDocumentInfo* Info = Scene->GetSceneInfo())
    {
        Exporter->SetStringField(
            TEXT("original_vendor"),
            FromFbx(Info->Original_ApplicationVendor.Get().Buffer()));
        Exporter->SetStringField(
            TEXT("original_name"),
            FromFbx(Info->Original_ApplicationName.Get().Buffer()));
        Exporter->SetStringField(
            TEXT("original_version"),
            FromFbx(Info->Original_ApplicationVersion.Get().Buffer()));
        Exporter->SetStringField(
            TEXT("last_saved_vendor"),
            FromFbx(Info->LastSaved_ApplicationVendor.Get().Buffer()));
        Exporter->SetStringField(
            TEXT("last_saved_name"),
            FromFbx(Info->LastSaved_ApplicationName.Get().Buffer()));
        Exporter->SetStringField(
            TEXT("last_saved_version"),
            FromFbx(Info->LastSaved_ApplicationVersion.Get().Buffer()));
    }
    else
    {
        Exporter->SetStringField(TEXT("original_vendor"), FString());
        Exporter->SetStringField(TEXT("original_name"), FString());
        Exporter->SetStringField(TEXT("original_version"), FString());
        Exporter->SetStringField(TEXT("last_saved_vendor"), FString());
        Exporter->SetStringField(TEXT("last_saved_name"), FString());
        Exporter->SetStringField(TEXT("last_saved_version"), FString());
    }
    SceneObject->SetObjectField(TEXT("exporter"), Exporter);
    return SceneObject;
}

TSharedRef<FJsonObject> BuildSummary(const FDumpSummary& Summary)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetField(TEXT("node_count"), JsonInteger(Summary.NodeCount));
    Object->SetField(TEXT("mesh_node_count"), JsonInteger(Summary.MeshNodeCount));
    Object->SetField(TEXT("control_point_count"), JsonInteger(Summary.ControlPointCount));
    Object->SetField(TEXT("polygon_count"), JsonInteger(Summary.PolygonCount));
    Object->SetField(TEXT("polygon_vertex_count"), JsonInteger(Summary.PolygonVertexCount));
    Object->SetField(
        TEXT("invalid_lod_mesh_node_count"),
        JsonInteger(Summary.InvalidLodMeshNodeCount));

    TArray<int32> LodLevels;
    Summary.MeshNodesPerLod.GetKeys(LodLevels);
    LodLevels.Sort();
    TArray<TSharedPtr<FJsonValue>> LodLevelValues;
    TArray<TSharedPtr<FJsonValue>> MeshNodesPerLod;
    for (const int32 Level : LodLevels)
    {
        LodLevelValues.Add(JsonInteger(Level));
        TSharedRef<FJsonObject> LodCount = MakeShared<FJsonObject>();
        LodCount->SetField(TEXT("lod_level"), JsonInteger(Level));
        LodCount->SetField(
            TEXT("mesh_node_count"),
            JsonInteger(Summary.MeshNodesPerLod.FindChecked(Level)));
        MeshNodesPerLod.Add(MakeShared<FJsonValueObject>(LodCount));
    }
    Object->SetArrayField(TEXT("lod_levels"), MoveTemp(LodLevelValues));
    Object->SetArrayField(TEXT("mesh_nodes_per_lod"), MoveTemp(MeshNodesPerLod));
    return Object;
}
}

bool MH::FbxDump::BuildDom(
    const FString& FilePath,
    const bool bFull,
    TSharedPtr<FJsonObject>& OutRoot,
    FString& OutError)
{
    OutRoot.Reset();
    OutError.Reset();
    if (!FPaths::FileExists(FilePath))
    {
        OutError = FString::Printf(TEXT("MH_E_FBX_DUMP_FILE_NOT_FOUND: %s"), *FilePath);
        return false;
    }

    FbxManager* Manager = FbxManager::Create();
    if (Manager == nullptr)
    {
        OutError = TEXT("MH_E_FBX_DUMP_OPEN_FAILED: unable to create FBX manager");
        return false;
    }

    FbxIOSettings* IoSettings = FbxIOSettings::Create(Manager, IOSROOT);
    Manager->SetIOSettings(IoSettings);
    FbxImporter* Importer = FbxImporter::Create(Manager, "MHFbxDumpImporter");
    if (!Importer->Initialize(TCHAR_TO_UTF8(*FilePath), -1, IoSettings))
    {
        OutError = FString::Printf(
            TEXT("MH_E_FBX_DUMP_OPEN_FAILED: %s"),
            UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString()));
        Manager->Destroy();
        return false;
    }

    int FileMajor = 0;
    int FileMinor = 0;
    int FileRevision = 0;
    Importer->GetFileVersion(FileMajor, FileMinor, FileRevision);
    FString HeaderCreator;
    if (const FbxIOFileHeaderInfo* Header = Importer->GetFileHeaderInfo())
    {
        HeaderCreator = FromFbx(Header->mCreator.Buffer());
    }

    FbxScene* Scene = FbxScene::Create(Manager, "MHFbxDumpScene");
    if (Scene == nullptr)
    {
        OutError = TEXT("MH_E_FBX_DUMP_OPEN_FAILED: unable to create FBX scene");
        Manager->Destroy();
        return false;
    }
    if (!Importer->Import(Scene))
    {
        OutError = FString::Printf(
            TEXT("MH_E_FBX_DUMP_OPEN_FAILED: %s"),
            UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString()));
        Manager->Destroy();
        return false;
    }
    Importer->Destroy();

    FDumpContext Context;
    Context.bFull = bFull;
    if (FbxNode* SyntheticRoot = Scene->GetRootNode())
    {
        for (int32 ChildIndex = 0; ChildIndex < SyntheticRoot->GetChildCount(); ++ChildIndex)
        {
            BuildNode(SyntheticRoot->GetChild(ChildIndex), -1, Context);
        }
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("tag"), TEXT("mh.fbxdump:1"));
    Root->SetBoolField(TEXT("full"), bFull);
    Root->SetField(TEXT("quantization_digits"), JsonInteger(QuantizationDigits));
    Root->SetObjectField(
        TEXT("scene"),
        BuildSceneMetadata(Scene, HeaderCreator, FileMajor, FileMinor, FileRevision, Context));
    Root->SetObjectField(TEXT("summary"), BuildSummary(Context.Summary));
    Root->SetArrayField(TEXT("nodes"), MoveTemp(Context.Nodes));

    Manager->Destroy();
    if (!Context.Error.IsEmpty())
    {
        OutError = MoveTemp(Context.Error);
        return false;
    }

    OutRoot = Root;
    return true;
}

bool MH::FbxDump::SerializeCanonical(
    const TSharedPtr<FJsonObject>& Root,
    FString& OutJson,
    FString& OutError)
{
    OutJson.Reset();
    OutError.Reset();
    TArray<uint8> Bytes;
    const UE::MimirComposite::FMHCanonicalResult Result =
        UE::MimirComposite::MHCanonicalJsonBytes(MakeShared<FJsonValueObject>(Root), Bytes);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    OutJson = FString(Converted.Length(), Converted.Get());
    return true;
}
