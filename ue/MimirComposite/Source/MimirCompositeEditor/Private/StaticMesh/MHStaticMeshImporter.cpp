#include "StaticMesh/MHStaticMeshImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Geometry/MHFbxSceneTranslator.h"
#include "Geometry/MHSceneIR.h"
#include "HAL/FileManager.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHStaticMeshImport, Log, All);

namespace UE::MimirComposite
{
namespace
{

constexpr TCHAR StaticMeshGeneratedRoot[] = TEXT("/Game/MH/Generated/Meshes");

bool FailStaticMeshImport(FString& OutError, const TCHAR* Code, const FString& Message)
{
    OutError = FString::Printf(TEXT("%s: %s"), Code, *Message);
    return false;
}

bool LoadSourceBytes(const FString& Filename, TArray<uint8>& OutBytes, FString& OutError)
{
    OutBytes.Reset();
    if (!FFileHelper::LoadFileToArray(OutBytes, *Filename))
    {
        return FailStaticMeshImport(
            OutError,
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED"),
            FString::Printf(TEXT("cannot read indexed FBX payload '%s'"), *Filename));
    }
    return true;
}

bool SaveStaticMeshPackage(UStaticMesh& StaticMesh, FString& OutError)
{
    UPackage* Package = StaticMesh.GetOutermost();
    if (Package == nullptr)
    {
        return FailStaticMeshImport(OutError, TEXT("MH_E_IMPORT_FAILED"), TEXT("static mesh has no package"));
    }
    const FString Filename = FPackageName::LongPackageNameToFilename(
        Package->GetName(),
        FPackageName::GetAssetPackageExtension());
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    Args.Error = GError;
    if (!UPackage::SavePackage(Package, &StaticMesh, *Filename, Args))
    {
        return FailStaticMeshImport(
            OutError,
            TEXT("MH_E_IMPORT_FAILED"),
            FString::Printf(TEXT("failed to save package '%s'"), *Package->GetName()));
    }
    return true;
}

void DiscardCreatedStaticMesh(UStaticMesh& StaticMesh)
{
    UPackage* Package = StaticMesh.GetOutermost();
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
        .Get()
        .AssetDeleted(&StaticMesh);
    StaticMesh.ClearFlags(RF_Public | RF_Standalone);
    StaticMesh.MarkAsGarbage();
    if (Package != nullptr)
    {
        const FString Filename = FPackageName::LongPackageNameToFilename(
            Package->GetName(),
            FPackageName::GetAssetPackageExtension());
        IFileManager::Get().Delete(*Filename, false, true, true);
        Package->SetDirtyFlag(false);
    }
}

bool HasNonCoplanarHull(const TArray<FVector3f>& Positions)
{
    TArray<FVector3f> Unique;
    Unique.Reserve(Positions.Num());
    for (const FVector3f& Position : Positions)
    {
        if (Position.ContainsNaN())
        {
            return false;
        }
        if (!Unique.Contains(Position))
        {
            Unique.Add(Position);
        }
    }
    if (Unique.Num() < 4)
    {
        return false;
    }

    const FVector3f A = Unique[0];
    int32 BIndex = INDEX_NONE;
    for (int32 Index = 1; Index < Unique.Num(); ++Index)
    {
        if (!Unique[Index].Equals(A, UE_SMALL_NUMBER))
        {
            BIndex = Index;
            break;
        }
    }
    if (BIndex == INDEX_NONE)
    {
        return false;
    }

    const FVector3f AB = Unique[BIndex] - A;
    FVector3f PlaneNormal = FVector3f::ZeroVector;
    for (int32 Index = 1; Index < Unique.Num(); ++Index)
    {
        if (Index == BIndex)
        {
            continue;
        }
        PlaneNormal = FVector3f::CrossProduct(AB, Unique[Index] - A);
        if (PlaneNormal.SquaredLength() > FMath::Square(UE_SMALL_NUMBER))
        {
            break;
        }
        PlaneNormal = FVector3f::ZeroVector;
    }
    if (PlaneNormal.IsNearlyZero())
    {
        return false;
    }
    const float PlaneScale = PlaneNormal.Length();
    for (const FVector3f& Position : Unique)
    {
        if (FMath::Abs(FVector3f::DotProduct(PlaneNormal, Position - A)) > UE_SMALL_NUMBER * PlaneScale)
        {
            return true;
        }
    }
    return false;
}

ECollisionEnabled::Type CollisionEnabledFor(const EMHSceneCollisionMode Mode)
{
    switch (Mode)
    {
    case EMHSceneCollisionMode::PhysicsOnly: return ECollisionEnabled::PhysicsOnly;
    case EMHSceneCollisionMode::QueryOnly: return ECollisionEnabled::QueryOnly;
    case EMHSceneCollisionMode::QueryAndPhysics: return ECollisionEnabled::QueryAndPhysics;
    case EMHSceneCollisionMode::None:
    default: return ECollisionEnabled::NoCollision;
    }
}

bool ValidateBuildPlan(const FMHStaticMeshBuildPlan& Plan, FString& OutError)
{
    if (!Plan.IsValid() || Plan.Scene->LODLevels.IsEmpty())
    {
        return FailStaticMeshImport(OutError, TEXT("MH_E_INVALID_RESOURCE_SOURCE"), TEXT("static-mesh build plan has no dense LOD inventory"));
    }
    for (int32 Expected = 0; Expected < Plan.Scene->LODLevels.Num(); ++Expected)
    {
        if (Plan.Scene->LODLevels[Expected] != Expected)
        {
            return FailStaticMeshImport(OutError, TEXT("MH_E_LOD_LEVELS_SPARSE"), TEXT("static-mesh build plan has sparse LOD levels"));
        }
    }
    for (const FString& MaterialName : Plan.Scene->MaterialNames)
    {
        if (!Plan.Materials.Contains(MaterialName) || Plan.Materials.FindRef(MaterialName) == nullptr)
        {
            return FailStaticMeshImport(
                OutError,
                TEXT("MH_E_UNRESOLVED_MATERIAL_REFERENCE"),
                FString::Printf(TEXT("material slot '%s' is not resolved to a managed MI"), *MaterialName));
        }
    }
    for (const FMHSceneIRNode& Node : Plan.Scene->Nodes)
    {
        if ((Node.Kind == EMHSceneNodeKind::Render || Node.Kind == EMHSceneNodeKind::Collision) && !Node.Geometry.IsSet())
        {
            return FailStaticMeshImport(
                OutError,
                TEXT("MH_E_INVALID_RESOURCE_SOURCE"),
                FString::Printf(TEXT("classified mesh node '%s' has no geometry"), *Node.Name));
        }
        if (Node.Kind == EMHSceneNodeKind::Collision && !HasNonCoplanarHull(Node.Geometry->Positions))
        {
            return FailStaticMeshImport(
                OutError,
                TEXT("MH_E_INVALID_RESOURCE_SOURCE"),
                FString::Printf(TEXT("collision node '%s' has fewer than four non-coplanar points"), *Node.Name));
        }
    }
    return true;
}

/**
 * Build one LOD MeshDescription and report the slot name of every polygon group
 * in creation order. UE turns each polygon group into one render section in that
 * same order, so the reported list is the LOD's section -> slot table.
 */
bool BuildMeshDescriptionForLOD(
    const FMHSceneIR& Scene,
    const int32 LODLevel,
    FMeshDescription& OutDescription,
    TArray<FString>& OutSectionSlotNames,
    FString& OutError)
{
    OutSectionSlotNames.Reset();
    FStaticMeshAttributes Attributes(OutDescription);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
    TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
    UVs.SetNumChannels(1);
    TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

    TMap<FString, FPolygonGroupID> PolygonGroups;
    auto FindOrCreatePolygonGroup =
        [&OutDescription, &SlotNames, &PolygonGroups, &OutSectionSlotNames](const FString& SlotName)
    {
        if (const FPolygonGroupID* Existing = PolygonGroups.Find(SlotName))
        {
            return *Existing;
        }
        const FPolygonGroupID Created = OutDescription.CreatePolygonGroup();
        SlotNames[Created] = SlotName.IsEmpty() ? NAME_None : FName(*SlotName);
        PolygonGroups.Add(SlotName, Created);
        OutSectionSlotNames.Add(SlotName);
        return Created;
    };

    bool bAddedRenderGeometry = false;
    for (const FMHSceneIRNode& Node : Scene.Nodes)
    {
        if (Node.Kind != EMHSceneNodeKind::Render || Node.LODLevel != LODLevel)
        {
            continue;
        }
        bAddedRenderGeometry = true;
        const FMHSceneGeometry& Geometry = Node.Geometry.GetValue();
        TArray<FVertexID> VertexIds;
        VertexIds.Reserve(Geometry.Positions.Num());
        for (const FVector3f& Position : Geometry.Positions)
        {
            if (Position.ContainsNaN())
            {
                return FailStaticMeshImport(OutError, TEXT("MH_E_INVALID_RESOURCE_SOURCE"), FString::Printf(TEXT("node '%s' contains a non-finite vertex"), *Node.Name));
            }
            const FVertexID VertexId = OutDescription.CreateVertex();
            Positions[VertexId] = Position;
            VertexIds.Add(VertexId);
        }

        for (const FMHSceneTriangle& Triangle : Geometry.Triangles)
        {
            FString SlotName;
            if (Triangle.MaterialSlotIndex != INDEX_NONE)
            {
                if (!Node.MaterialSlots.IsValidIndex(Triangle.MaterialSlotIndex))
                {
                    return FailStaticMeshImport(
                        OutError,
                        TEXT("MH_E_FBX_TRANSPORT_FAILED"),
                        FString::Printf(TEXT("node '%s' triangle references slot %d outside its material table"), *Node.Name, Triangle.MaterialSlotIndex));
                }
                SlotName = Node.MaterialSlots[Triangle.MaterialSlotIndex];
            }
            const FPolygonGroupID PolygonGroup = FindOrCreatePolygonGroup(SlotName);
            TArray<FVertexInstanceID> Perimeter;
            Perimeter.Reserve(3);
            for (int32 OutputCorner = 0; OutputCorner < 3; ++OutputCorner)
            {
                // Match UFbxFactory: FBX->UE position conversion already
                // changes handedness, while polygon corner order is retained.
                const int32 SourceCorner = OutputCorner;
                const int32 PositionIndex = Triangle.PositionIndices[SourceCorner];
                if (!VertexIds.IsValidIndex(PositionIndex))
                {
                    return FailStaticMeshImport(
                        OutError,
                        TEXT("MH_E_FBX_TRANSPORT_FAILED"),
                        FString::Printf(TEXT("node '%s' triangle references vertex %d outside its geometry"), *Node.Name, PositionIndex));
                }
                const FVertexInstanceID Instance = OutDescription.CreateVertexInstance(VertexIds[PositionIndex]);
                Normals[Instance] = Triangle.CornerNormals[SourceCorner];
                UVs.Set(Instance, 0, Triangle.CornerUV0[SourceCorner]);
                Perimeter.Add(Instance);
            }
            OutDescription.CreatePolygon(PolygonGroup, Perimeter);
        }
    }
    if (!bAddedRenderGeometry || OutDescription.Polygons().Num() == 0)
    {
        return FailStaticMeshImport(
            OutError,
            TEXT("MH_E_EMPTY_RESOURCE_COLLECTION"),
            FString::Printf(TEXT("LOD%d has no render triangles"), LODLevel));
    }

    FStaticMeshOperations::ComputeTriangleTangentsAndNormals(OutDescription);
    FStaticMeshOperations::ComputeTangentsAndNormals(
        OutDescription,
        EComputeNTBsFlags::Tangents | EComputeNTBsFlags::UseMikkTSpace);
    return true;
}

bool ResolveMaterials(
    const FMHSceneIR& Scene,
    IMHSourceResolver& Resolver,
    TMap<FString, UMaterialInstanceConstant*>& OutMaterials,
    FString& OutError)
{
    OutMaterials.Reset();
    for (const FString& MaterialName : Scene.MaterialNames)
    {
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::Material;
        Key.LogicalName = MaterialName;
        const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            return FailStaticMeshImport(
                OutError,
                TEXT("MH_E_UNRESOLVED_MATERIAL_REFERENCE"),
                FString::Printf(TEXT("slot '%s' does not resolve to exactly one material source"), *MaterialName));
        }

        const FString PackageName = FString(TEXT("/Game/MH/Generated/Materials/")) + MaterialName;
        const FString ObjectPath = PackageName + TEXT(".") + MaterialName;
        UMaterialInstanceConstant* Material = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
        if (Material == nullptr)
        {
            return FailStaticMeshImport(
                OutError,
                TEXT("MH_E_UNRESOLVED_MATERIAL_REFERENCE"),
                FString::Printf(TEXT("slot '%s' has source but no applied managed MI at '%s'"), *MaterialName, *ObjectPath));
        }
        const UMHMaterialSourceData* Receipt = Cast<UMHMaterialSourceData>(
            Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
        if (Receipt == nullptr || Receipt->LogicalName != MaterialName ||
            Receipt->SourceHash.IsEmpty() || Receipt->AppliedHash.IsEmpty() ||
            Receipt->SourceHash != Outcome.RawHash)
        {
            return FailStaticMeshImport(
                OutError,
                TEXT("MH_E_UNRESOLVED_MATERIAL_REFERENCE"),
                FString::Printf(TEXT("slot '%s' does not have an applied managed material receipt"), *MaterialName));
        }
        OutMaterials.Add(MaterialName, Material);
    }
    return true;
}

} // namespace

bool FMHStaticMeshBuilder::Rebuild(
    UStaticMesh& StaticMesh,
    const FMHStaticMeshBuildPlan& Plan,
    FString& OutError)
{
    OutError.Reset();
    if (!ValidateBuildPlan(Plan, OutError))
    {
        return false;
    }
    const FMHSceneIR& Scene = *Plan.Scene;

    // Build every MeshDescription before touching the target UObject.
    TArray<FMeshDescription> Descriptions;
    TArray<TArray<FString>> SectionSlotNames;
    Descriptions.SetNum(Scene.LODLevels.Num());
    SectionSlotNames.SetNum(Scene.LODLevels.Num());
    for (const int32 LODLevel : Scene.LODLevels)
    {
        if (!BuildMeshDescriptionForLOD(
                Scene,
                LODLevel,
                Descriptions[LODLevel],
                SectionSlotNames[LODLevel],
                OutError))
        {
            return false;
        }
    }

    TArray<FStaticMaterial> StaticMaterials;
    // Slot index of every union material name; the mesh material list is exactly
    // Scene.MaterialNames, so this is the authoritative section -> slot table.
    TMap<FString, int32> MaterialSlotIndices;
    if (Scene.MaterialNames.IsEmpty())
    {
        StaticMaterials.Emplace(nullptr, NAME_None, NAME_None);
        // A mesh with no slot at all keeps its single unnamed placeholder.
        MaterialSlotIndices.Add(FString(), 0);
    }
    else
    {
        StaticMaterials.Reserve(Scene.MaterialNames.Num());
        for (const FString& MaterialName : Scene.MaterialNames)
        {
            const FName SlotName(*MaterialName);
            MaterialSlotIndices.Add(MaterialName, StaticMaterials.Num());
            StaticMaterials.Emplace(Plan.Materials.FindRef(MaterialName), SlotName, SlotName);
        }
    }

    // Resolve every LOD section to a union slot before mutating the asset, so a
    // scene whose nodes reference a slot outside the union fails closed instead
    // of silently rendering with the positionally matching material.
    TArray<TArray<int32>> SectionMaterialIndices;
    SectionMaterialIndices.SetNum(SectionSlotNames.Num());
    for (int32 LODIndex = 0; LODIndex < SectionSlotNames.Num(); ++LODIndex)
    {
        SectionMaterialIndices[LODIndex].Reserve(SectionSlotNames[LODIndex].Num());
        for (const FString& SlotName : SectionSlotNames[LODIndex])
        {
            const int32* SlotIndex = MaterialSlotIndices.Find(SlotName);
            if (SlotIndex == nullptr)
            {
                return FailStaticMeshImport(
                    OutError,
                    TEXT("MH_E_UNRESOLVED_MATERIAL_REFERENCE"),
                    FString::Printf(
                        TEXT("LOD%d section slot '%s' is absent from the mesh material union"),
                        LODIndex,
                        SlotName.IsEmpty() ? TEXT("<unassigned>") : *SlotName));
            }
            SectionMaterialIndices[LODIndex].Add(*SlotIndex);
        }
    }

    StaticMesh.Modify();
    StaticMesh.SetStaticMaterials(StaticMaterials);
    StaticMesh.SetNumSourceModels(Descriptions.Num());
    StaticMesh.GetSectionInfoMap().Clear();
    StaticMesh.GetOriginalSectionInfoMap().Clear();
    StaticMesh.GetNaniteSettings().bEnabled = false;
    for (int32 LODIndex = 0; LODIndex < Descriptions.Num(); ++LODIndex)
    {
        FStaticMeshSourceModel& SourceModel = StaticMesh.GetSourceModel(LODIndex);
        // Every LOD carries authored geometry. UStaticMesh::SetNumSourceModels
        // seeds LOD1+ with the LOD group's auto-reduction defaults
        // (PercentTriangles 0.5), which makes the build discard the authored
        // mesh description and replace it with a reduction of LOD0 - taking the
        // LOD's own material sections with it. Reset it on every LOD.
        SourceModel.ResetReductionSetting();
        SourceModel.BuildSettings.bRecomputeNormals = false;
        SourceModel.BuildSettings.bRecomputeTangents = true;
        SourceModel.BuildSettings.bUseMikkTSpace = true;
        SourceModel.BuildSettings.bRemoveDegenerates = false;
        SourceModel.BuildSettings.bGenerateLightmapUVs = false;
        StaticMesh.CreateMeshDescription(LODIndex, MoveTemp(Descriptions[LODIndex]));
        UStaticMesh::FCommitMeshDescriptionParams CommitParams;
        CommitParams.bMarkPackageDirty = false;
        CommitParams.bUseHashAsGuid = true;
        StaticMesh.CommitMeshDescription(LODIndex, CommitParams);
    }

    // Bind each LOD's sections to their own union slots. Without this the
    // engine's FStaticMeshRenderData::ResolveSectionInfo falls back to the
    // identity map (section N -> material N), which silently binds a LOD to the
    // wrong material whenever its slot inventory or order differs from LOD0.
    for (int32 LODIndex = 0; LODIndex < SectionMaterialIndices.Num(); ++LODIndex)
    {
        for (int32 SectionIndex = 0; SectionIndex < SectionMaterialIndices[LODIndex].Num(); ++SectionIndex)
        {
            FMeshSectionInfo SectionInfo;
            SectionInfo.MaterialIndex = SectionMaterialIndices[LODIndex][SectionIndex];
            StaticMesh.GetSectionInfoMap().Set(LODIndex, SectionIndex, SectionInfo);
            StaticMesh.GetOriginalSectionInfoMap().Set(LODIndex, SectionIndex, SectionInfo);
        }
    }

    StaticMesh.Sockets.Reset();
    for (const FMHSceneIRNode& Node : Scene.Nodes)
    {
        if (Node.Kind != EMHSceneNodeKind::Socket)
        {
            continue;
        }
        UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(&StaticMesh, NAME_None, RF_Transactional);
        Socket->SocketName = FName(*Node.SocketName);
        Socket->RelativeLocation = Node.GlobalTransform.GetTranslation();
        Socket->RelativeRotation = Node.GlobalTransform.Rotator();
        Socket->RelativeScale = Node.GlobalTransform.GetScale3D();
        StaticMesh.Sockets.Add(Socket);
    }

    StaticMesh.CreateBodySetup();
    UBodySetup* BodySetup = StaticMesh.GetBodySetup();
    check(BodySetup != nullptr);
    BodySetup->Modify();
    BodySetup->AggGeom.EmptyElements();
    BodySetup->CollisionTraceFlag = CTF_UseDefault;
    for (const FMHSceneIRNode& Node : Scene.Nodes)
    {
        if (Node.Kind != EMHSceneNodeKind::Collision)
        {
            continue;
        }
        FKConvexElem& Convex = BodySetup->AggGeom.ConvexElems.AddDefaulted_GetRef();
        Convex.VertexData.Reserve(Node.Geometry->Positions.Num());
        for (const FVector3f& Position : Node.Geometry->Positions)
        {
            Convex.VertexData.Add(FVector(Position));
        }
        Convex.SetCollisionEnabled(CollisionEnabledFor(Node.CollisionMode));
        Convex.UpdateElemBox();
    }
    BodySetup->InvalidatePhysicsData();

    TArray<FText> BuildErrors;
    StaticMesh.Build(true, &BuildErrors);
    TArray<UStaticMesh*> MeshesToFinish{&StaticMesh};
    FStaticMeshCompilingManager::Get().FinishCompilation(MeshesToFinish);
    if (!BuildErrors.IsEmpty())
    {
        return FailStaticMeshImport(
            OutError,
            TEXT("MH_E_IMPORT_FAILED"),
            FString::Printf(TEXT("UStaticMesh build failed: %s"), *BuildErrors[0].ToString()));
    }
    BodySetup->CreatePhysicsMeshes();
    StaticMesh.PostEditChange();
    return true;
}

FMHStaticMeshOperationResult MHImportStaticMeshV4(
    const FMHSourceAnalysisEntry& Entry,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const bool bForceReimport)
{
    FMHStaticMeshOperationResult Result;
    if (Entry.Key.Kind != EMHResourceKind::StaticMesh || !Entry.Key.IsCanonical() ||
        Entry.PayloadPath.IsEmpty() || Entry.SourcePath.IsEmpty())
    {
        Result.Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: import entry is not a resolved static mesh");
        return Result;
    }

    TArray<uint8> SourceBytes;
    if (!LoadSourceBytes(Entry.PayloadPath, SourceBytes, Result.Error))
    {
        return Result;
    }
    const FString SourceHash = MHRawPayloadHash(SourceBytes);
    if (!Entry.RawHash.IsEmpty() && SourceHash != Entry.RawHash)
    {
        Result.Error = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: FBX bytes changed after source scan");
        return Result;
    }

    FMHSceneIR Scene;
    FMHFbxSceneTranslator Translator;
    if (!Translator.Translate(Entry.Key.LogicalName, SourceBytes, Scene, Result.Error))
    {
        return Result;
    }

    FMHStaticMeshBuildPlan Plan;
    Plan.Scene = &Scene;
    if (!ResolveMaterials(Scene, Resolver, Plan.Materials, Result.Error))
    {
        return Result;
    }
    if (!ValidateBuildPlan(Plan, Result.Error))
    {
        return Result;
    }

    TArray<uint8> FinalBytes;
    if (!LoadSourceBytes(Entry.PayloadPath, FinalBytes, Result.Error) || FinalBytes != SourceBytes)
    {
        Result.Error = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: FBX bytes changed before generated-asset mutation");
        return Result;
    }

    const FString PackageName = FString(StaticMeshGeneratedRoot) + TEXT("/") + Entry.Key.LogicalName;
    const FString ObjectPath = PackageName + TEXT(".") + Entry.Key.LogicalName;
    UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
    UStaticMesh* StaticMesh = Cast<UStaticMesh>(ExistingObject);
    if (ExistingObject != nullptr && StaticMesh == nullptr)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_IMPORT_TARGET_OCCUPIED: generated mesh path is occupied by %s"),
            *ExistingObject->GetClass()->GetName());
        return Result;
    }
    UMHStaticMeshImportData* ExistingData = StaticMesh != nullptr
        ? Cast<UMHStaticMeshImportData>(StaticMesh->GetAssetImportData())
        : nullptr;
    const bool bSameAppliedPayload = ExistingData != nullptr &&
        ExistingData->SourceHash == SourceHash &&
        ExistingData->ImporterVersion == MHStaticMeshImporterVersion;
    if (ExistingData != nullptr && ExistingData->bLocallyModified)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("MH_W_MANAGED_STATIC_MESH_LOCALLY_MODIFIED: %s was modified after its last applied source"),
            *StaticMesh->GetPathName()));
    }
    if (!bForceReimport && bSameAppliedPayload && ExistingData->SourceRelativePath == Entry.SourcePath)
    {
        Result.StaticMesh = StaticMesh;
        return Result;
    }
    if (!bForceReimport && bSameAppliedPayload)
    {
        // A same-byte source move is NO_CHANGE for every mesh domain, but its
        // provenance receipt must follow the current candidate path.
        FMHScopedStaticMeshImportMutation MutationGuard;
        const FString OldRelativePath = ExistingData->SourceRelativePath;
        const FAssetImportInfo OldSourceData = ExistingData->GetSourceData();
        ExistingData->Modify();
        ExistingData->SourceRelativePath = Entry.SourcePath;
        ExistingData->Update(Entry.PayloadPath);
        StaticMesh->PostEditChange();
        if (!SaveStaticMeshPackage(*StaticMesh, Result.Error))
        {
            ExistingData->SourceRelativePath = OldRelativePath;
            ExistingData->SourceData = OldSourceData;
            StaticMesh->PostEditChange();
            return Result;
        }
        FString RebindEvent;
        if (MHConsumeOrphanRebindEvent(SourceRoot, Entry.Key, RebindEvent))
        {
            Result.Warnings.Add(RebindEvent);
            UE_LOG(LogMHStaticMeshImport, Warning, TEXT("%s"), *RebindEvent);
        }
        if (!MHRefreshGeneratedAssetProjection(SourceRoot, Result.Error))
        {
            return Result;
        }
        Result.StaticMesh = StaticMesh;
        Result.bReceiptUpdated = true;
        return Result;
    }

    if (StaticMesh == nullptr)
    {
        UPackage* Package = CreatePackage(*PackageName);
        StaticMesh = NewObject<UStaticMesh>(
            Package,
            FName(*Entry.Key.LogicalName),
            RF_Public | RF_Standalone | RF_Transactional);
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get()
            .AssetCreated(StaticMesh);
        Result.bCreated = true;
    }

    FMHScopedStaticMeshImportMutation MutationGuard;
    if (!FMHStaticMeshBuilder::Rebuild(*StaticMesh, Plan, Result.Error))
    {
        if (Result.bCreated)
        {
            DiscardCreatedStaticMesh(*StaticMesh);
        }
        return Result;
    }

    UAssetImportData* PreviousImportData = StaticMesh->GetAssetImportData();
    UMHStaticMeshImportData* Data = Cast<UMHStaticMeshImportData>(StaticMesh->GetAssetImportData());
    const bool bCreatedReceipt = Data == nullptr;
    FString OldLogicalName;
    FString OldSourceRelativePath;
    FString OldSourceHash;
    int32 OldImporterVersion = 0;
    bool bOldLocallyModified = false;
    FAssetImportInfo OldSourceData;
    if (Data != nullptr)
    {
        OldLogicalName = Data->LogicalName;
        OldSourceRelativePath = Data->SourceRelativePath;
        OldSourceHash = Data->SourceHash;
        OldImporterVersion = Data->ImporterVersion;
        bOldLocallyModified = Data->bLocallyModified;
        OldSourceData = Data->GetSourceData();
    }
    if (Data == nullptr)
    {
        Data = NewObject<UMHStaticMeshImportData>(StaticMesh, NAME_None, RF_Transactional);
        StaticMesh->SetAssetImportData(Data);
    }
    Data->Modify();
    Data->LogicalName = Entry.Key.LogicalName;
    Data->SourceRelativePath = Entry.SourcePath;
    Data->SourceHash = SourceHash;
    Data->ImporterVersion = MHStaticMeshImporterVersion;
    Data->bLocallyModified = false;
    Data->Update(Entry.PayloadPath);
    StaticMesh->PostEditChange();
    if (!SaveStaticMeshPackage(*StaticMesh, Result.Error))
    {
        if (bCreatedReceipt)
        {
            StaticMesh->SetAssetImportData(PreviousImportData);
        }
        else
        {
            Data->LogicalName = OldLogicalName;
            Data->SourceRelativePath = OldSourceRelativePath;
            Data->SourceHash = OldSourceHash;
            Data->ImporterVersion = OldImporterVersion;
            Data->bLocallyModified = bOldLocallyModified;
            Data->SourceData = OldSourceData;
        }
        StaticMesh->PostEditChange();
        if (Result.bCreated)
        {
            DiscardCreatedStaticMesh(*StaticMesh);
        }
        return Result;
    }

    FString RebindEvent;
    if (MHConsumeOrphanRebindEvent(SourceRoot, Entry.Key, RebindEvent))
    {
        Result.Warnings.Add(RebindEvent);
        UE_LOG(LogMHStaticMeshImport, Warning, TEXT("%s"), *RebindEvent);
    }
    if (!MHRefreshGeneratedAssetProjection(SourceRoot, Result.Error))
    {
        return Result;
    }
    Result.StaticMesh = StaticMesh;
    Result.bRebuilt = true;
    MHNotifyGeneratedResourceChanged(Entry.Key);
    return Result;
}

} // namespace UE::MimirComposite
