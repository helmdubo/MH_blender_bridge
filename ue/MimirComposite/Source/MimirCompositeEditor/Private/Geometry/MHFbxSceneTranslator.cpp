#include "Geometry/MHFbxSceneTranslator.h"

#include "Geometry/MHSceneIR.h"

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace UE::MimirComposite
{
namespace
{

class FMHFbxMemoryStream final : public FbxStream
{
public:
    FMHFbxMemoryStream(TConstArrayView<uint8> InBytes, const int32 InReaderId)
        : Bytes(InBytes)
        , ReaderId(InReaderId)
    {
    }

    virtual EState GetState() override
    {
        return bOpen ? (Bytes.IsEmpty() ? eEmpty : eOpen) : eClosed;
    }

    virtual bool Open(void*) override
    {
        Position = 0;
        Error = 0;
        bOpen = true;
        return true;
    }

    virtual bool Close() override
    {
        Position = 0;
        bOpen = false;
        return true;
    }

    virtual bool Flush() override { return true; }

    virtual int Write(const void*, int) override
    {
        Error = 1;
        return 0;
    }

    virtual int Read(void* Data, const int Size) const override
    {
        if (!bOpen || Data == nullptr || Size < 0)
        {
            Error = 1;
            return 0;
        }
        const int64 Remaining = Bytes.Num() - Position;
        const int32 ToRead = static_cast<int32>(FMath::Min<int64>(Size, FMath::Max<int64>(0, Remaining)));
        if (ToRead > 0)
        {
            FMemory::Memcpy(Data, Bytes.GetData() + Position, ToRead);
            Position += ToRead;
        }
        return ToRead;
    }

    virtual int GetReaderID() const override { return ReaderId; }
    virtual int GetWriterID() const override { return -1; }

    virtual void Seek(const FbxInt64& Offset, const FbxFile::ESeekPos& SeekPos) override
    {
        int64 Base = 0;
        switch (SeekPos)
        {
        case FbxFile::eCurrent: Base = Position; break;
        case FbxFile::eEnd: Base = Bytes.Num(); break;
        case FbxFile::eBegin:
        default: break;
        }
        const int64 Requested = Base + static_cast<int64>(Offset);
        if (Requested < 0 || Requested > Bytes.Num())
        {
            Error = 1;
            Position = FMath::Clamp<int64>(Requested, 0, Bytes.Num());
            return;
        }
        Position = Requested;
    }

    virtual long GetPosition() const override { return static_cast<long>(Position); }

    virtual void SetPosition(const long InPosition) override
    {
        Seek(static_cast<FbxInt64>(InPosition), FbxFile::eBegin);
    }

    virtual int GetError() const override { return Error; }
    virtual void ClearError() override { Error = 0; }

private:
    TConstArrayView<uint8> Bytes;
    int32 ReaderId = -1;
    mutable int64 Position = 0;
    mutable int32 Error = 0;
    bool bOpen = false;
};

FString TransportError(const FString& Detail)
{
    return FString::Printf(TEXT("MH_E_FBX_TRANSPORT_FAILED: %s"), *Detail);
}

FString MarkerError(const FString& Detail)
{
    return FString::Printf(TEXT("MH_E_INVALID_NODE_MARKERS: %s"), *Detail);
}

/**
 * Read one Blender `use_custom_props` string carried on the FBX Model node.
 * Blender writes object ID properties as user-defined FbxString properties, so
 * only that exact shape is admitted; anything else is reported as absent.
 */
bool ReadNodeStringProperty(FbxNode& Node, const char* PropertyName, FString& OutValue)
{
    OutValue.Reset();
    FbxProperty Property = Node.FindProperty(PropertyName, false);
    if (!Property.IsValid() || Property.GetPropertyDataType().GetType() != eFbxString)
    {
        return false;
    }
    const FbxString Value = Property.Get<FbxString>();
    OutValue = UTF8_TO_TCHAR(Value.Buffer());
    OutValue.TrimStartAndEndInline();
    return !OutValue.IsEmpty();
}

/** Apply the S6.1.2 carrier contract (doc 15 section 3.4) to one Model node. */
bool ReadCollisionCarrier(FbxNode& Node, FMHSceneIRNode& OutNode, FString& OutError)
{
    FString Carrier;
    if (!ReadNodeStringProperty(Node, "mh_collision", Carrier))
    {
        return true;
    }
    if (Carrier.Equals(TEXT("phys"), ESearchCase::IgnoreCase))
    {
        OutNode.CollisionCarrier = EMHSceneCollisionCarrier::Phys;
    }
    else if (Carrier.Equals(TEXT("trace"), ESearchCase::IgnoreCase))
    {
        OutNode.CollisionCarrier = EMHSceneCollisionCarrier::Trace;
    }
    else
    {
        OutError = MarkerError(FString::Printf(
            TEXT("node '%s' has an unknown mh_collision value '%s'"), *OutNode.Name, *Carrier));
        return false;
    }

    FString Shape;
    if (ReadNodeStringProperty(Node, "mh_collision_shape", Shape))
    {
        if (Shape.Equals(TEXT("mesh"), ESearchCase::IgnoreCase))
        {
            OutNode.CollisionShape = EMHSceneCollisionShape::Mesh;
        }
        else if (Shape.Equals(TEXT("box"), ESearchCase::IgnoreCase))
        {
            OutNode.CollisionShape = EMHSceneCollisionShape::Box;
        }
        else if (Shape.Equals(TEXT("convex"), ESearchCase::IgnoreCase))
        {
            OutNode.CollisionShape = EMHSceneCollisionShape::Convex;
        }
        else if (Shape.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
        {
            OutNode.CollisionShape = EMHSceneCollisionShape::Capsule;
        }
        else
        {
            OutError = MarkerError(FString::Printf(
                TEXT("node '%s' has an unknown mh_collision_shape value '%s'"), *OutNode.Name, *Shape));
            return false;
        }
    }

    // A node exported before the dag4blend overlay patch simply carries no
    // phmat; that is a warning at import time, never a transport failure.
    ReadNodeStringProperty(Node, "mh_phmat", OutNode.PhysicalMaterialToken);
    return true;
}

FVector3f FbxSceneToUnrealPosition(const FbxVector4& Position)
{
    return FVector3f(
        static_cast<float>(Position[0]),
        static_cast<float>(-Position[1]),
        static_cast<float>(Position[2]));
}

FVector3f ToUnrealNormal(const FbxVector4& Normal)
{
    return FbxSceneToUnrealPosition(Normal).GetSafeNormal();
}

FbxAMatrix GeometryTransform(const FbxNode& Node)
{
    FbxAMatrix Result;
    Result.SetT(Node.GetGeometricTranslation(FbxNode::eSourcePivot));
    Result.SetR(Node.GetGeometricRotation(FbxNode::eSourcePivot));
    Result.SetS(Node.GetGeometricScaling(FbxNode::eSourcePivot));
    return Result;
}

FbxAMatrix CanonicalAxisUnwind()
{
    // Blender's canonical axis_forward='X' export composes a RotZ(-90) axis
    // conversion into every root node transform while control points stay in
    // raw Blender coordinates. The ratified R1 transport (axis_probe fixture,
    // MHAxisProbeTest) bakes raw points plus the Y reflection only, so the
    // conversion must be unwound before node transforms are baked into
    // geometry - otherwise every mesh arrives rotated 90 degrees against its
    // composite node TRS (field defect 2026-08-31).
    FbxAMatrix Result;
    Result.SetR(FbxVector4(0.0, 0.0, 90.0));
    return Result;
}

FTransform ToUnrealTransform(const FbxAMatrix& Matrix, bool& bOutRepresentableAsTRS)
{
    const FbxVector4 OriginFbx = Matrix.MultT(FbxVector4(0.0, 0.0, 0.0, 1.0));
    const FVector3f Origin = FbxSceneToUnrealPosition(OriginFbx);
    const FVector3f X = FbxSceneToUnrealPosition(Matrix.MultT(FbxVector4(1.0, 0.0, 0.0, 1.0))) - Origin;
    // UE local +Y corresponds to FBX local -Y at the pinned reflection seam.
    const FVector3f Y = FbxSceneToUnrealPosition(Matrix.MultT(FbxVector4(0.0, -1.0, 0.0, 1.0))) - Origin;
    const FVector3f Z = FbxSceneToUnrealPosition(Matrix.MultT(FbxVector4(0.0, 0.0, 1.0, 1.0))) - Origin;
    bOutRepresentableAsTRS = !Origin.ContainsNaN() && !X.ContainsNaN() && !Y.ContainsNaN() && !Z.ContainsNaN() &&
        !X.IsNearlyZero() && !Y.IsNearlyZero() && !Z.IsNearlyZero();
    if (bOutRepresentableAsTRS)
    {
        const FVector3f NormalizedX = X.GetSafeNormal();
        const FVector3f NormalizedY = Y.GetSafeNormal();
        const FVector3f NormalizedZ = Z.GetSafeNormal();
        constexpr float OrthogonalityTolerance = 1.0e-4f;
        bOutRepresentableAsTRS =
            FMath::Abs(FVector3f::DotProduct(NormalizedX, NormalizedY)) <= OrthogonalityTolerance &&
            FMath::Abs(FVector3f::DotProduct(NormalizedX, NormalizedZ)) <= OrthogonalityTolerance &&
            FMath::Abs(FVector3f::DotProduct(NormalizedY, NormalizedZ)) <= OrthogonalityTolerance;
    }
    const FMatrix UnrealMatrix(
        FPlane(FVector(X), 0.0),
        FPlane(FVector(Y), 0.0),
        FPlane(FVector(Z), 0.0),
        FPlane(FVector(Origin), 1.0));
    return FTransform(UnrealMatrix);
}

int32 PolygonMaterialIndex(const FbxMesh& Mesh, const int32 PolygonIndex, FString& OutError)
{
    const FbxLayerElementMaterial* Layer = Mesh.GetElementMaterial();
    if (Layer == nullptr)
    {
        return INDEX_NONE;
    }
    int32 Lookup = INDEX_NONE;
    switch (Layer->GetMappingMode())
    {
    case FbxLayerElement::eAllSame: Lookup = 0; break;
    case FbxLayerElement::eByPolygon: Lookup = PolygonIndex; break;
    default:
        OutError = TransportError(TEXT("material layer must map all-same or by-polygon"));
        return INDEX_NONE;
    }
    const auto& Indices = Layer->GetIndexArray();
    if (Lookup < 0 || Lookup >= Indices.GetCount())
    {
        OutError = TransportError(FString::Printf(
            TEXT("polygon %d has an invalid material-layer index"), PolygonIndex));
        return INDEX_NONE;
    }
    return Indices.GetAt(Lookup);
}

bool ReadGeometry(
    FbxNode& Node,
    FMHSceneIRNode& OutNode,
    FString& OutError)
{
    FbxMesh* Mesh = Node.GetMesh();
    if (Mesh == nullptr || Mesh->GetControlPointsCount() <= 0 || Mesh->GetPolygonCount() <= 0)
    {
        OutError = TransportError(FString::Printf(
            TEXT("mesh node '%s' has no usable geometry"), UTF8_TO_TCHAR(Node.GetName())));
        return false;
    }
    const FbxAMatrix GlobalGeometry =
        CanonicalAxisUnwind() * Node.EvaluateGlobalTransform() * GeometryTransform(Node);
    const double GeometryDeterminant = GlobalGeometry.Determinant();
    if (!FMath::IsFinite(GeometryDeterminant) || FMath::IsNearlyZero(GeometryDeterminant))
    {
        OutError = TransportError(FString::Printf(
            TEXT("mesh node '%s' has a singular geometry transform"),
            UTF8_TO_TCHAR(Node.GetName())));
        return false;
    }
    // Match the stock UFbxFactory path exactly: ConvertPos performs the same
    // right-handed FBX -> left-handed UE reflection while preserving the FBX
    // polygon corner order. Reversing the perimeter a second time turns every
    // ordinary Blender face inside-out.
    OutNode.bReverseWinding = false;
    const FbxAMatrix NormalMatrix = GlobalGeometry.Inverse().Transpose();
    FMHSceneGeometry Geometry;
    Geometry.Positions.Reserve(Mesh->GetControlPointsCount());
    for (int32 PointIndex = 0; PointIndex < Mesh->GetControlPointsCount(); ++PointIndex)
    {
        Geometry.Positions.Add(FbxSceneToUnrealPosition(
            GlobalGeometry.MultT(Mesh->GetControlPointAt(PointIndex))));
    }

    FbxStringList UVSetNames;
    Mesh->GetUVSetNames(UVSetNames);
    const char* UVSetName = UVSetNames.GetCount() > 0 ? UVSetNames.GetStringAt(0) : nullptr;
    const bool bHasNormalLayer = Mesh->GetElementNormalCount() > 0;
    Geometry.Triangles.Reserve(Mesh->GetPolygonCount());
    for (int32 PolygonIndex = 0; PolygonIndex < Mesh->GetPolygonCount(); ++PolygonIndex)
    {
        if (Mesh->GetPolygonSize(PolygonIndex) != 3)
        {
            OutError = TransportError(FString::Printf(
                TEXT("SDK triangulation left polygon %d with %d corners"),
                PolygonIndex,
                Mesh->GetPolygonSize(PolygonIndex)));
            return false;
        }
        FMHSceneTriangle& Triangle = Geometry.Triangles.AddDefaulted_GetRef();
        Triangle.MaterialSlotIndex = PolygonMaterialIndex(*Mesh, PolygonIndex, OutError);
        if (!OutError.IsEmpty())
        {
            return false;
        }
        if (Triangle.MaterialSlotIndex == INDEX_NONE && Node.GetMaterialCount() == 1)
        {
            Triangle.MaterialSlotIndex = 0;
        }
        if (Triangle.MaterialSlotIndex == INDEX_NONE && Node.GetMaterialCount() > 1)
        {
            OutError = TransportError(FString::Printf(
                TEXT("polygon %d has no determinate material slot on node '%s' with %d slots"),
                PolygonIndex,
                UTF8_TO_TCHAR(Node.GetName()),
                Node.GetMaterialCount()));
            return false;
        }
        if (Triangle.MaterialSlotIndex != INDEX_NONE &&
            (Triangle.MaterialSlotIndex < 0 || Triangle.MaterialSlotIndex >= Node.GetMaterialCount()))
        {
            OutError = TransportError(FString::Printf(
                TEXT("polygon %d references material slot %d outside node '%s'"),
                PolygonIndex,
                Triangle.MaterialSlotIndex,
                UTF8_TO_TCHAR(Node.GetName())));
            return false;
        }

        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const int32 PositionIndex = Mesh->GetPolygonVertex(PolygonIndex, Corner);
            if (!Geometry.Positions.IsValidIndex(PositionIndex))
            {
                OutError = TransportError(FString::Printf(
                    TEXT("polygon %d references control point %d outside geometry"),
                    PolygonIndex,
                    PositionIndex));
                return false;
            }
            Triangle.PositionIndices[Corner] = PositionIndex;

            FbxVector4 Normal;
            if (Mesh->GetPolygonVertexNormal(PolygonIndex, Corner, Normal))
            {
                // MultR transforms an Euler rotation vector, not an arbitrary
                // direction. Stock UE applies the inverse-transpose with
                // MultT before the FBX->UE basis conversion.
                Triangle.CornerNormals[Corner] = ToUnrealNormal(NormalMatrix.MultT(Normal));
            }
            else if (bHasNormalLayer)
            {
                OutError = TransportError(FString::Printf(
                    TEXT("polygon %d corner %d has an invalid normal-layer index"),
                    PolygonIndex,
                    Corner));
                return false;
            }
            if (UVSetName != nullptr)
            {
                FbxVector2 UV;
                bool bUnmapped = false;
                if (!Mesh->GetPolygonVertexUV(PolygonIndex, Corner, UVSetName, UV, bUnmapped))
                {
                    OutError = TransportError(FString::Printf(
                        TEXT("polygon %d corner %d has an invalid UV layer index"),
                        PolygonIndex,
                        Corner));
                    return false;
                }
                if (!bUnmapped)
                {
                    Triangle.CornerUV0[Corner] = FVector2f(
                        static_cast<float>(UV[0]),
                        static_cast<float>(1.0 - UV[1]));
                }
            }
        }
        if (Triangle.CornerNormals[0].IsNearlyZero() ||
            Triangle.CornerNormals[1].IsNearlyZero() ||
            Triangle.CornerNormals[2].IsNearlyZero())
        {
            const FVector3f A = Geometry.Positions[Triangle.PositionIndices[0]];
            const FVector3f B = Geometry.Positions[Triangle.PositionIndices[1]];
            const FVector3f C = Geometry.Positions[Triangle.PositionIndices[2]];
            // FMeshDescription's left-handed face convention is the reverse
            // cross used by the stock FBX importer/static-mesh operations.
            FVector3f FaceNormal = FVector3f::CrossProduct(C - A, B - A).GetSafeNormal();
            if (FaceNormal.IsNearlyZero())
            {
                OutError = TransportError(FString::Printf(
                    TEXT("polygon %d is degenerate"), PolygonIndex));
                return false;
            }
            for (FVector3f& CornerNormal : Triangle.CornerNormals)
            {
                if (CornerNormal.IsNearlyZero())
                {
                    CornerNormal = FaceNormal;
                }
            }
        }
    }
    OutNode.Geometry = MoveTemp(Geometry);
    return true;
}

bool AppendNodeRecursive(
    FbxNode& Node,
    const int32 ParentIndex,
    FMHSceneIR& OutScene,
    FString& OutError)
{
    FMHSceneIRNode NewNode;
    NewNode.Name = UTF8_TO_TCHAR(Node.GetName());
    NewNode.ParentIndex = ParentIndex;
    if (!ReadCollisionCarrier(Node, NewNode, OutError))
    {
        return false;
    }
    bool bGlobalTransformIsTRS = false;
    NewNode.GlobalTransform = ToUnrealTransform(
        CanonicalAxisUnwind() * Node.EvaluateGlobalTransform(), bGlobalTransformIsTRS);

    FbxNodeAttribute* Attribute = Node.GetNodeAttribute();
    if (Attribute == nullptr || Attribute->GetAttributeType() == FbxNodeAttribute::eNull)
    {
        NewNode.Attribute = EMHSceneNodeAttribute::Null;
        if (NewNode.Name.StartsWith(TEXT("SOCKET_"), ESearchCase::CaseSensitive) && !bGlobalTransformIsTRS)
        {
            OutError = TransportError(FString::Printf(
                TEXT("socket node '%s' global transform contains shear or a singular scale"),
                *NewNode.Name));
            return false;
        }
    }
    else if (Attribute->GetAttributeType() == FbxNodeAttribute::eMesh)
    {
        NewNode.Attribute = EMHSceneNodeAttribute::Mesh;
        NewNode.MaterialSlots.Reserve(Node.GetMaterialCount());
        for (int32 MaterialIndex = 0; MaterialIndex < Node.GetMaterialCount(); ++MaterialIndex)
        {
            FbxSurfaceMaterial* Material = Node.GetMaterial(MaterialIndex);
            const FString Name = Material != nullptr ? UTF8_TO_TCHAR(Material->GetName()) : FString();
            if (Name.IsEmpty())
            {
                OutError = TransportError(FString::Printf(
                    TEXT("mesh node '%s' has an empty material slot name"), *NewNode.Name));
                return false;
            }
            NewNode.MaterialSlots.Add(Name);
        }
        if (!ReadGeometry(Node, NewNode, OutError))
        {
            return false;
        }
    }
    else
    {
        NewNode.Attribute = EMHSceneNodeAttribute::Unsupported;
    }

    const int32 NewIndex = OutScene.Nodes.Add(MoveTemp(NewNode));
    for (int32 ChildIndex = 0; ChildIndex < Node.GetChildCount(); ++ChildIndex)
    {
        FbxNode* Child = Node.GetChild(ChildIndex);
        if (Child == nullptr || !AppendNodeRecursive(
                *Child,
                NewIndex,
                OutScene,
                OutError))
        {
            if (OutError.IsEmpty())
            {
                OutError = TransportError(TEXT("FBX scene contains a null child node"));
            }
            return false;
        }
    }
    return true;
}

} // namespace

bool FMHFbxSceneTranslator::Translate(
    const FString& ResourceName,
    const TConstArrayView<uint8> SourceBytes,
    FMHSceneIR& OutScene,
    FString& OutError)
{
    OutScene = FMHSceneIR();
    OutError.Reset();
    if (SourceBytes.IsEmpty())
    {
        OutError = TransportError(TEXT("FBX payload is empty"));
        return false;
    }

    FbxManager* Manager = FbxManager::Create();
    if (Manager == nullptr)
    {
        OutError = TransportError(TEXT("FbxManager::Create failed"));
        return false;
    }
    FbxIOSettings* IOSettings = FbxIOSettings::Create(Manager, IOSROOT);
    Manager->SetIOSettings(IOSettings);
    FbxImporter* Importer = FbxImporter::Create(Manager, "MHSceneIRImporter");
    FbxScene* Scene = FbxScene::Create(Manager, "MHSceneIRScene");
    const int32 ReaderId = Manager->GetIOPluginRegistry()->FindReaderIDByExtension("fbx");
    FMHFbxMemoryStream Stream(SourceBytes, ReaderId);
    if (!Importer->Initialize(&Stream, nullptr, ReaderId, IOSettings) || !Importer->Import(Scene))
    {
        OutError = TransportError(UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString()));
        Manager->Destroy();
        return false;
    }

    const FbxAxisSystem AxisSystem = Scene->GetGlobalSettings().GetAxisSystem();
    int AxisSign = 0;
    const FbxAxisSystem::EUpVector UpVector = AxisSystem.GetUpVector(AxisSign);
    int FrontSign = 0;
    const FbxAxisSystem::EFrontVector FrontVector = AxisSystem.GetFrontVector(FrontSign);
    const FbxSystemUnit Units = Scene->GetGlobalSettings().GetSystemUnit();
    if (UpVector != FbxAxisSystem::eZAxis || AxisSign != 1 ||
        FrontVector != FbxAxisSystem::eParityEven || FrontSign != -1 ||
        AxisSystem.GetCoorSystem() != FbxAxisSystem::eRightHanded ||
        Units != FbxSystemUnit::cm)
    {
        OutError = TransportError(TEXT("axis or units differ from canonical Blender FBX transport"));
        Manager->Destroy();
        return false;
    }

    FbxGeometryConverter Converter(Manager);
    if (!Converter.Triangulate(Scene, true))
    {
        OutError = TransportError(TEXT("FBX SDK scene triangulation failed"));
        Manager->Destroy();
        return false;
    }

    FbxNode* Root = Scene->GetRootNode();
    if (Root == nullptr)
    {
        OutError = TransportError(TEXT("FBX scene has no root node"));
        Manager->Destroy();
        return false;
    }
    OutScene.ResourceName = ResourceName;
    for (int32 ChildIndex = 0; ChildIndex < Root->GetChildCount(); ++ChildIndex)
    {
        FbxNode* Child = Root->GetChild(ChildIndex);
        if (Child == nullptr || !AppendNodeRecursive(
                *Child,
                INDEX_NONE,
                OutScene,
                OutError))
        {
            if (OutError.IsEmpty())
            {
                OutError = TransportError(TEXT("FBX root contains a null child node"));
            }
            Manager->Destroy();
            OutScene = FMHSceneIR();
            return false;
        }
    }
    Manager->Destroy();

    if (!MHClassifySceneIR(OutScene, OutError))
    {
        OutScene = FMHSceneIR();
        return false;
    }
    return true;
}

} // namespace UE::MimirComposite
