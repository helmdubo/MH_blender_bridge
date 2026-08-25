#include "StaticMesh/MHStaticMeshImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "HAL/FileManager.h"
#include "MHGoldenRoot.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceResolver.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMeshAttributes.h"
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

    ~FGeneratedPackageCleanup()
    {
        CleanupObject(MeshObjectPath);
        CleanupObject(MaterialObjectPath);
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

FbxNode* AddMeshNode(FbxScene& Scene, const char* Name, FbxMesh& Mesh)
{
    FbxNode* Node = FbxNode::Create(&Scene, Name);
    Node->SetNodeAttribute(&Mesh);
    Scene.GetRootNode()->AddChild(Node);
    return Node;
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
    }
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

} // namespace UE::MimirComposite::Tests
