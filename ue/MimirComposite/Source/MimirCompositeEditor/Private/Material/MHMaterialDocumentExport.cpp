#include "Material/MHMaterialDocumentExport.h"

#include "Materials/MaterialInstanceConstant.h"

namespace UE::MimirComposite
{

FString MHGetMaterialDocumentExportLogicalName(const UMaterialInstanceConstant& Material)
{
    return Material.GetName();
}

bool MHPrepareMaterialDocumentExport(
    TConstArrayView<FMHMaterialDocumentExportRequest> Requests,
    const UMHCompositeSettings& Settings,
    const FString& SourceRoot,
    FMHMaterialDocumentExportPlan& OutPlan,
    FString& OutError)
{
    static_cast<void>(Requests);
    static_cast<void>(Settings);
    static_cast<void>(SourceRoot);
    OutPlan = FMHMaterialDocumentExportPlan();
    OutError = TEXT("RED: material document export is not implemented");
    return false;
}

bool MHCommitMaterialDocumentExport(
    const FMHMaterialDocumentExportPlan& Plan,
    const bool bAllowOverwrite,
    FMHMaterialDocumentExportResult& OutResult,
    FString& OutError)
{
    static_cast<void>(Plan);
    static_cast<void>(bAllowOverwrite);
    OutResult = FMHMaterialDocumentExportResult();
    OutError = TEXT("RED: material document export is not implemented");
    return false;
}

} // namespace UE::MimirComposite
