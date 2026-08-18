#pragma once

#include "Composite/MHCompositeTypes.h"
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MHCompositeAsset.generated.h"

class UAssetImportData;

/**
 * Read-only editor image of one mh.composite v2 payload. ResourceUID stays the
 * node-graph truth; ResolvedAsset paths are a convenience filled by importers.
 */
UCLASS(BlueprintType)
class MIMIRCOMPOSITERUNTIME_API UMHCompositeAsset final : public UObject
{
	GENERATED_BODY()

public:
	/** Full lowercase UUID of this composite resource. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FString CompositeUid;

	/** Current resource display name from the payload. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FString CompositeName;

	/** Asset-level top-level properties bag, compact JSON. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	FString ResourcePropertiesJson;

	/** Flat node table; hierarchy is expressed through ParentUid. */
	UPROPERTY(VisibleAnywhere, Category = "Mimir")
	TArray<FMHCompositeNode> Nodes;

	/** Exact payload text this asset was imported from. */
	UPROPERTY()
	FString SourceJsonSnapshot;

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Instanced, Category = "Mimir")
	TObjectPtr<UAssetImportData> AssetImportData;

	virtual void PostInitProperties() override;
#endif
};
