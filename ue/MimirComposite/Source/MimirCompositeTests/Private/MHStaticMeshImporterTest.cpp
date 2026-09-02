#include "StaticMesh/MHStaticMeshImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeDefinitionSubsystem.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Components/StaticMeshComponent.h"
#include "Diagnostics/MHSourceOperations.h"
#include "Editor.h"
#include "EditorReimportHandler.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "MHGoldenRoot.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Interface_CollisionDataProviderCore.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Performance/MHPerformanceTrace.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/ConvexElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "Source/MHSourceImportMetrics.h"
#include "Source/MHSourceResolver.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMeshAttributes.h"
#include "StaticMesh/MHStaticMeshReimportHandler.h"
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

class FStaticMeshImporterTestResolver final : public IMHSourceResolver
{
public:
    FString MaterialName;
    FString MaterialPath;
    FString MaterialHash;

    virtual FMHSourceSnapshot GetSnapshot() const override
    {
        FMHSourceSnapshot Snapshot;
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::Material;
        Key.LogicalName = MaterialName;
        Snapshot.ResourceKeys.Add(Key);
        return Snapshot;
    }

    virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) override
    {
        FMHResolveOutcome Outcome;
        if (Key.Kind == EMHResourceKind::Material && Key.LogicalName == MaterialName)
        {
            Outcome.Status = EMHResolveStatus::Resolved;
            Outcome.PayloadPath = MaterialPath;
            Outcome.RawHash = MaterialHash;
            Outcome.CandidatePaths.Add(MaterialPath);
        }
        return Outcome;
    }
};

/** Resolver for the LOD material union fixture, which needs several slots. */
class FStaticMeshUnionTestResolver final : public IMHSourceResolver
{
public:
    TMap<FString, TPair<FString, FString>> Materials;

    virtual FMHSourceSnapshot GetSnapshot() const override
    {
        FMHSourceSnapshot Snapshot;
        for (const TPair<FString, TPair<FString, FString>>& Entry : Materials)
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::Material;
            Key.LogicalName = Entry.Key;
            Snapshot.ResourceKeys.Add(Key);
        }
        return Snapshot;
    }

    virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) override
    {
        FMHResolveOutcome Outcome;
        if (Key.Kind != EMHResourceKind::Material)
        {
            return Outcome;
        }
        if (const TPair<FString, FString>* Entry = Materials.Find(Key.LogicalName))
        {
            Outcome.Status = EMHResolveStatus::Resolved;
            Outcome.PayloadPath = Entry->Key;
            Outcome.RawHash = Entry->Value;
            Outcome.CandidatePaths.Add(Entry->Key);
        }
        return Outcome;
    }
};

struct FStaticMeshImporterFixture
{
    FString SourceRoot;

