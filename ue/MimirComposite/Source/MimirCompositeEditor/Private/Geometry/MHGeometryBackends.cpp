#include "Geometry/MHGeometryBackends.h"

#include "AssetImportTask.h"
#include "Engine/StaticMesh.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "MeshDescription.h"
#include "Misc/Paths.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace
{
FVector3f AxisProbeToUnrealPosition(const FbxVector4& Position)
{
    return FVector3f(
        static_cast<float>(Position[0]),
        static_cast<float>(-Position[1]),
        static_cast<float>(Position[2]));
}

FVector3f ToUnrealDirection(const FbxVector4& Direction)
{
    return AxisProbeToUnrealPosition(Direction).GetSafeNormal();
}

void CaptureMeshResult(UStaticMesh& StaticMesh, FMHGeometryProbeResult& OutResult)
{
    if (const FMeshDescription* MeshDescription = StaticMesh.GetMeshDescription(0))
    {
        OutResult.MeshDescriptionVertexCount = MeshDescription->Vertices().Num();
        OutResult.MeshDescriptionPolygonCount = MeshDescription->Polygons().Num();

        const FStaticMeshConstAttributes Attributes(*MeshDescription);
        const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
        OutResult.MeshDescriptionPositions.Reserve(OutResult.MeshDescriptionVertexCount);
        for (const FVertexID VertexId : MeshDescription->Vertices().GetElementIDs())
        {
            OutResult.MeshDescriptionPositions.Add(Positions[VertexId]);
        }
    }

    if (const FStaticMeshRenderData* RenderData = StaticMesh.GetRenderData();
        RenderData && !RenderData->LODResources.IsEmpty())
    {
        const FStaticMeshLODResources& Lod = RenderData->LODResources[0];
        OutResult.RenderVertexCount = Lod.GetNumVertices();
        OutResult.RenderTriangleCount = Lod.GetNumTriangles();
        OutResult.RenderPositions.Reserve(OutResult.RenderVertexCount);
        for (uint32 Index = 0; Index < static_cast<uint32>(OutResult.RenderVertexCount); ++Index)
        {
            OutResult.RenderPositions.Add(Lod.VertexBuffers.PositionVertexBuffer.VertexPosition(Index));
        }
    }

    OutResult.StaticMesh = &StaticMesh;
}

FbxNode* FindFirstMeshNode(FbxNode* Node)
{
    if (!Node)
    {
        return nullptr;
    }

    if (Node->GetMesh())
    {
        return Node;
    }

    for (int32 ChildIndex = 0; ChildIndex < Node->GetChildCount(); ++ChildIndex)
    {
        if (FbxNode* MeshNode = FindFirstMeshNode(Node->GetChild(ChildIndex)))
        {
            return MeshNode;
        }
    }

    return nullptr;
}

bool ReadRawFbxCounts(
    const FString& Filename,
    int32& OutVertexCount,
    int32& OutPolygonCount,
    FString& OutError)
{
    FbxManager* Manager = FbxManager::Create();
    if (!Manager)
    {
        OutError = TEXT("FbxManager::Create failed while reading raw topology");
        return false;
    }

    FbxIOSettings* IOSettings = FbxIOSettings::Create(Manager, IOSROOT);
    Manager->SetIOSettings(IOSettings);
    FbxImporter* Importer = FbxImporter::Create(Manager, "MHRawTopologyImporter");
    FbxScene* Scene = FbxScene::Create(Manager, "MHRawTopologyScene");
    const FTCHARToUTF8 FilenameUtf8(*Filename);
    const bool bInitialized = Importer->Initialize(FilenameUtf8.Get(), -1, Manager->GetIOSettings());
    const bool bImported = bInitialized && Importer->Import(Scene);
    if (!bImported)
    {
        OutError = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }

    FbxNode* MeshNode = FindFirstMeshNode(Scene->GetRootNode());
    FbxMesh* Mesh = MeshNode ? MeshNode->GetMesh() : nullptr;
    if (!Mesh)
    {
        OutError = TEXT("FBX scene contains no mesh node while reading raw topology");
        Manager->Destroy();
        return false;
    }

    OutVertexCount = Mesh->GetControlPointsCount();
    OutPolygonCount = Mesh->GetPolygonCount();
    Manager->Destroy();
    return true;
}

bool BuildProbeMeshDescription(FbxMesh& Mesh, FMeshDescription& OutMeshDescription, FString& OutError)
{
    FStaticMeshAttributes Attributes(OutMeshDescription);
    Attributes.Register();

    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
    TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
    UVs.SetNumChannels(1);
    TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

    const FPolygonGroupID PolygonGroupId = OutMeshDescription.CreatePolygonGroup();
    SlotNames[PolygonGroupId] = TEXT("Default");

    const int32 ControlPointCount = Mesh.GetControlPointsCount();
    TArray<FVertexID> VertexIds;
    VertexIds.Reserve(ControlPointCount);
    for (int32 ControlPointIndex = 0; ControlPointIndex < ControlPointCount; ++ControlPointIndex)
    {
        const FVertexID VertexId = OutMeshDescription.CreateVertex();
        VertexIds.Add(VertexId);
        Positions[VertexId] = AxisProbeToUnrealPosition(Mesh.GetControlPointAt(ControlPointIndex));
    }

    FbxStringList UVSetNames;
    Mesh.GetUVSetNames(UVSetNames);
    const char* UVSetName = UVSetNames.GetCount() > 0 ? UVSetNames.GetStringAt(0) : nullptr;

    for (int32 PolygonIndex = 0; PolygonIndex < Mesh.GetPolygonCount(); ++PolygonIndex)
    {
        const int32 PolygonSize = Mesh.GetPolygonSize(PolygonIndex);
        if (PolygonSize < 3)
        {
            OutError = FString::Printf(TEXT("FBX polygon %d has fewer than three vertices"), PolygonIndex);
            return false;
        }

        TArray<FVertexInstanceID> Perimeter;
        Perimeter.Reserve(PolygonSize);

        for (int32 PolygonVertexIndex = 0; PolygonVertexIndex < PolygonSize; ++PolygonVertexIndex)
        {
            const int32 ControlPointIndex = Mesh.GetPolygonVertex(PolygonIndex, PolygonVertexIndex);
            if (!VertexIds.IsValidIndex(ControlPointIndex))
            {
                OutError = FString::Printf(
                    TEXT("FBX polygon %d references invalid control point %d"),
                    PolygonIndex,
                    ControlPointIndex);
                return false;
            }

            const FVertexInstanceID InstanceId = OutMeshDescription.CreateVertexInstance(VertexIds[ControlPointIndex]);

            FbxVector4 FbxNormal;
            if (Mesh.GetPolygonVertexNormal(PolygonIndex, PolygonVertexIndex, FbxNormal))
            {
                Normals[InstanceId] = ToUnrealDirection(FbxNormal);
            }

            if (UVSetName)
            {
                FbxVector2 FbxUV;
                bool bUnmapped = false;
                if (Mesh.GetPolygonVertexUV(PolygonIndex, PolygonVertexIndex, UVSetName, FbxUV, bUnmapped) && !bUnmapped)
                {
                    UVs.Set(InstanceId, 0, FVector2f(
                        static_cast<float>(FbxUV[0]),
                        static_cast<float>(1.0 - FbxUV[1])));
                }
            }

            Perimeter.Add(InstanceId);
        }

        OutMeshDescription.CreatePolygon(PolygonGroupId, Perimeter);
    }

    FStaticMeshOperations::ComputeTriangleTangentsAndNormals(OutMeshDescription);
    FStaticMeshOperations::ComputeTangentsAndNormals(
        OutMeshDescription,
        EComputeNTBsFlags::Tangents | EComputeNTBsFlags::UseMikkTSpace);
    return true;
}
} // namespace

