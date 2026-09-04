#pragma once

#include "CoreMinimal.h"
#include "Material/MHMaterialDocumentExport.h"
#include "Source/MHSourceImporter.h"

namespace UE::MimirComposite
{

/**
 * Read-only full-state donor preflight. Exactly one leading m_ is removed from
 * each asset name. Existing unique sources anywhere in SourceRoot retain their
 * paths; new sources use NewSourceFolder. Any rejection clears the ready batch.
 */
MIMIRCOMPOSITEEDITOR_API bool MHPrepareMaterialDonorTransfer(
    TConstArrayView<UMaterialInstanceConstant*> Materials,
    const FString& SourceRoot,
    const FString& NewSourceFolder,
    FMHMaterialDocumentExportPlan& OutPlan,
    FString& OutError);

/** Successful writes only. Do not call ImportSources when ResourceKeys is empty. */
MIMIRCOMPOSITEEDITOR_API FMHImportSourcesScope MHMaterialDonorImportScope(
    const FMHMaterialDocumentExportPlan& Plan,
    const FMHMaterialDocumentExportResult& Result);

/** Internal commit guard; no writes if logical source destinations changed. */
MIMIRCOMPOSITEEDITOR_API bool MHValidateMaterialDonorDestinations(
    const FMHMaterialDocumentExportPlan& Plan, FString& OutError);

} // namespace UE::MimirComposite
