#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceAnalyzer.h"

class UMHCompositeSettings;
class UStaticMesh;

namespace UE::MimirComposite
{

/** Full project-index scan with no generated-asset mutation. */
MIMIRCOMPOSITEEDITOR_API bool MHScanSourcesOperation(
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError);

/** Name-domain diagnostics only; other payload errors do not change this result. */
MIMIRCOMPOSITEEDITOR_API bool MHValidateNamesOperation(
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError);

/** Read-only live verification of managed MIs plus strict managed-mesh edit audit. */
MIMIRCOMPOSITEEDITOR_API bool MHVerifyMaterialsOperation(
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings,
    bool bStrict,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError);

/** Read-only live verification of managed composites plus strict managed-mesh edit audit. */
MIMIRCOMPOSITEEDITOR_API bool MHVerifyCompositesOperation(
    const FString& SourceRoot,
    bool bStrict,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError);

/** Focused object seam used by both explicit Verify operations and automation. */
MIMIRCOMPOSITEEDITOR_API void MHVerifyManagedStaticMeshLocalEdit(
    const UStaticMesh& StaticMesh,
    bool bStrict,
    FMHSourceAnalysisEntry& InOutEntry);

/** Shared process result policy: 2 usage/configuration, 1 operation/E, 0 success/W. */
MIMIRCOMPOSITEEDITOR_API int32 MHSourceCommandletExitCode(
    bool bUsageValid,
    bool bOperationSucceeded,
    const FMHSourceAnalysis& Analysis);

} // namespace UE::MimirComposite
