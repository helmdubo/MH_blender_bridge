#pragma once

#include "CoreMinimal.h"
#include "MHCompositeTypes.generated.h"

/** Source Protocol v4 resource kinds. Identity is kind plus logical name. */
UENUM()
enum class EMHResourceKind : uint8
{
	StaticMesh,
	Material,
	Composite,
	Texture
};