bool FMHFbxBackend::ImportAxisProbe(
    const FString& Filename,
    FMHGeometryProbeResult& OutResult,
    FString& OutError)
{
    OutResult = FMHGeometryProbeResult();
    OutError.Reset();

    if (!FPaths::FileExists(Filename))
    {
        OutError = FString::Printf(TEXT("FBX file does not exist: %s"), *Filename);
        return false;
    }

    FbxManager* Manager = FbxManager::Create();
    if (!Manager)
    {
        OutError = TEXT("FbxManager::Create failed");
        return false;
    }

    FbxIOSettings* IOSettings = FbxIOSettings::Create(Manager, IOSROOT);
    Manager->SetIOSettings(IOSettings);
    FbxImporter* Importer = FbxImporter::Create(Manager, "MHAxisProbeImporter");
    FbxScene* Scene = FbxScene::Create(Manager, "MHAxisProbeScene");

    const FTCHARToUTF8 FilenameUtf8(*Filename);
    if (!Importer->Initialize(FilenameUtf8.Get(), -1, Manager->GetIOSettings()))
    {
        OutError = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }

    if (!Importer->Import(Scene))
    {
        OutError = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }

    // Deliberately do not call FbxAxisSystem::ConvertScene or FbxSystemUnit::ConvertScene.
    FbxNode* MeshNode = FindFirstMeshNode(Scene->GetRootNode());
    FbxMesh* Mesh = MeshNode ? MeshNode->GetMesh() : nullptr;
    if (!Mesh)
    {
        OutError = TEXT("FBX scene contains no mesh node");
        Manager->Destroy();
        return false;
    }

    OutResult.SourceVertexCount = Mesh->GetControlPointsCount();
    OutResult.SourcePolygonCount = Mesh->GetPolygonCount();

    const FbxAxisSystem AxisSystem = Scene->GetGlobalSettings().GetAxisSystem();
    int AxisSign = 0;
    const FbxAxisSystem::EUpVector UpVector = AxisSystem.GetUpVector(AxisSign);
    int FrontSign = 0;
    const FbxAxisSystem::EFrontVector FrontVector = AxisSystem.GetFrontVector(FrontSign);
    const bool bExpectedAxis =
        UpVector == FbxAxisSystem::eZAxis && AxisSign == 1 &&
        FrontVector == FbxAxisSystem::eParityEven && FrontSign == -1 &&
        AxisSystem.GetCoorSystem() == FbxAxisSystem::eRightHanded;
    // FBX SDK semantic equality accepts the writer's sub-1e-6 scale noise and
    // checks both scale and multiplier, matching UE's FBX import paths.
    const FbxSystemUnit DeclaredUnit = Scene->GetGlobalSettings().GetSystemUnit();
    const bool bExpectedUnits =
        FMath::IsFinite(DeclaredUnit.GetScaleFactor()) &&
        FMath::IsFinite(DeclaredUnit.GetMultiplier()) &&
        DeclaredUnit == FbxSystemUnit::cm;
    if (!bExpectedAxis || !bExpectedUnits)
    {
        OutError = FString::Printf(
            TEXT("MH_E_FBX_TRANSPORT_FAILED: axis or units differ from the canonical FBX export "
                 "(up=%d, up_sign=%d, front=%d, front_sign=%d, handedness=%d, "
                 "unit_scale=%.17g, unit_multiplier=%.17g, expected_unit_scale=%.17g, expected_unit_multiplier=%.17g)"),
            static_cast<int32>(UpVector),
            AxisSign,
            static_cast<int32>(FrontVector),
            FrontSign,
            static_cast<int32>(AxisSystem.GetCoorSystem()),
            DeclaredUnit.GetScaleFactor(),
            DeclaredUnit.GetMultiplier(),
            FbxSystemUnit::cm.GetScaleFactor(),
            FbxSystemUnit::cm.GetMultiplier());
        Manager->Destroy();
        return false;
    }

    FbxGeometryConverter GeometryConverter(Manager);
    FbxNodeAttribute* TriangulatedAttribute = GeometryConverter.Triangulate(Mesh, true);
    Mesh = FbxCast<FbxMesh>(TriangulatedAttribute);
    if (!Mesh)
    {
        OutError = TEXT("FBX SDK triangulation failed");
        Manager->Destroy();
        return false;
    }

    FMeshDescription MeshDescription;
    if (!BuildProbeMeshDescription(*Mesh, MeshDescription, OutError))
    {
        Manager->Destroy();
        return false;
    }

    UStaticMesh* StaticMesh = NewObject<UStaticMesh>(
        GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("MH_AxisProbe_Direct")),
        RF_Transient);
    StaticMesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, TEXT("Default"), TEXT("Default")));

    FStaticMeshSourceModel& SourceModel = StaticMesh->AddSourceModel();
    // R1 mirrors the legacy factory defaults used by tools/ue_axis_check.py.
    // The full C2 mapper will exercise the frozen split-normal contract.
    SourceModel.BuildSettings.bRecomputeNormals = true;
    SourceModel.BuildSettings.bRecomputeTangents = true;
    SourceModel.BuildSettings.bUseMikkTSpace = true;
    SourceModel.BuildSettings.bRemoveDegenerates = false;
    SourceModel.BuildSettings.bGenerateLightmapUVs = false;
    StaticMesh->CreateMeshDescription(0, MoveTemp(MeshDescription));

    UStaticMesh::FCommitMeshDescriptionParams CommitParams;
    CommitParams.bMarkPackageDirty = false;
    CommitParams.bUseHashAsGuid = true;
    StaticMesh->CommitMeshDescription(0, CommitParams);

    TArray<FText> BuildErrors;
    StaticMesh->Build(true, &BuildErrors);
    Manager->Destroy();

    if (!BuildErrors.IsEmpty())
    {
        OutError = FString::Printf(TEXT("UStaticMesh build failed: %s"), *BuildErrors[0].ToString());
        return false;
    }

    CaptureMeshResult(*StaticMesh, OutResult);
    return true;
}

