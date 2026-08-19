#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "MHCompositeTypes.generated.h"

/** Source Schema v1 resource kinds accepted by the import slice. */
UENUM()
enum class EMHResourceKind : uint8
{
	StaticMesh,
	Composite,
	Material
};

/** mh.composite v1 node kinds accepted by the import slice. */
UENUM()
enum class EMHCompositeNodeKind : uint8
{
	Group,
	Mesh,
	CompositeRef
};

/** One flat mh.composite v1 node; hierarchy is expressed through ParentUid. */
USTRUCT()
struct MIMIRCOMPOSITERUNTIME_API FMHCompositeNode
{
	GENERATED_BODY()

	/** Full lowercase UUID of the node, unique inside the composite. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FString NodeUid;

	/** Empty for a root node, otherwise the NodeUid of the parent in this file. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FString ParentUid;

	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	EMHCompositeNodeKind Kind = EMHCompositeNodeKind::Group;

	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FString DisplayName;

	/** Referenced ResourceUID; empty for group nodes. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FString ResourceUid;

	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FVector TranslationCm = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FQuat RotationQuat = FQuat::Identity;

	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FVector Scale = FVector::OneVector;

	/** Placement-level JSON bag exactly as authored, compact form. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FString PropertiesJson;

	/** Filled by the importer once the referenced asset exists; UID stays the truth. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FSoftObjectPath ResolvedAsset;
};