    FStaticMeshImporterFixture()
    {
        SourceRoot = FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("MimirCompositeTests"),
            FString::Printf(
                TEXT("StaticMeshImporter_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        IFileManager::Get().MakeDirectory(*SourceRoot, true);
    }

    ~FStaticMeshImporterFixture()
    {
        MHShutdownProjectIndex();
        IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    }
};

struct FGeneratedPackageCleanup
{
    FString MeshObjectPath;
    FString MaterialObjectPath;
    FString TraceObjectPath;

    ~FGeneratedPackageCleanup()
    {
        CleanupObject(MeshObjectPath);
        CleanupObject(MaterialObjectPath);
        CleanupObject(TraceObjectPath);
    }

private:
    static void CleanupObject(const FString& ObjectPath)
    {
        if (ObjectPath.IsEmpty())
        {
            return;
        }
        const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
        if (!PackageName.StartsWith(TEXT("/Game/MH/Generated/"), ESearchCase::CaseSensitive))
        {
            return;
        }
        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
        {
            ObjectTools::DeleteSingleObject(Asset, false);
        }
        if (Package != nullptr)
        {
            Package->SetDirtyFlag(false);
        }
        const FString Filename = FPackageName::LongPackageNameToFilename(
            PackageName,
            FPackageName::GetAssetPackageExtension());
        IFileManager::Get().Delete(*Filename, false, true, true);
        IFileManager::Get().Delete(*FPaths::ChangeExtension(Filename, TEXT("uexp")), false, true, true);
        IFileManager::Get().Delete(*FPaths::ChangeExtension(Filename, TEXT("ubulk")), false, true, true);
    }
};

bool WriteStaticMeshImporterUtf8(const FString& Path, const FString& Contents)
{
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    return FFileHelper::SaveStringToFile(
        Contents,
        *Path,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FbxMesh* CreateTriangleMesh(FbxScene& Scene, const char* Name, const double Size)
{
    FbxMesh* Mesh = FbxMesh::Create(&Scene, Name);
    Mesh->InitControlPoints(3);
    Mesh->GetControlPoints()[0] = FbxVector4(0.0, 0.0, 0.0);
    Mesh->GetControlPoints()[1] = FbxVector4(Size, 0.0, 0.0);
    Mesh->GetControlPoints()[2] = FbxVector4(0.0, Size, 0.0);
    Mesh->BeginPolygon();
    Mesh->AddPolygon(0);
    Mesh->AddPolygon(1);
    Mesh->AddPolygon(2);
    Mesh->EndPolygon();
    FbxGeometryElementNormal* Normals = Mesh->CreateElementNormal();
    Normals->SetMappingMode(FbxGeometryElement::eByControlPoint);
    Normals->SetReferenceMode(FbxGeometryElement::eDirect);
    Normals->GetDirectArray().Add(FbxVector4(0.0, 0.0, 1.0));
    Normals->GetDirectArray().Add(FbxVector4(0.0, 0.0, 1.0));
    Normals->GetDirectArray().Add(FbxVector4(0.0, 0.0, 1.0));
    return Mesh;
}

FbxMesh* CreateReplacementRenderMesh(FbxScene& Scene, const char* Name)
{
    FbxMesh* Mesh = FbxMesh::Create(&Scene, Name);
    Mesh->InitControlPoints(4);
    Mesh->GetControlPoints()[0] = FbxVector4(0.0, 0.0, 0.0);
    Mesh->GetControlPoints()[1] = FbxVector4(175.0, 0.0, 0.0);
    Mesh->GetControlPoints()[2] = FbxVector4(175.0, 125.0, 0.0);
    Mesh->GetControlPoints()[3] = FbxVector4(0.0, 125.0, 0.0);
    const int32 Triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (const auto& Triangle : Triangles)
    {
        Mesh->BeginPolygon();
        Mesh->AddPolygon(Triangle[0]);
        Mesh->AddPolygon(Triangle[1]);
        Mesh->AddPolygon(Triangle[2]);
        Mesh->EndPolygon();
    }
    return Mesh;
}

FbxMesh* CreateHullMesh(
    FbxScene& Scene,
    const char* Name,
    const double Size,
    const FbxVector4& Offset)
{
    FbxMesh* Mesh = FbxMesh::Create(&Scene, Name);
    Mesh->InitControlPoints(4);
    Mesh->GetControlPoints()[0] = Offset;
    Mesh->GetControlPoints()[1] = Offset + FbxVector4(Size, 0.0, 0.0);
    Mesh->GetControlPoints()[2] = Offset + FbxVector4(0.0, Size, 0.0);
    Mesh->GetControlPoints()[3] = Offset + FbxVector4(0.0, 0.0, Size);
    const int32 Triangles[4][3] = {
        {0, 2, 1},
        {0, 1, 3},
        {0, 3, 2},
        {1, 2, 3}};
    for (const auto& Triangle : Triangles)
    {
        Mesh->BeginPolygon();
        Mesh->AddPolygon(Triangle[0]);
        Mesh->AddPolygon(Triangle[1]);
        Mesh->AddPolygon(Triangle[2]);
        Mesh->EndPolygon();
    }
    return Mesh;
}

/** Axis-aligned box hull; collision fixtures carry no authored normal layer. */
FbxMesh* CreateBoxMesh(
    FbxScene& Scene,
    const char* Name,
    const FbxVector4& Min,
    const FbxVector4& Max)
{
    FbxMesh* Mesh = FbxMesh::Create(&Scene, Name);
    Mesh->InitControlPoints(8);
    for (int32 Index = 0; Index < 8; ++Index)
    {
        Mesh->GetControlPoints()[Index] = FbxVector4(
            (Index & 1) != 0 ? Max[0] : Min[0],
            (Index & 2) != 0 ? Max[1] : Min[1],
            (Index & 4) != 0 ? Max[2] : Min[2]);
    }
    static const int32 Faces[12][3] = {
        {0, 2, 1}, {1, 2, 3},
        {4, 5, 6}, {5, 7, 6},
        {0, 1, 4}, {1, 5, 4},
        {2, 6, 3}, {3, 6, 7},
        {0, 4, 2}, {2, 4, 6},
        {1, 3, 5}, {3, 7, 5}};
    for (const auto& Face : Faces)
    {
        Mesh->BeginPolygon();
        Mesh->AddPolygon(Face[0]);
        Mesh->AddPolygon(Face[1]);
        Mesh->AddPolygon(Face[2]);
        Mesh->EndPolygon();
    }
    return Mesh;
}

/** Blender `use_custom_props` writes object ID properties in exactly this shape. */
void SetNodeStringProperty(FbxNode& Node, const char* Name, const FString& Value)
{
    FbxProperty Property = FbxProperty::Create(&Node, FbxStringDT, Name);
    Property.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
    Property.Set(FbxString(TCHAR_TO_UTF8(*Value)));
}

FbxNode* AddMeshNode(FbxScene& Scene, const char* Name, FbxMesh& Mesh)
{
    FbxNode* Node = FbxNode::Create(&Scene, Name);
    Node->SetNodeAttribute(&Mesh);
    Scene.GetRootNode()->AddChild(Node);
    return Node;
}

/**
 * Mirror the canonical Blender exporter transport: axis_forward='X' composes
 * a RotZ(-90) axis conversion into every root node's local transform while
 * control points stay raw. Fixtures must carry that conversion too, so the
 * production translator's unwind (importer version 5) reads them exactly like
 * a real exported mesh.
 */
void ApplyCanonicalExportConversion(FbxScene& Scene)
{
    FbxAMatrix Conversion;
    Conversion.SetR(FbxVector4(0.0, 0.0, -90.0));
    FbxNode* Root = Scene.GetRootNode();
    for (int32 Index = 0; Index < Root->GetChildCount(); ++Index)
    {
        FbxNode* Node = Root->GetChild(Index);
        FbxAMatrix Local;
        Local.SetT(FbxVector4(Node->LclTranslation.Get()));
        Local.SetR(FbxVector4(Node->LclRotation.Get()));
        Local.SetS(FbxVector4(Node->LclScaling.Get()));
        const FbxAMatrix Converted = Conversion * Local;
        const FbxVector4 T = Converted.GetT();
        const FbxVector4 R = Converted.GetR();
        const FbxVector4 S = Converted.GetS();
        Node->LclTranslation.Set(FbxDouble3(T[0], T[1], T[2]));
        Node->LclRotation.Set(FbxDouble3(R[0], R[1], R[2]));
        Node->LclScaling.Set(FbxDouble3(S[0], S[1], S[2]));
    }
}

void BindMaterial(FbxMesh& Mesh, FbxNode& Node, FbxSurfaceMaterial& Material)
{
    Node.AddMaterial(&Material);
    FbxGeometryElementMaterial* MaterialLayer = Mesh.CreateElementMaterial();
    MaterialLayer->SetMappingMode(FbxGeometryElement::eAllSame);
    MaterialLayer->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    MaterialLayer->GetIndexArray().Add(0);
}

bool ExportPlainStaticMeshFbx(
    const FString& TemplatePath,
    const FString& Path,
    const FString& MaterialName,
    const bool bReplacement,
    FString& OutError)
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
    FbxScene* Scene = FbxScene::Create(Manager, "Scene");
    FbxImporter* Importer = FbxImporter::Create(Manager, "TemplateImporter");
    if (!Importer->Initialize(TCHAR_TO_UTF8(*TemplatePath), -1, IOSettings) ||
        !Importer->Import(Scene))
    {
        OutError = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }
    FbxNode* Root = Scene->GetRootNode();
    while (Root != nullptr && Root->GetChildCount() > 0)
    {
        FbxNode* Child = Root->GetChild(0);
        Root->RemoveChild(Child);
        Child->Destroy(true);
    }
    FbxSurfacePhong* Material = FbxSurfacePhong::Create(
        Scene,
        TCHAR_TO_UTF8(*MaterialName));

    if (!bReplacement)
    {
        FbxMesh* LOD0Mesh = CreateTriangleMesh(*Scene, "render_lod00_geometry", 100.0);
        FbxNode* LOD0 = AddMeshNode(*Scene, "render_lod00", *LOD0Mesh);
        // A non-axis-aligned transform makes the regression distinguish
        // inverse-transpose direction handling from FBX MultR, which treats
        // its argument as Euler angles.
        LOD0->LclRotation.Set(FbxDouble3(25.0, 0.0, 0.0));
        BindMaterial(*LOD0Mesh, *LOD0, *Material);
        FbxMesh* LOD1Mesh = CreateTriangleMesh(*Scene, "render_lod01_geometry", 50.0);
        FbxNode* LOD1 = AddMeshNode(*Scene, "render_lod01", *LOD1Mesh);
        LOD1->LclScaling.Set(FbxDouble3(-1.0, 1.0, 1.0));
        BindMaterial(*LOD1Mesh, *LOD1, *Material);
        AddMeshNode(
            *Scene,
            "UCX_body",
            *CreateHullMesh(*Scene, "ucx_geometry", 30.0, FbxVector4(0.0, 0.0, 0.0)));
        AddMeshNode(
            *Scene,
            "trace_hull_cls_trace",
            *CreateHullMesh(*Scene, "trace_geometry", 20.0, FbxVector4(40.0, 0.0, 0.0)));
        FbxNode* Socket = FbxNode::Create(Scene, "SOCKET_mount");
        Socket->SetNodeAttribute(FbxNull::Create(Scene, "mount_null"));
        Socket->LclTranslation.Set(FbxDouble3(10.0, 20.0, 30.0));
        Scene->GetRootNode()->AddChild(Socket);
    }
    else
    {
        FbxMesh* RenderMesh = CreateReplacementRenderMesh(*Scene, "replacement_geometry");
        FbxNode* Render = AddMeshNode(*Scene, "replacement_lod00", *RenderMesh);
        BindMaterial(*RenderMesh, *Render, *Material);
        AddMeshNode(
            *Scene,
            "replacement_hull_cls_phys",
            *CreateHullMesh(*Scene, "replacement_hull_geometry", 45.0, FbxVector4(5.0, 6.0, 7.0)));
        FbxNode* Socket = FbxNode::Create(Scene, "SOCKET_replaced");
        Socket->SetNodeAttribute(FbxNull::Create(Scene, "replaced_null"));
        Socket->LclTranslation.Set(FbxDouble3(-5.0, 6.0, 70.0));
        Scene->GetRootNode()->AddChild(Socket);
    }

    ApplyCanonicalExportConversion(*Scene);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    FbxExporter* Exporter = FbxExporter::Create(Manager, "Exporter");
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

/**
 * Two-LOD FBX where LOD1 uses a material LOD0 never references. This is the
 * shape real Dagor content uses for simplified far-LOD shaders.
 */
bool ExportLodUnionStaticMeshFbx(
    const FString& TemplatePath,
    const FString& Path,
    const FString& BaseMaterialName,
    const FString& FarMaterialName,
    FString& OutError)
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
    FbxScene* Scene = FbxScene::Create(Manager, "Scene");
    FbxImporter* Importer = FbxImporter::Create(Manager, "TemplateImporter");
    if (!Importer->Initialize(TCHAR_TO_UTF8(*TemplatePath), -1, IOSettings) ||
        !Importer->Import(Scene))
    {
        OutError = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }
    FbxNode* Root = Scene->GetRootNode();
    while (Root != nullptr && Root->GetChildCount() > 0)
    {
        FbxNode* Child = Root->GetChild(0);
        Root->RemoveChild(Child);
        Child->Destroy(true);
    }

    FbxSurfacePhong* BaseMaterial = FbxSurfacePhong::Create(Scene, TCHAR_TO_UTF8(*BaseMaterialName));
    FbxSurfacePhong* FarMaterial = FbxSurfacePhong::Create(Scene, TCHAR_TO_UTF8(*FarMaterialName));

    FbxMesh* LOD0Mesh = CreateTriangleMesh(*Scene, "union_lod00_geometry", 100.0);
    FbxNode* LOD0 = AddMeshNode(*Scene, "union_lod00", *LOD0Mesh);
    BindMaterial(*LOD0Mesh, *LOD0, *BaseMaterial);

    FbxMesh* LOD1Mesh = CreateTriangleMesh(*Scene, "union_lod01_geometry", 50.0);
    FbxNode* LOD1 = AddMeshNode(*Scene, "union_lod01", *LOD1Mesh);
    BindMaterial(*LOD1Mesh, *LOD1, *FarMaterial);

    ApplyCanonicalExportConversion(*Scene);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    FbxExporter* Exporter = FbxExporter::Create(Manager, "Exporter");
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

/** Tokens the S6.1.2 collision-carrier fixture stamps on its Model nodes. */
struct FCollisionCarrierTokens
{
    FString DominantPhmat;
    FString SecondaryPhmat;
    FString TracePhmat;
};

/**
 * One render LOD plus five collision carriers: two `phys` nodes sharing the
 * dominant phmat (mesh + box), one `phys` capsule with a second phmat, and two
 * `trace` nodes in two different phmat groups.
 */
bool ExportCollisionCarrierFbx(
    const FString& TemplatePath,
    const FString& Path,
    const FString& MaterialName,
    const FCollisionCarrierTokens& Tokens,
    FString& OutError)
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
    FbxScene* Scene = FbxScene::Create(Manager, "Scene");
    FbxImporter* Importer = FbxImporter::Create(Manager, "TemplateImporter");
    if (!Importer->Initialize(TCHAR_TO_UTF8(*TemplatePath), -1, IOSettings) ||
        !Importer->Import(Scene))
    {
        OutError = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }
    FbxNode* Root = Scene->GetRootNode();
    while (Root != nullptr && Root->GetChildCount() > 0)
    {
        FbxNode* Child = Root->GetChild(0);
        Root->RemoveChild(Child);
        Child->Destroy(true);
    }

    FbxSurfacePhong* Material = FbxSurfacePhong::Create(Scene, TCHAR_TO_UTF8(*MaterialName));
    FbxMesh* RenderMesh = CreateTriangleMesh(*Scene, "carrier_render_geometry", 100.0);
    FbxNode* Render = AddMeshNode(*Scene, "carrier_lod00", *RenderMesh);
    BindMaterial(*RenderMesh, *Render, *Material);

    // Names deliberately carry no v4 `_cls_*` marker: the user properties alone
    // must classify these nodes, exactly as real Dagor node names do.
    FbxNode* PhysHull = AddMeshNode(
        *Scene,
        "hull_shape",
        *CreateHullMesh(*Scene, "carrier_hull_geometry", 30.0, FbxVector4(0.0, 0.0, 0.0)));
    SetNodeStringProperty(*PhysHull, "mh_collision", TEXT("phys"));
    SetNodeStringProperty(*PhysHull, "mh_collision_shape", TEXT("mesh"));
    SetNodeStringProperty(*PhysHull, "mh_phmat", Tokens.DominantPhmat);

    FbxNode* PhysBox = AddMeshNode(
        *Scene,
        "box_shape",
        *CreateBoxMesh(
            *Scene,
            "carrier_box_geometry",
            FbxVector4(0.0, 0.0, 0.0),
            FbxVector4(20.0, 30.0, 40.0)));
    PhysBox->LclTranslation.Set(FbxDouble3(100.0, 0.0, 0.0));
    SetNodeStringProperty(*PhysBox, "mh_collision", TEXT("phys"));
    SetNodeStringProperty(*PhysBox, "mh_collision_shape", TEXT("box"));
    SetNodeStringProperty(*PhysBox, "mh_phmat", Tokens.DominantPhmat);

    FbxNode* PhysCapsule = AddMeshNode(
        *Scene,
        "capsule_shape",
        *CreateBoxMesh(
            *Scene,
            "carrier_capsule_geometry",
            FbxVector4(0.0, 0.0, 0.0),
            FbxVector4(10.0, 10.0, 60.0)));
    PhysCapsule->LclTranslation.Set(FbxDouble3(0.0, 200.0, 0.0));
    SetNodeStringProperty(*PhysCapsule, "mh_collision", TEXT("phys"));
    SetNodeStringProperty(*PhysCapsule, "mh_collision_shape", TEXT("capsule"));
    SetNodeStringProperty(*PhysCapsule, "mh_phmat", Tokens.SecondaryPhmat);

    FbxNode* TraceA = AddMeshNode(
        *Scene,
        "trace_shape_a",
        *CreateHullMesh(*Scene, "carrier_trace_a_geometry", 25.0, FbxVector4(0.0, 0.0, 300.0)));
    SetNodeStringProperty(*TraceA, "mh_collision", TEXT("trace"));
    SetNodeStringProperty(*TraceA, "mh_collision_shape", TEXT("mesh"));
    SetNodeStringProperty(*TraceA, "mh_phmat", Tokens.DominantPhmat);

    FbxNode* TraceB = AddMeshNode(
        *Scene,
        "trace_shape_b",
        *CreateHullMesh(*Scene, "carrier_trace_b_geometry", 25.0, FbxVector4(0.0, 100.0, 300.0)));
    SetNodeStringProperty(*TraceB, "mh_collision", TEXT("trace"));
    SetNodeStringProperty(*TraceB, "mh_collision_shape", TEXT("mesh"));
    SetNodeStringProperty(*TraceB, "mh_phmat", Tokens.TracePhmat);

    ApplyCanonicalExportConversion(*Scene);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    FbxExporter* Exporter = FbxExporter::Create(Manager, "Exporter");
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

bool ReadSourceHash(const FString& Path, FString& OutHash, TArray<uint8>* OutBytes = nullptr)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Path))
    {
        return false;
    }
    OutHash = MHRawPayloadHash(Bytes);
    if (OutBytes != nullptr)
    {
        *OutBytes = MoveTemp(Bytes);
    }
    return true;
}

bool ContainsMHPropertyMarker(const TArray<uint8>& Bytes)
{
    constexpr uint8 Marker[] = {'M', 'H', '_'};
    for (int32 Index = 0; Index + static_cast<int32>(UE_ARRAY_COUNT(Marker)) <= Bytes.Num(); ++Index)
    {
        if (FMemory::Memcmp(Bytes.GetData() + Index, Marker, UE_ARRAY_COUNT(Marker)) == 0)
        {
            return true;
        }
    }
    return false;
}

bool HasWarning(const FMHStaticMeshOperationResult& Result, const TCHAR* Code)
{
    return Result.Warnings.ContainsByPredicate([Code](const FString& Warning)
    {
        return Warning.StartsWith(Code, ESearchCase::CaseSensitive);
    });
}

bool VerifyInitialMesh(
    FAutomationTestBase& Test,
    UStaticMesh& StaticMesh,
    UMaterialInstanceConstant* Material,
    const FString& MaterialName)
{
    bool bPassed = Test.TestEqual(TEXT("initial dense LOD count"), StaticMesh.GetNumSourceModels(), 2);
    if (StaticMesh.GetNumSourceModels() > 0)
    {
        const FStaticMeshSourceModel& SourceModel = StaticMesh.GetSourceModel(0);
        bPassed &= Test.TestFalse(
            TEXT("source custom vertex normals are preserved"),
            SourceModel.BuildSettings.bRecomputeNormals);
        bPassed &= Test.TestTrue(
            TEXT("Mikk tangents are rebuilt"),
            SourceModel.BuildSettings.bRecomputeTangents && SourceModel.BuildSettings.bUseMikkTSpace);
    }
    const FMeshDescription* LOD0 = StaticMesh.GetMeshDescription(0);
    bPassed &= Test.TestNotNull(TEXT("initial LOD0 MeshDescription"), LOD0);
    if (LOD0 != nullptr && LOD0->Triangles().Num() > 0)
    {
        const FTriangleID TriangleId = LOD0->Triangles().GetFirstValidID();
        const TArrayView<const FVertexInstanceID> Instances =
            LOD0->GetTriangleVertexInstances(TriangleId);
        const FStaticMeshConstAttributes Attributes(*LOD0);
        const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
        const TVertexInstanceAttributesConstRef<FVector3f> Normals =
            Attributes.GetVertexInstanceNormals();
        const FVector3f A = Positions[LOD0->GetVertexInstanceVertex(Instances[0])];
        const FVector3f B = Positions[LOD0->GetVertexInstanceVertex(Instances[1])];
        const FVector3f C = Positions[LOD0->GetVertexInstanceVertex(Instances[2])];
        const FVector3f FaceNormal = FVector3f::CrossProduct(C - A, B - A).GetSafeNormal();
        bPassed &= Test.TestTrue(
            TEXT("direct FBX winding matches imported split-normal hemisphere"),
            FVector3f::DotProduct(FaceNormal, Normals[Instances[0]]) > 0.999f);
    }
    bPassed &= Test.TestEqual(TEXT("material slot count"), StaticMesh.GetStaticMaterials().Num(), 1);
    if (StaticMesh.GetStaticMaterials().Num() == 1)
    {
        bPassed &= Test.TestEqual(
            TEXT("logical material slot name"),
            StaticMesh.GetStaticMaterials()[0].MaterialSlotName,
            FName(*MaterialName));
        bPassed &= Test.TestTrue(
            TEXT("resolved MI is bound before build"),
            StaticMesh.GetStaticMaterials()[0].MaterialInterface.Get() == Material);
    }
    bPassed &= Test.TestEqual(TEXT("initial socket count"), StaticMesh.Sockets.Num(), 1);
    if (StaticMesh.Sockets.Num() == 1)
    {
        bPassed &= Test.TestEqual(TEXT("SOCKET_ marker removed"), StaticMesh.Sockets[0]->SocketName, FName(TEXT("mount")));
        bPassed &= Test.TestEqual(
            TEXT("socket FBX reflection applied"),
            StaticMesh.Sockets[0]->RelativeLocation,
            FVector(10.0, -20.0, 30.0));
    }
    const UBodySetup* BodySetup = StaticMesh.GetBodySetup();
    bPassed &= Test.TestNotNull(TEXT("initial BodySetup"), BodySetup);
    if (BodySetup != nullptr)
    {
        bPassed &= Test.TestEqual(TEXT("UCX plus cls hull count"), BodySetup->AggGeom.ConvexElems.Num(), 2);
        bool bHasBoth = false;
        bool bHasQueryOnly = false;
        for (const FKConvexElem& Hull : BodySetup->AggGeom.ConvexElems)
        {
            bHasBoth |= Hull.GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics;
            bHasQueryOnly |= Hull.GetCollisionEnabled() == ECollisionEnabled::QueryOnly;
        }
        bPassed &= Test.TestTrue(TEXT("UCX collision is query-and-physics"), bHasBoth);
        bPassed &= Test.TestTrue(TEXT("cls_trace collision is query-only"), bHasQueryOnly);
        bPassed &= Test.TestEqual(TEXT("no carrier means no box element"), BodySetup->AggGeom.BoxElems.Num(), 0);
        bPassed &= Test.TestEqual(TEXT("no carrier means no capsule element"), BodySetup->AggGeom.SphylElems.Num(), 0);
        bPassed &= Test.TestNull(TEXT("no carrier means no per-mesh physical material"), BodySetup->PhysMaterial.Get());
    }
    // Name-marked collision keeps its v4 meaning: no companion complex mesh.
    bPassed &= Test.TestNull(
        TEXT("name-marked collision leaves ComplexCollisionMesh unset"),
        StaticMesh.ComplexCollisionMesh.Get());
    return bPassed;
}

bool VerifyReplacementMesh(FAutomationTestBase& Test, UStaticMesh& StaticMesh)
{
    bool bPassed = Test.TestEqual(TEXT("old LOD1 removed"), StaticMesh.GetNumSourceModels(), 1);
    const FMeshDescription* LOD0 = StaticMesh.GetMeshDescription(0);
    bPassed &= Test.TestNotNull(TEXT("replacement LOD0 MeshDescription"), LOD0);
    if (LOD0 != nullptr)
    {
        bPassed &= Test.TestEqual(TEXT("replacement geometry has two polygons"), LOD0->Polygons().Num(), 2);
    }
    bPassed &= Test.TestEqual(TEXT("old socket removed"), StaticMesh.Sockets.Num(), 1);
    if (StaticMesh.Sockets.Num() == 1)
    {
        bPassed &= Test.TestEqual(
            TEXT("replacement socket installed"),
            StaticMesh.Sockets[0]->SocketName,
            FName(TEXT("replaced")));
        bPassed &= Test.TestEqual(
            TEXT("replacement socket transform"),
            StaticMesh.Sockets[0]->RelativeLocation,
            FVector(-5.0, -6.0, 70.0));
    }
    const UBodySetup* BodySetup = StaticMesh.GetBodySetup();
    bPassed &= Test.TestNotNull(TEXT("replacement BodySetup"), BodySetup);
    if (BodySetup != nullptr)
    {
        bPassed &= Test.TestEqual(TEXT("old collision inventory replaced"), BodySetup->AggGeom.ConvexElems.Num(), 1);
        if (BodySetup->AggGeom.ConvexElems.Num() == 1)
        {
            bPassed &= Test.TestEqual(
                TEXT("replacement cls_phys mode"),
                BodySetup->AggGeom.ConvexElems[0].GetCollisionEnabled(),
                ECollisionEnabled::PhysicsOnly);
        }
    }
    return bPassed;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshImporterEndToEndTest,
    "Mimir.V4.StaticMesh.Importer.EndToEndReimport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshImporterEndToEndTest::RunTest(const FString& Parameters)
{
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString LogicalName = TEXT("s5_e2e_") + Token;
    const FString MaterialName = TEXT("s5_e2e_mat_") + Token;
    const FString MeshPackage = TEXT("/Game/MH/Generated/Meshes/") + LogicalName;
    const FString MaterialPackage = TEXT("/Game/MH/Generated/Materials/") + MaterialName;
    FGeneratedPackageCleanup Cleanup;
    Cleanup.MeshObjectPath = MeshPackage + TEXT(".") + LogicalName;
    Cleanup.MaterialObjectPath = MaterialPackage + TEXT(".") + MaterialName;
    FStaticMeshImporterFixture Fixture;
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    const FString TemplateFbx = FPaths::Combine(
        GoldenRoot,
        TEXT("fixtures/axis/axis_probe.fbx"));

    UPackage* MaterialOuter = CreatePackage(*MaterialPackage);
    UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(
        MaterialOuter,
        FName(*MaterialName),
        RF_Public | RF_Standalone | RF_Transactional);
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
        .Get()
        .AssetCreated(Material);

    const FString MaterialPath = FPaths::Combine(
        Fixture.SourceRoot,
        TEXT("materials"),
        MaterialName + TEXT(".material"));
    bool bPassed = TestTrue(
        TEXT("write resolved material source"),
        WriteStaticMeshImporterUtf8(MaterialPath, TEXT("{\n  \"class\": \"simple\"\n}\n")));
    FString MaterialHash;
    bPassed &= TestTrue(TEXT("hash resolved material source"), ReadSourceHash(MaterialPath, MaterialHash));
    UMHMaterialSourceData* MaterialReceipt = NewObject<UMHMaterialSourceData>(Material);
    MaterialReceipt->LogicalName = MaterialName;
    MaterialReceipt->SourceRelativePath = TEXT("materials/") + MaterialName + TEXT(".material");
    MaterialReceipt->SourceHash = MaterialHash;
    MaterialReceipt->AppliedHash = MaterialHash;
    MaterialReceipt->AppliedParent = TEXT("class:simple");
    Material->AddAssetUserData(MaterialReceipt);
    const FString MeshPath = FPaths::Combine(
        Fixture.SourceRoot,
        TEXT("meshes"),
        LogicalName + TEXT(".mesh.fbx"));
    FString Error;
    bPassed &= TestTrue(
        TEXT("export initial plain FBX"),
        ExportPlainStaticMeshFbx(TemplateFbx, MeshPath, MaterialName, false, Error));
    if (!Error.IsEmpty()) AddError(Error);

    FMHSourceAnalysisEntry Entry;
    Entry.Key.Kind = EMHResourceKind::StaticMesh;
    Entry.Key.LogicalName = LogicalName;
    Entry.PayloadPath = MeshPath;
    Entry.SourcePath = TEXT("meshes/") + LogicalName + TEXT(".mesh.fbx");
    Entry.Change = EMHSourceChange::Create;
    TArray<uint8> InitialBytes;
    bPassed &= TestTrue(TEXT("hash initial FBX"), ReadSourceHash(MeshPath, Entry.RawHash, &InitialBytes));
    bPassed &= TestFalse(TEXT("FBX has no MH metadata marker"), ContainsMHPropertyMarker(InitialBytes));

    FStaticMeshImporterTestResolver Resolver;
    Resolver.MaterialName = MaterialName;
    Resolver.MaterialPath = MaterialPath;
    Resolver.MaterialHash = MaterialHash;
    FMHStaticMeshOperationResult First = MHImportStaticMeshV4(
        Entry,
        Resolver,
        Fixture.SourceRoot);
    if (!First.Succeeded())
    {
        AddError(FString::Printf(TEXT("initial static-mesh import failed: %s"), *First.Error));
        return false;
    }
    bPassed &= TestTrue(TEXT("first import creates deterministic asset"), First.bCreated);
    bPassed &= TestTrue(TEXT("first import performs build"), First.bRebuilt);
    bPassed &= TestEqual(TEXT("deterministic object path"), First.StaticMesh->GetPathName(), Cleanup.MeshObjectPath);
    bPassed &= VerifyInitialMesh(*this, *First.StaticMesh, Material, MaterialName);

    UMHStaticMeshImportData* Receipt = Cast<UMHStaticMeshImportData>(First.StaticMesh->GetAssetImportData());
    bPassed &= TestNotNull(TEXT("managed mesh receipt exists"), Receipt);
    if (Receipt != nullptr)
    {
        bPassed &= TestEqual(TEXT("receipt logical name"), Receipt->LogicalName, LogicalName);
        bPassed &= TestEqual(TEXT("receipt relative source"), Receipt->SourceRelativePath, Entry.SourcePath);
        bPassed &= TestEqual(TEXT("receipt raw hash"), Receipt->SourceHash, Entry.RawHash);
        bPassed &= TestEqual(TEXT("receipt importer version"), Receipt->ImporterVersion, MHStaticMeshImporterVersion);
        bPassed &= TestFalse(TEXT("fresh receipt is not locally modified"), Receipt->bLocallyModified);
    }

    UStaticMesh* const OriginalMesh = First.StaticMesh;
    FMHStaticMeshOperationResult NoChange = MHImportStaticMeshV4(
        Entry,
        Resolver,
        Fixture.SourceRoot);
    bPassed &= TestTrue(TEXT("equal source returns existing mesh"), NoChange.Succeeded());
    bPassed &= TestEqual(TEXT("NO_CHANGE preserves exact UObject"), NoChange.StaticMesh, OriginalMesh);
    bPassed &= TestFalse(TEXT("NO_CHANGE performs no rebuild"), NoChange.bRebuilt);

    if (Receipt != nullptr)
    {
        Receipt->ImporterVersion = MHStaticMeshImporterVersion - 1;
    }
    FMHStaticMeshOperationResult VersionUpgrade = MHImportStaticMeshV4(
        Entry,
        Resolver,
        Fixture.SourceRoot);
    bPassed &= TestTrue(TEXT("stale importer-version upgrade succeeds"), VersionUpgrade.Succeeded());
    bPassed &= TestEqual(
        TEXT("version upgrade preserves exact UObject"),
        VersionUpgrade.StaticMesh,
        OriginalMesh);
    bPassed &= TestTrue(
        TEXT("equal-hash version upgrade performs full rebuild"),
        VersionUpgrade.bRebuilt);
    Receipt = Cast<UMHStaticMeshImportData>(OriginalMesh->GetAssetImportData());
    if (Receipt != nullptr)
    {
        bPassed &= TestEqual(
            TEXT("version upgrade advances receipt"),
            Receipt->ImporterVersion,
            MHStaticMeshImporterVersion);
    }

    bPassed &= TestTrue(
        TEXT("export changed plain FBX"),
        ExportPlainStaticMeshFbx(TemplateFbx, MeshPath, MaterialName, true, Error));
    if (!Error.IsEmpty()) AddError(Error);
    const FString InitialHash = Entry.RawHash;
    TArray<uint8> ReplacementBytes;
    bPassed &= TestTrue(TEXT("hash changed FBX"), ReadSourceHash(MeshPath, Entry.RawHash, &ReplacementBytes));
    bPassed &= TestNotEqual(TEXT("replacement raw hash changed"), Entry.RawHash, InitialHash);
    bPassed &= TestFalse(TEXT("replacement FBX has no MH metadata marker"), ContainsMHPropertyMarker(ReplacementBytes));
    Entry.Change = EMHSourceChange::Reimport;
    Receipt = Cast<UMHStaticMeshImportData>(OriginalMesh->GetAssetImportData());
    if (Receipt != nullptr)
    {
        Receipt->bLocallyModified = true;
    }

    FMHStaticMeshOperationResult Reimport = MHImportStaticMeshV4(
        Entry,
        Resolver,
        Fixture.SourceRoot);
    if (!Reimport.Succeeded())
    {
        AddError(FString::Printf(TEXT("static-mesh reimport failed: %s"), *Reimport.Error));
        return false;
    }
    bPassed &= TestEqual(TEXT("reimport preserves exact UObject"), Reimport.StaticMesh, OriginalMesh);
    bPassed &= TestFalse(TEXT("reimport does not create another asset"), Reimport.bCreated);
    bPassed &= TestTrue(TEXT("changed source performs full rebuild"), Reimport.bRebuilt);
    bPassed &= TestTrue(
        TEXT("local edit warning emitted at import"),
        HasWarning(Reimport, TEXT("MH_W_MANAGED_STATIC_MESH_LOCALLY_MODIFIED")));
    bPassed &= VerifyReplacementMesh(*this, *Reimport.StaticMesh);
    Receipt = Cast<UMHStaticMeshImportData>(Reimport.StaticMesh->GetAssetImportData());
    bPassed &= TestNotNull(TEXT("receipt survives in-place rebuild"), Receipt);
    if (Receipt != nullptr)
    {
        bPassed &= TestEqual(TEXT("receipt advances to replacement hash"), Receipt->SourceHash, Entry.RawHash);
        bPassed &= TestFalse(TEXT("successful apply resets local-edit flag"), Receipt->bLocallyModified);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshImporterLodMaterialUnionTest,
    "Mimir.V4.StaticMesh.Importer.LodMaterialUnion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshImporterLodMaterialUnionTest::RunTest(const FString& Parameters)
{
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString LogicalName = TEXT("s611_union_") + Token;
    const FString BaseMaterialName = TEXT("s611_base_") + Token;
    const FString FarMaterialName = TEXT("s611_far_") + Token;
    const FString MeshPackage = TEXT("/Game/MH/Generated/Meshes/") + LogicalName;

    FGeneratedPackageCleanup MeshCleanup;
    MeshCleanup.MeshObjectPath = MeshPackage + TEXT(".") + LogicalName;
    MeshCleanup.MaterialObjectPath =
        TEXT("/Game/MH/Generated/Materials/") + BaseMaterialName + TEXT(".") + BaseMaterialName;
    FGeneratedPackageCleanup FarCleanup;
    FarCleanup.MaterialObjectPath =
        TEXT("/Game/MH/Generated/Materials/") + FarMaterialName + TEXT(".") + FarMaterialName;

    FStaticMeshImporterFixture Fixture;
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    const FString TemplateFbx = FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"));

    bool bPassed = true;
    FStaticMeshUnionTestResolver Resolver;
    TMap<FString, UMaterialInstanceConstant*> Managed;
    for (const FString& MaterialName : {BaseMaterialName, FarMaterialName})
    {
        const FString MaterialPackage = TEXT("/Game/MH/Generated/Materials/") + MaterialName;
        UPackage* MaterialOuter = CreatePackage(*MaterialPackage);
        UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(
            MaterialOuter,
            FName(*MaterialName),
            RF_Public | RF_Standalone | RF_Transactional);
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get()
            .AssetCreated(Material);
        const FString MaterialPath = FPaths::Combine(
            Fixture.SourceRoot,
            TEXT("materials"),
            MaterialName + TEXT(".material"));
        bPassed &= TestTrue(
            TEXT("write resolved material source"),
            WriteStaticMeshImporterUtf8(MaterialPath, TEXT("{\n  \"class\": \"simple\"\n}\n")));
        FString MaterialHash;
        bPassed &= TestTrue(TEXT("hash resolved material source"), ReadSourceHash(MaterialPath, MaterialHash));
        UMHMaterialSourceData* Receipt = NewObject<UMHMaterialSourceData>(Material);
        Receipt->LogicalName = MaterialName;
        Receipt->SourceRelativePath = TEXT("materials/") + MaterialName + TEXT(".material");
        Receipt->SourceHash = MaterialHash;
        Receipt->AppliedHash = MaterialHash;
        Receipt->AppliedParent = TEXT("class:simple");
        Material->AddAssetUserData(Receipt);
        Resolver.Materials.Add(MaterialName, TPair<FString, FString>(MaterialPath, MaterialHash));
        Managed.Add(MaterialName, Material);
    }

    const FString MeshPath = FPaths::Combine(
        Fixture.SourceRoot,
        TEXT("meshes"),
        LogicalName + TEXT(".mesh.fbx"));
    FString Error;
    bPassed &= TestTrue(
        TEXT("export LOD union FBX"),
        ExportLodUnionStaticMeshFbx(TemplateFbx, MeshPath, BaseMaterialName, FarMaterialName, Error));
    if (!Error.IsEmpty()) AddError(Error);

    FMHSourceAnalysisEntry Entry;
    Entry.Key.Kind = EMHResourceKind::StaticMesh;
    Entry.Key.LogicalName = LogicalName;
    Entry.PayloadPath = MeshPath;
    Entry.SourcePath = TEXT("meshes/") + LogicalName + TEXT(".mesh.fbx");
    Entry.Change = EMHSourceChange::Create;
    bPassed &= TestTrue(TEXT("hash LOD union FBX"), ReadSourceHash(MeshPath, Entry.RawHash));

    FMHStaticMeshOperationResult Import = MHImportStaticMeshV4(Entry, Resolver, Fixture.SourceRoot);
    if (!Import.Succeeded())
    {
        AddError(FString::Printf(TEXT("LOD union import failed: %s"), *Import.Error));
        return false;
    }
    UStaticMesh* Mesh = Import.StaticMesh;
    bPassed &= TestEqual(TEXT("union import has two LODs"), Mesh->GetNumSourceModels(), 2);
    bPassed &= TestEqual(TEXT("union material slot count"), Mesh->GetStaticMaterials().Num(), 2);
    if (Mesh->GetStaticMaterials().Num() == 2)
    {
        bPassed &= TestEqual(
            TEXT("LOD0 slot is first"),
            Mesh->GetStaticMaterials()[0].MaterialSlotName,
            FName(*BaseMaterialName));
        bPassed &= TestEqual(
            TEXT("LOD1-only slot is appended"),
            Mesh->GetStaticMaterials()[1].MaterialSlotName,
            FName(*FarMaterialName));
        bPassed &= TestTrue(
            TEXT("LOD1-only slot binds its managed MI"),
            Mesh->GetStaticMaterials()[1].MaterialInterface.Get() == Managed.FindRef(FarMaterialName));
    }
    bPassed &= TestEqual(
        TEXT("LOD0 section 0 addresses the LOD0 slot"),
        Mesh->GetSectionInfoMap().Get(0, 0).MaterialIndex,
        0);
    bPassed &= TestEqual(
        TEXT("LOD1 section 0 addresses the LOD1-only slot"),
        Mesh->GetSectionInfoMap().Get(1, 0).MaterialIndex,
        1);

    // Reimport of unchanged bytes must not reorder the union.
    const FMHStaticMeshOperationResult NoChange = MHImportStaticMeshV4(Entry, Resolver, Fixture.SourceRoot);
    bPassed &= TestTrue(TEXT("unchanged reimport succeeds"), NoChange.Succeeded());
    bPassed &= TestEqual(TEXT("unchanged reimport keeps the asset"), NoChange.StaticMesh, Mesh);
    bPassed &= TestFalse(TEXT("unchanged reimport performs no rebuild"), NoChange.bRebuilt);

    // A forced full rebuild of the same bytes reproduces the same order.
    const FMHStaticMeshOperationResult Forced = MHImportStaticMeshV4(Entry, Resolver, Fixture.SourceRoot, true);
    bPassed &= TestTrue(TEXT("forced reimport succeeds"), Forced.Succeeded());
    bPassed &= TestTrue(TEXT("forced reimport rebuilds"), Forced.bRebuilt);
    if (Forced.StaticMesh != nullptr && Forced.StaticMesh->GetStaticMaterials().Num() == 2)
    {
        bPassed &= TestEqual(
            TEXT("forced reimport keeps LOD0 slot first"),
            Forced.StaticMesh->GetStaticMaterials()[0].MaterialSlotName,
            FName(*BaseMaterialName));
        bPassed &= TestEqual(
            TEXT("forced reimport keeps LOD1-only slot second"),
            Forced.StaticMesh->GetStaticMaterials()[1].MaterialSlotName,
            FName(*FarMaterialName));
        bPassed &= TestEqual(
            TEXT("forced reimport keeps LOD1 section mapping"),
            Forced.StaticMesh->GetSectionInfoMap().Get(1, 0).MaterialIndex,
            1);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHStaticMeshImporterCollisionCarrierTest,
    "Mimir.V4.StaticMesh.Importer.CollisionCarriers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHStaticMeshImporterCollisionCarrierTest::RunTest(const FString& Parameters)
{
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString LogicalName = TEXT("s612_carrier_") + Token;
    const FString MaterialName = TEXT("s612_mat_") + Token;
    const FString MeshPackage = TEXT("/Game/MH/Generated/Meshes/") + LogicalName;
    const FString TracePackage = MHTraceCollisionMeshPackageName(LogicalName);
    const FString TraceObjectName = LogicalName + TEXT("_trace");

    FCollisionCarrierTokens Tokens;
    Tokens.DominantPhmat = TEXT("wood_") + Token;
    Tokens.SecondaryPhmat = TEXT("steel_") + Token;
    Tokens.TracePhmat = TEXT("glass_") + Token;

    FGeneratedPackageCleanup Cleanup;
    Cleanup.MeshObjectPath = MeshPackage + TEXT(".") + LogicalName;
    Cleanup.MaterialObjectPath =
        TEXT("/Game/MH/Generated/Materials/") + MaterialName + TEXT(".") + MaterialName;
    Cleanup.TraceObjectPath = TracePackage + TEXT(".") + TraceObjectName;

    FStaticMeshImporterFixture Fixture;
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    const FString TemplateFbx = FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"));

    // Only the dominant token has an asset; the other two must degrade to a
    // warning, never to an import failure.
    const FString PhysicalMaterialRoot = TEXT("/Game/MH/TestPhysicalMaterials/") + Token;
    UPackage* PhysMaterialPackage =
        CreatePackage(*(PhysicalMaterialRoot + TEXT("/") + Tokens.DominantPhmat));
    UPhysicalMaterial* DominantPhysMaterial = NewObject<UPhysicalMaterial>(
        PhysMaterialPackage,
        FName(*Tokens.DominantPhmat),
        RF_Public | RF_Standalone | RF_Transactional);
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FString PreviousPhysicalMaterialRoot = Settings->PhysicalMaterialRoot;
    Settings->PhysicalMaterialRoot = PhysicalMaterialRoot;
    ON_SCOPE_EXIT
    {
        Settings->PhysicalMaterialRoot = PreviousPhysicalMaterialRoot;
    };

    const FString MaterialPackage = TEXT("/Game/MH/Generated/Materials/") + MaterialName;
    UPackage* MaterialOuter = CreatePackage(*MaterialPackage);
    UMaterialInstanceConstant* Material = NewObject<UMaterialInstanceConstant>(
        MaterialOuter,
        FName(*MaterialName),
        RF_Public | RF_Standalone | RF_Transactional);
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
        .Get()
        .AssetCreated(Material);
    const FString MaterialPath = FPaths::Combine(
        Fixture.SourceRoot,
        TEXT("materials"),
        MaterialName + TEXT(".material"));
    bool bPassed = TestTrue(
        TEXT("write resolved material source"),
        WriteStaticMeshImporterUtf8(MaterialPath, TEXT("{\n  \"class\": \"simple\"\n}\n")));
    FString MaterialHash;
    bPassed &= TestTrue(TEXT("hash resolved material source"), ReadSourceHash(MaterialPath, MaterialHash));
    UMHMaterialSourceData* MaterialReceipt = NewObject<UMHMaterialSourceData>(Material);
    MaterialReceipt->LogicalName = MaterialName;
    MaterialReceipt->SourceRelativePath = TEXT("materials/") + MaterialName + TEXT(".material");
    MaterialReceipt->SourceHash = MaterialHash;
    MaterialReceipt->AppliedHash = MaterialHash;
    MaterialReceipt->AppliedParent = TEXT("class:simple");
    Material->AddAssetUserData(MaterialReceipt);

    const FString MeshPath = FPaths::Combine(
        Fixture.SourceRoot,
        TEXT("meshes"),
        LogicalName + TEXT(".mesh.fbx"));
    FString Error;
    bPassed &= TestTrue(
        TEXT("export collision carrier FBX"),
        ExportCollisionCarrierFbx(TemplateFbx, MeshPath, MaterialName, Tokens, Error));
    if (!Error.IsEmpty()) AddError(Error);

    FMHSourceAnalysisEntry Entry;
    Entry.Key.Kind = EMHResourceKind::StaticMesh;
    Entry.Key.LogicalName = LogicalName;
    Entry.PayloadPath = MeshPath;
    Entry.SourcePath = TEXT("meshes/") + LogicalName + TEXT(".mesh.fbx");
    Entry.Change = EMHSourceChange::Create;
    bPassed &= TestTrue(TEXT("hash collision carrier FBX"), ReadSourceHash(MeshPath, Entry.RawHash));

    FStaticMeshImporterTestResolver Resolver;
    Resolver.MaterialName = MaterialName;
    Resolver.MaterialPath = MaterialPath;
    Resolver.MaterialHash = MaterialHash;
    const FMHStaticMeshOperationResult Import =
        MHImportStaticMeshV4(Entry, Resolver, Fixture.SourceRoot);
    if (!Import.Succeeded())
    {
        AddError(FString::Printf(TEXT("collision carrier import failed: %s"), *Import.Error));
        return false;
    }
    UStaticMesh* Mesh = Import.StaticMesh;

    // Carrier nodes never enter the render inventory or the material union.
    bPassed &= TestEqual(TEXT("carrier FBX has a single render LOD"), Mesh->GetNumSourceModels(), 1);
    bPassed &= TestEqual(TEXT("union stays render-only"), Mesh->GetStaticMaterials().Num(), 1);
    if (Mesh->GetStaticMaterials().Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("union slot is the render material"),
            Mesh->GetStaticMaterials()[0].MaterialSlotName,
            FName(*MaterialName));
    }
    if (const FMeshDescription* LOD0 = Mesh->GetMeshDescription(0))
    {
        bPassed &= TestEqual(TEXT("only the render triangle is drawn"), LOD0->Polygons().Num(), 1);
    }

    const UBodySetup* BodySetup = Mesh->GetBodySetup();
    bPassed &= TestNotNull(TEXT("carrier BodySetup"), BodySetup);
    if (BodySetup != nullptr)
    {
        bPassed &= TestEqual(TEXT("one convex element"), BodySetup->AggGeom.ConvexElems.Num(), 1);
        bPassed &= TestEqual(TEXT("one box element"), BodySetup->AggGeom.BoxElems.Num(), 1);
        bPassed &= TestEqual(TEXT("one capsule element"), BodySetup->AggGeom.SphylElems.Num(), 1);
        if (BodySetup->AggGeom.BoxElems.Num() == 1)
        {
            const FKBoxElem& Box = BodySetup->AggGeom.BoxElems[0];
            bPassed &= TestTrue(
                TEXT("box element centre follows the node transform"),
                Box.Center.Equals(FVector(110.0, -15.0, 20.0), 0.01));
            bPassed &= TestTrue(
                TEXT("box element carries full edge lengths"),
                FMath::IsNearlyEqual(Box.X, 20.0f, 0.01f) &&
                    FMath::IsNearlyEqual(Box.Y, 30.0f, 0.01f) &&
                    FMath::IsNearlyEqual(Box.Z, 40.0f, 0.01f));
            bPassed &= TestEqual(
                TEXT("phys carrier is physics-only"),
                Box.GetCollisionEnabled(),
                ECollisionEnabled::PhysicsOnly);
        }
        if (BodySetup->AggGeom.SphylElems.Num() == 1)
        {
            const FKSphylElem& Sphyl = BodySetup->AggGeom.SphylElems[0];
            bPassed &= TestTrue(
                TEXT("capsule element centre follows the node transform"),
                Sphyl.Center.Equals(FVector(5.0, -205.0, 30.0), 0.01));
            bPassed &= TestTrue(
                TEXT("capsule radius is half the widest cross section"),
                FMath::IsNearlyEqual(Sphyl.Radius, 5.0f, 0.01f));
            bPassed &= TestTrue(
                TEXT("capsule length excludes both hemispheres"),
                FMath::IsNearlyEqual(Sphyl.Length, 50.0f, 0.01f));
        }
        // UE 5.7 has no per-shape physical material, so the dominant token wins.
        bPassed &= TestEqual(
            TEXT("dominant phmat becomes the body physical material"),
            BodySetup->PhysMaterial.Get(),
            DominantPhysMaterial);
    }

    UStaticMesh* TraceMesh = Mesh->ComplexCollisionMesh.Get();
    bPassed &= TestNotNull(TEXT("trace carriers produce a complex collision mesh"), TraceMesh);
    if (TraceMesh != nullptr)
    {
        bPassed &= TestEqual(
            TEXT("companion lives at the deterministic collision path"),
            TraceMesh->GetPathName(),
            Cleanup.TraceObjectPath);
        bPassed &= TestEqual(
            TEXT("companion groups its sections by phmat"),
            TraceMesh->GetStaticMaterials().Num(),
            2);
        if (TraceMesh->GetStaticMaterials().Num() == 2)
        {
            bPassed &= TestEqual(
                TEXT("first companion section is the dominant phmat"),
                TraceMesh->GetStaticMaterials()[0].MaterialSlotName,
                FName(*Tokens.DominantPhmat));
            bPassed &= TestEqual(
                TEXT("second companion section is the trace-only phmat"),
                TraceMesh->GetStaticMaterials()[1].MaterialSlotName,
                FName(*Tokens.TracePhmat));
        }
    }

    // The behavioural contract: the mesh's complex collision is the trace
    // geometry (two tetrahedra), not the single render triangle.
    FTriMeshCollisionData TriMeshData;
    const bool bHasTriMesh = Mesh->GetPhysicsTriMeshData(&TriMeshData, true);
    bPassed &= TestTrue(TEXT("complex collision data is available"), bHasTriMesh);
    bPassed &= TestEqual(
        TEXT("complex collision uses the trace geometry"),
        TriMeshData.Indices.Num(),
        8);

    bPassed &= TestTrue(
        TEXT("unresolved phmat tokens warn without blocking"),
        HasWarning(Import, TEXT("MH_W_DAGOR_CONSTRUCT_DROPPED")));

    // Importer version 4 is what forces the one-time rebuild of version 3 meshes.
    UMHStaticMeshImportData* Receipt = Cast<UMHStaticMeshImportData>(Mesh->GetAssetImportData());
    bPassed &= TestNotNull(TEXT("carrier receipt exists"), Receipt);
    if (Receipt != nullptr)
    {
        bPassed &= TestEqual(TEXT("receipt records importer version 5"), Receipt->ImporterVersion, 5);
        Receipt->ImporterVersion = 3;
        const FMHStaticMeshOperationResult Upgrade =
            MHImportStaticMeshV4(Entry, Resolver, Fixture.SourceRoot);
        bPassed &= TestTrue(TEXT("version 3 receipt upgrade succeeds"), Upgrade.Succeeded());
        bPassed &= TestTrue(TEXT("version 3 receipt forces a rebuild"), Upgrade.bRebuilt);
        bPassed &= TestEqual(TEXT("version upgrade keeps the asset"), Upgrade.StaticMesh, Mesh);
    }

    // An unchanged reimport must stay on the silent NO_CHANGE fast path.
    const FMHStaticMeshOperationResult NoChange =
        MHImportStaticMeshV4(Entry, Resolver, Fixture.SourceRoot);
    bPassed &= TestTrue(TEXT("unchanged carrier reimport succeeds"), NoChange.Succeeded());
    bPassed &= TestFalse(TEXT("unchanged carrier reimport performs no rebuild"), NoChange.bRebuilt);
    bPassed &= TestTrue(TEXT("NO_CHANGE emits no warnings"), NoChange.Warnings.IsEmpty());

    if (DominantPhysMaterial != nullptr)
    {
        DominantPhysMaterial->ClearFlags(RF_Public | RF_Standalone);
        DominantPhysMaterial->MarkAsGarbage();
    }
    if (PhysMaterialPackage != nullptr)
    {
        PhysMaterialPackage->SetDirtyFlag(false);
    }
    return bPassed;
}

struct FTargetedStaticMeshReimportFixture
{
    FAutomationTestBase& Test;
    FStaticMeshImporterFixture Source;
    FGeneratedPackageCleanup Cleanup;
    FGeneratedPackageCleanup SecondCleanup;
    FDirectoryPath PreviousSourceRoot;
    FString PreviousMasterRoot;
    FString LogicalName;
    FString MaterialName;
    FString MaterialClassName;
    FString TemplateFbx;
    FString MaterialPath;
    FString MaterialHash;
    FString MeshPath;
    UStaticMesh* Mesh = nullptr;
    UMaterialInstanceConstant* ManagedMaterial = nullptr;
    UMaterial* MaterialParent = nullptr;

    explicit FTargetedStaticMeshReimportFixture(FAutomationTestBase& InTest)
        : Test(InTest)
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        PreviousSourceRoot = Settings->SourceRoot;
        PreviousMasterRoot = Settings->MasterRoot;
    }

    ~FTargetedStaticMeshReimportFixture()
    {
        MHSetGeneratedResourceChangedObserverForTests({});
        if (IsValid(ManagedMaterial))
        {
            ManagedMaterial->SetParentEditorOnly(nullptr);
        }
        if (IsValid(MaterialParent))
        {
            UPackage* ParentPackage = MaterialParent->GetOutermost();
            ObjectTools::DeleteSingleObject(MaterialParent, false);
            if (ParentPackage != nullptr)
            {
                ParentPackage->SetDirtyFlag(false);
            }
        }
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        Settings->SourceRoot = PreviousSourceRoot;
        Settings->MasterRoot = PreviousMasterRoot;
        MHShutdownProjectIndex();
    }

    bool Build()
    {
        const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        LogicalName = TEXT("targeted_mesh_") + Token;
        MaterialName = TEXT("targeted_mat_") + Token;
        MaterialClassName = TEXT("targeted_parent_") + Token;
        if (!ResolveGoldenRoot(Test, TemplateFbx))
        {
            return false;
        }
        TemplateFbx = FPaths::Combine(TemplateFbx, TEXT("fixtures/axis/axis_probe.fbx"));
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        Settings->SourceRoot.Path = Source.SourceRoot;
        Settings->MasterRoot = TEXT("/Game/Mimir/MasterMaterials");

        const FString ParentPackageName = Settings->MasterRoot + TEXT("/") + MaterialClassName;
        MaterialParent = NewObject<UMaterial>(
            CreatePackage(*ParentPackageName),
            FName(*MaterialClassName),
            RF_Public | RF_Standalone);
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get()
            .AssetCreated(MaterialParent);

        const FString MaterialPackage = TEXT("/Game/MH/Generated/Materials/") + MaterialName;
        UPackage* MaterialOuter = CreatePackage(*MaterialPackage);
        ManagedMaterial = NewObject<UMaterialInstanceConstant>(
            MaterialOuter,
            FName(*MaterialName),
            RF_Public | RF_Standalone | RF_Transactional);
        ManagedMaterial->SetParentEditorOnly(MaterialParent);
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get()
            .AssetCreated(ManagedMaterial);
        Cleanup.MaterialObjectPath = MaterialPackage + TEXT(".") + MaterialName;

        MaterialPath = FPaths::Combine(
            Source.SourceRoot,
            TEXT("materials"),
            MaterialName + TEXT(".material"));
        bool bPassed = Test.TestTrue(
            TEXT("write targeted material source"),
            WriteStaticMeshImporterUtf8(
                MaterialPath,
                FString::Printf(TEXT("{\n  \"class\": \"%s\"\n}\n"), *MaterialClassName)));
        bPassed &= Test.TestTrue(
            TEXT("hash targeted material source"),
            ReadSourceHash(MaterialPath, MaterialHash));
        UMHMaterialSourceData* MaterialReceipt = NewObject<UMHMaterialSourceData>(ManagedMaterial);
        MaterialReceipt->LogicalName = MaterialName;
        MaterialReceipt->SourceRelativePath =
            TEXT("materials/") + MaterialName + TEXT(".material");
        MaterialReceipt->SourceHash = MaterialHash;
        MaterialReceipt->AppliedHash = MaterialHash;
        MaterialReceipt->AppliedParent = TEXT("class:") + MaterialClassName;
        ManagedMaterial->AddAssetUserData(MaterialReceipt);

        Mesh = ImportMesh(LogicalName, Cleanup, false);
        return bPassed && Mesh != nullptr;
    }

    UStaticMesh* AddSecondMesh()
    {
        return ImportMesh(LogicalName + TEXT("_second"), SecondCleanup, false);
    }

    bool WriteReplacementSource(FString& OutHash)
    {
        FString Error;
        const bool bExported = ExportPlainStaticMeshFbx(
            TemplateFbx,
            MeshPath,
            MaterialName,
            true,
            Error);
        if (!Error.IsEmpty())
        {
            Test.AddError(Error);
        }
        return Test.TestTrue(TEXT("export targeted replacement FBX"), bExported) &&
            Test.TestTrue(TEXT("hash targeted replacement FBX"), ReadSourceHash(MeshPath, OutHash));
    }

private:
    UStaticMesh* ImportMesh(
        const FString& MeshLogicalName,
        FGeneratedPackageCleanup& MeshCleanup,
        const bool bReplacement)
    {
        const FString PackageName = TEXT("/Game/MH/Generated/Meshes/") + MeshLogicalName;
        MeshCleanup.MeshObjectPath = PackageName + TEXT(".") + MeshLogicalName;
        const FString Path = FPaths::Combine(
            Source.SourceRoot,
            TEXT("meshes"),
            MeshLogicalName + TEXT(".mesh.fbx"));
        FString Error;
        if (!Test.TestTrue(
                TEXT("export targeted source FBX"),
                ExportPlainStaticMeshFbx(
                    TemplateFbx,
                    Path,
                    MaterialName,
                    bReplacement,
                    Error)))
        {
            if (!Error.IsEmpty())
            {
                Test.AddError(Error);
            }
            return nullptr;
        }

        FMHSourceAnalysisEntry Entry;
        Entry.Key.Kind = EMHResourceKind::StaticMesh;
        Entry.Key.LogicalName = MeshLogicalName;
        Entry.PayloadPath = Path;
        Entry.SourcePath = TEXT("meshes/") + MeshLogicalName + TEXT(".mesh.fbx");
        Entry.Change = EMHSourceChange::Create;
        if (!Test.TestTrue(TEXT("hash targeted source FBX"), ReadSourceHash(Path, Entry.RawHash)))
        {
            return nullptr;
        }
        FStaticMeshImporterTestResolver Resolver;
        Resolver.MaterialName = MaterialName;
        Resolver.MaterialPath = MaterialPath;
        Resolver.MaterialHash = MaterialHash;
        FMHStaticMeshOperationResult Result = MHImportStaticMeshV4(
            Entry,
            Resolver,
            Source.SourceRoot);
        if (!Result.Succeeded())
        {
            Test.AddError(FString::Printf(
                TEXT("targeted fixture import failed: %s"),
                *Result.Error));
            return nullptr;
        }
        if (MeshLogicalName == LogicalName)
        {
            MeshPath = Path;
        }
        return Result.StaticMesh;
    }
};

TArray<FVector3f> CaptureLOD0Positions(const UStaticMesh& Mesh)
{
    TArray<FVector3f> Result;
    const FMeshDescription* Description = Mesh.GetMeshDescription(0);
    if (Description == nullptr)
    {
        return Result;
    }
    const FStaticMeshConstAttributes Attributes(*Description);
    const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
    for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
    {
        Result.Add(Positions[VertexId]);
    }
    return Result;
}

UMHCompositeAsset* BuildTargetedPlacementAsset(
    FAutomationTestBase& Test,
    const FString& CompositeName,
    const FString& MeshName)
{
    FMHCompositeDocument Document;
    FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
    Leaf.Kind = EMHCompositeNodeKind::Mesh;
    Leaf.Resource = MeshName;
    TArray<uint8> CanonicalBytes;
    FString Error;
    if (!MHWriteCanonicalCompositeV5(Document, CanonicalBytes, Error))
    {
        Test.AddError(TEXT("cannot write targeted placement composite: ") + Error);
        return nullptr;
    }
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
        CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + CompositeName)),
        FName(*CompositeName),
        RF_Public | RF_Standalone);
    if (!MHApplyCompositeV5(*Asset, Document, Error))
    {
        Test.AddError(TEXT("cannot apply targeted placement composite: ") + Error);
        return nullptr;
    }
    Asset->LogicalName = CompositeName;
    Asset->SourceRelativePath = CompositeName + TEXT(".composite");
    Asset->SourceHash = MHRawPayloadHash(CanonicalBytes);
    Asset->AppliedHash = Asset->SourceHash;
    return Asset;
}

UMHCompositeAsset* BuildPerfRandomPlacementAsset(
    FAutomationTestBase& Test,
    const FString& CompositeName,
    const FString& FirstMeshName,
    const FString& SecondMeshName)
{
    FMHCompositeDocument Document;
    FMHCompositeNode& Random = Document.Nodes.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    FMHCompositeOption& First = Random.Options.AddDefaulted_GetRef();
    First.Kind = EMHCompositeOptionKind::Mesh;
    First.Resource = FirstMeshName;
    First.Weight = 1.0f;
    FMHCompositeOption& Second = Random.Options.AddDefaulted_GetRef();
    Second.Kind = EMHCompositeOptionKind::Mesh;
    Second.Resource = SecondMeshName;
    Second.Weight = 1.0f;

    TArray<uint8> CanonicalBytes;
    FString Error;
    if (!MHWriteCanonicalCompositeV5(Document, CanonicalBytes, Error))
    {
        Test.AddError(TEXT("cannot write perf placement composite: ") + Error);
        return nullptr;
    }
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
        CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + CompositeName)),
        FName(*CompositeName),
        RF_Public | RF_Standalone);
    if (!MHApplyCompositeV5(*Asset, Document, Error))
    {
        Test.AddError(TEXT("cannot apply perf placement composite: ") + Error);
        return nullptr;
    }
    Asset->LogicalName = CompositeName;
    Asset->SourceRelativePath = CompositeName + TEXT(".composite");
    Asset->SourceHash = MHRawPayloadHash(CanonicalBytes);
    Asset->AppliedHash = Asset->SourceHash;
    return Asset;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPerformanceInstrumentationCountersTest,
    "Mimir.V5.Composite.Perf.InstrumentationCounters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPerformanceInstrumentationCountersTest::RunTest(const FString& Parameters)
{
    IConsoleVariable* PerfTrace =
        IConsoleManager::Get().FindConsoleVariable(TEXT("mh.PerfTrace"));
    if (!TestNotNull(TEXT("mh.PerfTrace cvar exists"), PerfTrace))
    {
        return false;
    }
    const int32 PreviousTrace = PerfTrace->GetInt();
    ON_SCOPE_EXIT
    {
        PerfTrace->Set(PreviousTrace, ECVF_SetByCode);
        MHResetPerformanceTraceForTests();
    };

    FTargetedStaticMeshReimportFixture Fixture(*this);
    if (!Fixture.Build())
    {
        return false;
    }
    UStaticMesh* SecondMesh = Fixture.AddSecondMesh();
    if (!TestNotNull(TEXT("second all-options mesh"), SecondMesh))
    {
        return false;
    }
    const FString CompositeName = Fixture.LogicalName + TEXT("_perf");
    UMHCompositeAsset* PlacementAsset = BuildPerfRandomPlacementAsset(
        *this,
        CompositeName,
        Fixture.LogicalName,
        SecondMesh->GetName());
    if (!TestNotNull(TEXT("perf random composite"), PlacementAsset))
    {
        return false;
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("perf placement world"), World))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        World->DestroyWorld(true);
        PlacementAsset->ClearFlags(RF_Public | RF_Standalone);
        PlacementAsset->MarkAsGarbage();
    };
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("perf placement actor"), Actor))
    {
        return false;
    }
    Actor->SetAutoSeed(false);
    Actor->SetSeed(7);
    Actor->SetCompositeAsset(PlacementAsset);

    bool bPassed = true;
    PerfTrace->Set(0, ECVF_SetByCode);
    MHResetPerformanceTraceForTests();
    {
        FMHMapLoadInitialBuildScope Scope(*Actor);
        Actor->RebuildComposite();
        Scope.Complete(*Actor);
    }
    MHFlushMapLoadPerfReport();
    bPassed &= TestEqual(
        TEXT("trace zero emits no map-load report"),
        MHGetMapLoadPerfReportForTests().EmittedReports,
        0ull);
    FMHSourceAnalysis OffAnalysis;
    FString OffError;
    bPassed &= TestTrue(
        TEXT("trace-zero source scan still succeeds"),
        MHScanSourcesOperation(
            Fixture.Source.SourceRoot,
            OffAnalysis,
            OffError,
            EMHPerfScanTrigger::Startup));
    bPassed &= TestEqual(
        TEXT("trace zero emits no scan report"),
        MHGetStartupScanPerfReportForTests().EmittedReports,
        0ull);
    UMHSourceImporter* OffImporter = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>()
        : nullptr;
    if (!TestNotNull(TEXT("trace-zero source importer subsystem"), OffImporter))
    {
        return false;
    }
    TArray<FString> OffWarnings;
    OffError.Reset();
    bPassed &= TestTrue(
        TEXT("trace-zero targeted reimport still succeeds"),
        OffImporter->ReimportStaticMesh(Fixture.Mesh, OffWarnings, OffError));
    bPassed &= TestEqual(
        TEXT("trace zero emits no reimport report"),
        MHGetReimportPerfReportForTests().EmittedReports,
        0ull);

    PerfTrace->Set(1, ECVF_SetByCode);
    MHResetPerformanceTraceForTests();
    MHResetPlacementStageMetrics();
    MHResetDefinitionCacheMetrics();
    MHResetPlacementMutationMetrics();
    if (GEditor != nullptr)
    {
        if (UMHCompositeDefinitionSubsystem* Definitions =
                GEditor->GetEditorSubsystem<UMHCompositeDefinitionSubsystem>())
        {
            Definitions->InvalidateAllDefinitions();
        }
    }
    {
        FMHMapLoadInitialBuildScope Scope(*Actor);
        Actor->RebuildComposite();
        Scope.Complete(*Actor);
    }
    MHFlushMapLoadPerfReport();
    const FMHMapLoadPerfReport MapReport = MHGetMapLoadPerfReportForTests();
    bPassed &= TestEqual(TEXT("all-options unique meshes"), MapReport.AllOptionUniqueMeshes, 2ull);
    bPassed &= TestEqual(TEXT("selected unique meshes"), MapReport.SelectedUniqueMeshes, 1ull);
    bPassed &= TestTrue(
        TEXT("all-options exceeds selected meshes"),
        MapReport.AllOptionUniqueMeshes > MapReport.SelectedUniqueMeshes);
    bPassed &= TestTrue(TEXT("BuildAppliedGraph calls are captured"), MapReport.BuildAppliedGraphMs >= 0.0 &&
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::BuildAppliedGraph).Calls > 0);
    bPassed &= TestTrue(TEXT("ResolveCompositePlan calls are captured"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::ResolveCompositePlan).Calls > 0);
    bPassed &= TestTrue(TEXT("CompilePlacement calls are captured"),
        MHGetPlacementStageMetrics().Get(EMHPlacementStage::CompilePlacement).Calls > 0);
    bPassed &= TestEqual(TEXT("trace one emits one map-load report"), MapReport.EmittedReports, 1ull);

    MHResetPerformanceTraceForTests();
    FMHSourceAnalysis Analysis;
    FString Error;
    bPassed &= TestTrue(
        TEXT("instrumented manual source scan succeeds"),
        MHScanSourcesOperation(
            Fixture.Source.SourceRoot,
            Analysis,
            Error,
            EMHPerfScanTrigger::Startup));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    const FMHStartupScanPerfReport ScanReport = MHGetStartupScanPerfReportForTests();
    bPassed &= TestEqual(TEXT("startup scan trigger"), ScanReport.Trigger, FString(TEXT("startup")));
    bPassed &= TestEqual(TEXT("manual scan records one full scan"), ScanReport.FullScanCountDelta, 1ll);
    bPassed &= TestEqual(TEXT("manual scan uses two snapshot passes"), ScanReport.ScanPasses, 2ull);
    bPassed &= TestEqual(TEXT("trace one emits one scan report"), ScanReport.EmittedReports, 1ull);

    MHResetPerformanceTraceForTests();
    UMHSourceImporter* Importer = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>()
        : nullptr;
    if (!TestNotNull(TEXT("source importer subsystem"), Importer))
    {
        return false;
    }
    TArray<FString> Warnings;
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("instrumented targeted reimport succeeds"),
        Importer->ReimportStaticMesh(Fixture.Mesh, Warnings, Error));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    const FMHReimportPerfReport ReimportReport = MHGetReimportPerfReportForTests();
    bPassed &= TestEqual(
        TEXT("targeted reimport records current full scan"),
        ReimportReport.FullScanCountDelta,
        1ll);
    bPassed &= TestEqual(
        TEXT("targeted reimport records no incremental paths"),
        ReimportReport.IncrementalPaths,
        0ull);
    bPassed &= TestEqual(
        TEXT("trace one emits one reimport report"),
        ReimportReport.EmittedReports,
        1ull);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPerformanceEndpointCountersTest,
    "Mimir.V5.Composite.Perf.EndpointCounters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPerformanceEndpointCountersTest::RunTest(const FString& Parameters)
{
    // Recipe Model M0: the map-load report exposes how the current resolve
    // path finds endpoints (registry lookups, Asset Registry tag queries,
    // synchronous package loads, identity admissions, live receipt tag reads).
    // R0 replaces that path and asserts the forbidden-in-preview counters at 0.
    IConsoleVariable* PerfTrace =
        IConsoleManager::Get().FindConsoleVariable(TEXT("mh.PerfTrace"));
    if (!TestNotNull(TEXT("mh.PerfTrace cvar exists"), PerfTrace))
    {
        return false;
    }
    const int32 PreviousTrace = PerfTrace->GetInt();
    ON_SCOPE_EXIT
    {
        PerfTrace->Set(PreviousTrace, ECVF_SetByCode);
        MHResetPerformanceTraceForTests();
    };

    FTargetedStaticMeshReimportFixture Fixture(*this);
    if (!Fixture.Build())
    {
        return false;
    }
    UStaticMesh* SecondMesh = Fixture.AddSecondMesh();
    if (!TestNotNull(TEXT("second all-options mesh"), SecondMesh))
    {
        return false;
    }
    UMHCompositeAsset* PlacementAsset = BuildPerfRandomPlacementAsset(
        *this,
        Fixture.LogicalName + TEXT("_endpoints"),
        Fixture.LogicalName,
        SecondMesh->GetName());
    if (!TestNotNull(TEXT("endpoint random composite"), PlacementAsset))
    {
        return false;
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("endpoint placement world"), World))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        World->DestroyWorld(true);
        PlacementAsset->ClearFlags(RF_Public | RF_Standalone);
        PlacementAsset->MarkAsGarbage();
    };
    AMHCompositeActor* First = World->SpawnActor<AMHCompositeActor>();
    AMHCompositeActor* Second = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("first placement actor"), First) ||
        !TestNotNull(TEXT("second placement actor"), Second))
    {
        return false;
    }
    for (AMHCompositeActor* Actor : {First, Second})
    {
        Actor->SetAutoSeed(false);
        Actor->SetSeed(7);
        Actor->SetCompositeAsset(PlacementAsset);
    }

    const auto InvalidateDefinitions = []()
    {
        if (GEditor != nullptr)
        {
            if (UMHCompositeDefinitionSubsystem* Definitions =
                    GEditor->GetEditorSubsystem<UMHCompositeDefinitionSubsystem>())
            {
                Definitions->InvalidateAllDefinitions();
            }
        }
    };
    const auto BuildActor = [](AMHCompositeActor& Actor)
    {
        {
            FMHMapLoadInitialBuildScope Scope(Actor);
            Actor.RebuildComposite();
            Scope.Complete(Actor);
        }
        MHFlushMapLoadPerfReport();
        return MHGetMapLoadPerfReportForTests();
    };
    const auto ResetAll = []()
    {
        MHResetPerformanceTraceForTests();
        MHResetEndpointResolveMetrics();
        MHResetDefinitionCacheMetrics();
    };
    // R0a: endpoints are admitted once per key per session; a cold pass must
    // also forget the prototypes, otherwise every resolve is a registry hit.
    const auto InvalidateEndpoints = []()
    {
        if (UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get())
        {
            Registry->InvalidateAll();
        }
    };

    bool bPassed = true;
    PerfTrace->Set(0, ECVF_SetByCode);
    ResetAll();
    InvalidateDefinitions();
    const FMHMapLoadPerfReport Off = BuildActor(*First);
    bPassed &= TestEqual(TEXT("trace zero emits no map-load report"), Off.EmittedReports, 0ull);
    bPassed &= TestEqual(TEXT("trace zero reports no registry lookups"), Off.RegistryLookups, 0ull);
    bPassed &= TestEqual(TEXT("trace zero reports no identity admissions"), Off.IdentityAdmissions, 0ull);

    PerfTrace->Set(1, ECVF_SetByCode);
    ResetAll();
    InvalidateDefinitions();
    InvalidateEndpoints();
    const FMHMapLoadPerfReport Cold = BuildActor(*First);
    // Root composite plus every mesh option: the closure resolves all of them.
    const uint64 UniqueEndpointKeys = Cold.AllOptionUniqueMeshes + 1ull;
    bPassed &= TestEqual(TEXT("cold build emits one map-load report"), Cold.EmittedReports, 1ull);
    bPassed &= TestTrue(TEXT("cold build misses the definition cache"), Cold.DefinitionCacheMisses >= 1ull);
    bPassed &= TestTrue(
        TEXT("cold build resolves at least every unique endpoint key"),
        Cold.RegistryLookups >= UniqueEndpointKeys);
    bPassed &= TestEqual(
        TEXT("current resolve path queries the Asset Registry by tags once per lookup"),
        Cold.AssetRegistryTagQueries,
        Cold.RegistryLookups);
    bPassed &= TestTrue(
        TEXT("cold build admits at least every unique endpoint key"),
        Cold.IdentityAdmissions >= UniqueEndpointKeys);
    bPassed &= TestEqual(
        TEXT("current admission reads live receipt tags once per admission"),
        Cold.LiveReceiptTagReads,
        Cold.IdentityAdmissions);
    bPassed &= TestTrue(
        TEXT("sync package loads never exceed lookups"),
        Cold.PackageLoadsSync <= Cold.RegistryLookups);

    ResetAll();
    const FMHMapLoadPerfReport Warm = BuildActor(*Second);
    bPassed &= TestTrue(TEXT("warm build hits the definition cache"), Warm.DefinitionCacheHits >= 1ull);
    bPassed &= TestEqual(TEXT("warm build does not miss the definition cache"), Warm.DefinitionCacheMisses, 0ull);
    bPassed &= TestTrue(
        TEXT("warm build resolves fewer endpoints than cold"),
        Warm.RegistryLookups < Cold.RegistryLookups);
    bPassed &= TestTrue(
        TEXT("warm build admits fewer endpoints than cold"),
        Warm.IdentityAdmissions < Cold.IdentityAdmissions);
    bPassed &= TestEqual(
        TEXT("warm build still queries the Asset Registry once per lookup"),
        Warm.AssetRegistryTagQueries,
        Warm.RegistryLookups);
    AddInfo(FString::Printf(
        TEXT("MH_PERF_ENDPOINTS cold: unique_keys=%llu registry_lookups=%llu asset_registry_tag_queries=%llu package_loads_sync=%llu identity_admissions=%llu live_receipt_tag_reads=%llu; warm: registry_lookups=%llu asset_registry_tag_queries=%llu package_loads_sync=%llu identity_admissions=%llu live_receipt_tag_reads=%llu"),
        UniqueEndpointKeys,
        Cold.RegistryLookups,
        Cold.AssetRegistryTagQueries,
        Cold.PackageLoadsSync,
        Cold.IdentityAdmissions,
        Cold.LiveReceiptTagReads,
        Warm.RegistryLookups,
        Warm.AssetRegistryTagQueries,
        Warm.PackageLoadsSync,
        Warm.IdentityAdmissions,
        Warm.LiveReceiptTagReads));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHTargetedStaticMeshReimportAdmissionTest,
    "Mimir.V4.StaticMesh.TargetedReimport.Admission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHTargetedStaticMeshReimportAdmissionTest::RunTest(const FString& Parameters)
{
    FTargetedStaticMeshReimportFixture Fixture(*this);
    if (!Fixture.Build())
    {
        return false;
    }
    FReimportHandler* Handler = MHGetManagedStaticMeshReimportHandlerForTests();
    bool bPassed = TestNotNull(TEXT("managed static-mesh handler is registered"), Handler);
    TArray<FString> Filenames;
    bPassed &= TestTrue(
        TEXT("managed canonical static mesh is admitted"),
        Handler != nullptr && Handler->CanReimport(Fixture.Mesh, Filenames));
    bPassed &= TestEqual(TEXT("managed handler reports one source file"), Filenames.Num(), 1);
    if (Filenames.Num() == 1)
    {
        bPassed &= TestTrue(
            TEXT("reported source comes from receipt relative path"),
            FPaths::IsSamePath(Filenames[0], Fixture.MeshPath));
    }

    UStaticMesh* Foreign = NewObject<UStaticMesh>(GetTransientPackage());
    Filenames.Reset();
    bPassed &= TestFalse(
        TEXT("foreign static mesh without receipt is rejected"),
        Handler != nullptr && Handler->CanReimport(Foreign, Filenames));
    UMHStaticMeshImportData* CopiedReceipt = NewObject<UMHStaticMeshImportData>(Foreign);
    CopiedReceipt->LogicalName = Fixture.LogicalName;
    CopiedReceipt->SourceRelativePath =
        TEXT("meshes/") + Fixture.LogicalName + TEXT(".mesh.fbx");
    Foreign->SetAssetImportData(CopiedReceipt);
    bPassed &= TestFalse(
        TEXT("noncanonical copy with a managed receipt is rejected"),
        Handler != nullptr && Handler->CanReimport(Foreign, Filenames));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHTargetedStaticMeshForceReimportTest,
    "Mimir.V4.StaticMesh.TargetedReimport.ForceAndNotify",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHTargetedStaticMeshForceReimportTest::RunTest(const FString& Parameters)
{
    FTargetedStaticMeshReimportFixture Fixture(*this);
    if (!Fixture.Build())
    {
        return false;
    }
    UMHStaticMeshImportData* Receipt =
        Cast<UMHStaticMeshImportData>(Fixture.Mesh->GetAssetImportData());
    if (!TestNotNull(TEXT("targeted fixture receipt"), Receipt))
    {
        return false;
    }
    const FString InitialHash = Receipt->SourceHash;
    const FString CompositeName = Fixture.LogicalName + TEXT("_placement");
    UMHCompositeAsset* PlacementAsset = BuildTargetedPlacementAsset(
        *this,
        CompositeName,
        Fixture.LogicalName);
    if (!TestNotNull(TEXT("targeted placement composite"), PlacementAsset))
    {
        return false;
    }
    UWorld* PlacementWorld = UWorld::CreateWorld(EWorldType::EditorPreview, true);
    if (!TestNotNull(TEXT("targeted placement world"), PlacementWorld))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        PlacementWorld->DestroyWorld(true);
        PlacementAsset->ClearFlags(RF_Public | RF_Standalone);
        PlacementAsset->MarkAsGarbage();
    };
    AMHCompositeActor* PlacementActor = PlacementWorld->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("targeted placed composite actor"), PlacementActor))
    {
        return false;
    }
    PlacementActor->SetAutoSeed(false);
    PlacementActor->SetCompositeAsset(PlacementAsset);
    const uint32 InitialPlacementRebuilds = PlacementActor->GetPlacementRebuildCount();
    bool bPassed = TestEqual(
        TEXT("targeted placement begins with one leaf"),
        PlacementActor->GetLeafPlacementComponents().Num(),
        1);
    FString ReplacementHash;
    if (!Fixture.WriteReplacementSource(ReplacementHash))
    {
        return false;
    }
    bPassed &= TestNotEqual(TEXT("replacement source hash changes"), ReplacementHash, InitialHash);

    TArray<FMHResourceKey> Notifications;
    MHSetGeneratedResourceChangedObserverForTests(
        [&Notifications](const FMHResourceKey& Key)
        {
            Notifications.Add(Key);
        });
    MHResetSourceImportMetrics();
    const bool bChangedResult = FReimportManager::Instance()->Reimport(
        Fixture.Mesh,
        false,
        false,
        FString(),
        nullptr,
        INDEX_NONE,
        false,
        true);
    bPassed &= TestTrue(TEXT("standard Reimport applies changed managed source"), bChangedResult);
    Receipt = Cast<UMHStaticMeshImportData>(Fixture.Mesh->GetAssetImportData());
    bPassed &= TestNotNull(TEXT("managed receipt survives standard Reimport"), Receipt);
    if (Receipt != nullptr)
    {
        bPassed &= TestEqual(TEXT("changed Reimport advances receipt hash"), Receipt->SourceHash, ReplacementHash);
    }
    bPassed &= VerifyReplacementMesh(*this, *Fixture.Mesh);
    FMHSourceImportMetrics Metrics = MHGetSourceImportMetrics();
    bPassed &= TestEqual(
        TEXT("single force-reimport has one batch compilation wait"),
        Metrics.Get(EMHSourceImportMetricResource::Batch, EMHSourceImportMetricStage::BuildWait).Calls,
        1ull);
    bPassed &= TestEqual(
        TEXT("single force-reimport has one batch save"),
        Metrics.Get(EMHSourceImportMetricResource::Batch, EMHSourceImportMetricStage::SavePackage).Calls,
        1ull);
    bPassed &= TestEqual(TEXT("changed Reimport emits one placement notification"), Notifications.Num(), 1);
    if (Notifications.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("placement notification carries mesh key"), Notifications[0].LogicalName, Fixture.LogicalName);
    }
    bPassed &= TestEqual(
        TEXT("changed mesh reimport rebuilds the existing placed composite"),
        PlacementActor->GetPlacementRebuildCount(),
        InitialPlacementRebuilds + 1);
    const TArray<TObjectPtr<USceneComponent>>& ChangedLeaves =
        PlacementActor->GetLeafPlacementComponents();
    UStaticMeshComponent* ChangedComponent = ChangedLeaves.Num() == 1
        ? Cast<UStaticMeshComponent>(ChangedLeaves[0])
        : nullptr;
    bPassed &= TestNotNull(TEXT("rebuilt placement keeps a static-mesh leaf"), ChangedComponent);
    if (ChangedComponent != nullptr)
    {
        bPassed &= TestEqual(
            TEXT("rebuilt placement uses the force-reimported mesh"),
            ChangedComponent->GetStaticMesh().Get(),
            Fixture.Mesh);
    }

    const TArray<FVector3f> FirstPositions = CaptureLOD0Positions(*Fixture.Mesh);
    UStaticMeshSocket* LocalSocket = NewObject<UStaticMeshSocket>(Fixture.Mesh);
    LocalSocket->SocketName = TEXT("local_only");
    Fixture.Mesh->Sockets.Add(LocalSocket);
    if (Receipt != nullptr)
    {
        Receipt->bLocallyModified = true;
    }
    Notifications.Reset();
    MHResetSourceImportMetrics();
    const bool bUnchangedResult = FReimportManager::Instance()->Reimport(
        Fixture.Mesh,
        false,
        false,
        FString(),
        nullptr,
        INDEX_NONE,
        false,
        true);
    bPassed &= TestTrue(TEXT("unchanged source is force-reimported"), bUnchangedResult);
    Receipt = Cast<UMHStaticMeshImportData>(Fixture.Mesh->GetAssetImportData());
    if (Receipt != nullptr)
    {
        bPassed &= TestEqual(TEXT("unchanged force keeps deterministic hash"), Receipt->SourceHash, ReplacementHash);
        bPassed &= TestFalse(TEXT("unchanged force clears local-edit marker"), Receipt->bLocallyModified);
    }
    bPassed &= TestTrue(
        TEXT("unchanged force reproduces geometry"),
        CaptureLOD0Positions(*Fixture.Mesh) == FirstPositions);
    bPassed &= VerifyReplacementMesh(*this, *Fixture.Mesh);
    Metrics = MHGetSourceImportMetrics();
    bPassed &= TestEqual(
        TEXT("unchanged force still waits for one batch build"),
        Metrics.Get(EMHSourceImportMetricResource::Batch, EMHSourceImportMetricStage::BuildWait).Calls,
        1ull);
    bPassed &= TestEqual(
        TEXT("unchanged force still saves one batch"),
        Metrics.Get(EMHSourceImportMetricResource::Batch, EMHSourceImportMetricStage::SavePackage).Calls,
        1ull);
    bPassed &= TestEqual(TEXT("unchanged force still notifies placements"), Notifications.Num(), 1);
    bPassed &= TestEqual(
        TEXT("unchanged force also refreshes the existing placed composite"),
        PlacementActor->GetPlacementRebuildCount(),
        InitialPlacementRebuilds + 2);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHTargetedStaticMeshMissingSourceTest,
    "Mimir.V4.StaticMesh.TargetedReimport.MissingSourceDoesNotMutate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHTargetedStaticMeshMissingSourceTest::RunTest(const FString& Parameters)
{
    FTargetedStaticMeshReimportFixture Fixture(*this);
    if (!Fixture.Build())
    {
        return false;
    }
    const FString PackageName = FPackageName::ObjectPathToPackageName(Fixture.Mesh->GetPathName());
    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        PackageName,
        FPackageName::GetAssetPackageExtension());
    TArray<uint8> PackageBefore;
    bool bPassed = TestTrue(
        TEXT("read package before missing-source reimport"),
        FFileHelper::LoadFileToArray(PackageBefore, *PackageFilename));
    const TArray<FVector3f> PositionsBefore = CaptureLOD0Positions(*Fixture.Mesh);
    const UMHStaticMeshImportData* ReceiptBefore =
        Cast<UMHStaticMeshImportData>(Fixture.Mesh->GetAssetImportData());
    const FString HashBefore = ReceiptBefore != nullptr ? ReceiptBefore->SourceHash : FString();

    const FString BackupPath = Fixture.MeshPath + TEXT(".missing-test-backup");
    bPassed &= TestTrue(
        TEXT("move source aside"),
        IFileManager::Get().Move(*BackupPath, *Fixture.MeshPath, true, true, false, true));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Move(*Fixture.MeshPath, *BackupPath, true, true, false, true);
    };

    UMHSourceImporter* Importer = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>()
        : nullptr;
    bPassed &= TestNotNull(TEXT("source importer subsystem exists"), Importer);
    TArray<FString> Warnings;
    FString Error;
    const bool bResult = Importer != nullptr &&
        Importer->ReimportStaticMesh(Fixture.Mesh, Warnings, Error);
    bPassed &= TestFalse(TEXT("missing source fails reimport"), bResult);
    bPassed &= TestTrue(TEXT("missing-source error reports exact path"), Error.Contains(Fixture.MeshPath));

    TArray<uint8> PackageAfter;
    bPassed &= TestTrue(
        TEXT("read package after missing-source refusal"),
        FFileHelper::LoadFileToArray(PackageAfter, *PackageFilename));
    bPassed &= TestTrue(TEXT("missing source leaves package byte-identical"), PackageAfter == PackageBefore);
    bPassed &= TestTrue(
        TEXT("missing source leaves live geometry unchanged"),
        CaptureLOD0Positions(*Fixture.Mesh) == PositionsBefore);
    const UMHStaticMeshImportData* ReceiptAfter =
        Cast<UMHStaticMeshImportData>(Fixture.Mesh->GetAssetImportData());
    bPassed &= TestNotNull(TEXT("missing-source refusal preserves receipt"), ReceiptAfter);
    if (ReceiptAfter != nullptr)
    {
        bPassed &= TestEqual(TEXT("missing-source refusal preserves hash"), ReceiptAfter->SourceHash, HashBefore);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHTargetedStaticMeshMultiSelectionTest,
    "Mimir.V4.StaticMesh.TargetedReimport.SequentialMultiSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHTargetedStaticMeshMultiSelectionTest::RunTest(const FString& Parameters)
{
    FTargetedStaticMeshReimportFixture Fixture(*this);
    if (!Fixture.Build())
    {
        return false;
    }
    UStaticMesh* Second = Fixture.AddSecondMesh();
    if (!TestNotNull(TEXT("second managed mesh"), Second))
    {
        return false;
    }
    FReimportHandler* Handler = MHGetManagedStaticMeshReimportHandlerForTests();
    if (!TestNotNull(TEXT("managed handler for multi-selection"), Handler))
    {
        return false;
    }
    TArray<UObject*> Meshes{Fixture.Mesh, Second};
    TArray<FMHResourceKey> Notifications;
    MHSetGeneratedResourceChangedObserverForTests(
        [&Notifications](const FMHResourceKey& Key)
        {
            Notifications.Add(Key);
        });
    MHResetSourceImportMetrics();
    const double Start = FPlatformTime::Seconds();
    const bool bResult = FReimportManager::Instance()->ReimportMultiple(
        Meshes,
        false,
        false,
        FString(),
        Handler,
        INDEX_NONE,
        false,
        true);
    const double WallMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
    AddInfo(FString::Printf(
        TEXT("TARGETED_REIMPORT_MULTI resources=2 wall_ms=%.6f policy=sequential_single_batches"),
        WallMilliseconds));
    const FMHSourceImportMetrics Metrics = MHGetSourceImportMetrics();
    bool bPassed = TestTrue(TEXT("two selected managed meshes reimport"), bResult);
    bPassed &= TestEqual(
        TEXT("two sequential single batches perform two waits"),
        Metrics.Get(EMHSourceImportMetricResource::Batch, EMHSourceImportMetricStage::BuildWait).Calls,
        2ull);
    bPassed &= TestEqual(
        TEXT("two sequential single batches perform two saves"),
        Metrics.Get(EMHSourceImportMetricResource::Batch, EMHSourceImportMetricStage::SavePackage).Calls,
        2ull);
    bPassed &= TestEqual(TEXT("two successful resources notify twice"), Notifications.Num(), 2);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
