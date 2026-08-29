#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"

namespace UE::MimirComposite
{

/** FBX node attribute kinds admitted by Source Protocol v4. */
enum class EMHSceneNodeAttribute : uint8
{
    Unsupported,
    Mesh,
    Null
};

/** Semantic node kinds produced by the common v4 classifier. */
enum class EMHSceneNodeKind : uint8
{
    Unclassified,
    Render,
    Collision,
    Socket,
    Group
};

/** Per-shape collision behavior from the v4 FBX name marker. */
enum class EMHSceneCollisionMode : uint8
{
    None,
    PhysicsOnly,
    QueryOnly,
    QueryAndPhysics
};

/**
 * Collision role declared by the V5-S6.1.2 FBX Model user property
 * `mh_collision`. None means the node carries no property and is classified by
 * the pre-existing v4 name markers alone.
 */
enum class EMHSceneCollisionCarrier : uint8
{
    None,
    Phys,
    Trace
};

/** Collision primitive requested by the `mh_collision_shape` user property. */
enum class EMHSceneCollisionShape : uint8
{
    Mesh,
    Box,
    Convex,
    Capsule
};

/** One triangulated polygon with corner data and its node-local slot index. */
struct FMHSceneTriangle
{
    TStaticArray<int32, 3> PositionIndices{INDEX_NONE, INDEX_NONE, INDEX_NONE};
    TStaticArray<FVector3f, 3> CornerNormals{FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector};
    TStaticArray<FVector2f, 3> CornerUV0{FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector};
    int32 MaterialSlotIndex = INDEX_NONE;
};

/** Geometry owned by one mesh node after FBX SDK triangulation. */
struct FMHSceneGeometry
{
    TArray<FVector3f> Positions;
    TArray<FMHSceneTriangle> Triangles;
};

/**
 * Blender-agnostic node data shared by the FBX translator, classifier and
 * static-mesh builder. ParentIndex and GlobalTransform are evaluated by the
 * translator; classification fields are overwritten only after a successful
 * MHClassifySceneIR call.
 */
struct FMHSceneIRNode
{
    FString Name;
    EMHSceneNodeAttribute Attribute = EMHSceneNodeAttribute::Unsupported;
    int32 ParentIndex = INDEX_NONE;
    FTransform GlobalTransform = FTransform::Identity;
    TArray<FString> MaterialSlots;
    TOptional<FMHSceneGeometry> Geometry;

    /** Reverse source triangle corners after the complete FBX->UE matrix. */
    bool bReverseWinding = false;

    /**
     * Collision carrier read verbatim from the FBX Model user properties. These
     * are transport facts, not classifier output: MHClassifySceneIR reads them
     * and never rewrites them.
     */
    EMHSceneCollisionCarrier CollisionCarrier = EMHSceneCollisionCarrier::None;
    EMHSceneCollisionShape CollisionShape = EMHSceneCollisionShape::Mesh;
    /** Dagor phmat registry token; empty when the node declares none. */
    FString PhysicalMaterialToken;

    EMHSceneNodeKind Kind = EMHSceneNodeKind::Unclassified;
    int32 LODLevel = INDEX_NONE;
    EMHSceneCollisionMode CollisionMode = EMHSceneCollisionMode::None;
    FString SocketName;
};

/** Complete neutral scene plus classifier-derived build inventory. */
struct FMHSceneIR
{
    FString ResourceName;
    TArray<FMHSceneIRNode> Nodes;
    TArray<FString> MaterialNames;
    TArray<int32> LODLevels;
    bool bUsesExplicitLODs = false;
};

/**
 * Validate and classify a translated FBX scene according to 08 section 4.
 * The operation is atomic: InOutScene is unchanged when false is returned.
 */
MIMIRCOMPOSITEEDITOR_API bool MHClassifySceneIR(
    FMHSceneIR& InOutScene,
    FString& OutError);

} // namespace UE::MimirComposite
