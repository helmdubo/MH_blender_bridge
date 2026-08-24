#pragma once

#include "CoreMinimal.h"
#include "Material/MHMaterialImporter.h"

class UMaterialInstanceConstant;
class UMHCompositeSettings;

namespace UE::MimirComposite
{

/** Narrow S2 editor modal for the folder + logical-name Adopt contract. */
MIMIRCOMPOSITEEDITOR_API FMHMaterialOperationResult MHShowMaterialAdoptDialog(
    UMaterialInstanceConstant& Material,
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings);

} // namespace UE::MimirComposite