bool FMHLegacyFbxBackend::ImportAxisProbe(
    const FString& Filename,
    FMHGeometryProbeResult& OutResult,
    FString& OutError)
{
    OutResult = FMHGeometryProbeResult();
    OutError.Reset();

    if (!FPaths::FileExists(Filename))
    {
        OutError = FString::Printf(TEXT("FBX file does not exist: %s"), *Filename);
        return false;
    }

    if (!ReadRawFbxCounts(
            Filename,
            OutResult.SourceVertexCount,
            OutResult.SourcePolygonCount,
            OutError))
    {
        return false;
    }

    UFbxFactory* Factory = NewObject<UFbxFactory>();
    UAssetImportTask* Task = NewObject<UAssetImportTask>(Factory);
    UFbxImportUI* ImportOptions = NewObject<UFbxImportUI>(Task);

    Task->Filename = Filename;
    Task->bAutomated = true;
    Task->bReplaceExisting = false;
    Task->bReplaceExistingSettings = false;
    Task->bSave = false;
    Task->bAsync = false;
    Task->Factory = Factory;
    Task->Options = ImportOptions;

    ImportOptions->bAutomatedImportShouldDetectType = false;
    ImportOptions->bImportAsSkeletal = false;
    ImportOptions->bImportMesh = true;
    ImportOptions->bImportAnimations = false;
    ImportOptions->MeshTypeToImport = FBXIT_StaticMesh;
    ImportOptions->OriginalImportType = FBXIT_StaticMesh;
    ImportOptions->bImportMaterials = false;
    ImportOptions->bImportTextures = false;

    UFbxStaticMeshImportData* StaticMeshOptions = ImportOptions->StaticMeshImportData;
    StaticMeshOptions->bConvertScene = true;
    StaticMeshOptions->bForceFrontXAxis = false;
    StaticMeshOptions->bConvertSceneUnit = false;
    StaticMeshOptions->bTransformVertexToAbsolute = true;
    StaticMeshOptions->bBakePivotInVertex = false;
    StaticMeshOptions->bImportMeshLODs = false;
    StaticMeshOptions->bCombineMeshes = true;
    StaticMeshOptions->bRemoveDegenerates = false;
    StaticMeshOptions->bBuildNanite = false;
    StaticMeshOptions->bGenerateLightmapUVs = false;
    StaticMeshOptions->bAutoGenerateCollision = false;
    StaticMeshOptions->NormalImportMethod = FBXNIM_ComputeNormals;
    StaticMeshOptions->NormalGenerationMethod = EFBXNormalGenerationMethod::MikkTSpace;

    Factory->SetDetectImportTypeOnImport(false);
    Factory->SetAssetImportTask(Task);

    bool bCanceled = false;
    UObject* ImportedObject = Factory->ImportObject(
        UStaticMesh::StaticClass(),
        GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("MH_AxisProbe_Legacy")),
        RF_Transient,
        Filename,
        nullptr,
        bCanceled);
    Factory->CleanUp();

    UStaticMesh* StaticMesh = Cast<UStaticMesh>(ImportedObject);
    if (!StaticMesh || bCanceled)
    {
        OutError = bCanceled ? TEXT("Legacy FBX import was canceled") : TEXT("Legacy FBX factory returned no UStaticMesh");
        return false;
    }

    TArray<UStaticMesh*> MeshesToFinish{StaticMesh};
    FStaticMeshCompilingManager::Get().FinishCompilation(MeshesToFinish);
    CaptureMeshResult(*StaticMesh, OutResult);
    return true;
}
