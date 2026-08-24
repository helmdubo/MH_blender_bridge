#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Source/MHSourceAnalyzer.h"
#include "MHSourceImporter.generated.h"

class UMaterialInstanceConstant;
class UMHCompositeAsset;
class UStaticMesh;

namespace UE::MimirComposite
{

/** Empty ResourceKeys means the complete stable source snapshot. */
struct MIMIRCOMPOSITEEDITOR_API FMHImportSourcesScope
{
    TArray<FMHResourceKey> ResourceKeys;

    static FMHImportSourcesScope All() { return FMHImportSourcesScope(); }
};

/**
 * Filters an already-built plan without re-reading source files. C1 uses this
 * after the one full resolver/detector pass; dependency closure lands with the
 * executable Plan in C2.
 */
MIMIRCOMPOSITEEDITOR_API void MHFilterAnalysisToScope(
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& InOutAnalysis);

/** Pure coordinator core: analyze/filter only, with no asset mutation. */
MIMIRCOMPOSITEEDITOR_API bool MHBuildSourceImportPlan(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted);

} // namespace UE::MimirComposite

/**
 * Public import coordinator seam from docs/07 section 2.
 *
 * ImportSources executes the available v4 builders and leaves unsupported
 * resource kinds in the plan; bOutExecuted makes execution explicit to callers.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHSourceImporter final : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** Game-thread only. Executes material -> static mesh -> Composite in dependency order. */
    bool ImportSources(
        const UE::MimirComposite::FMHImportSourcesScope& Scope,
        UE::MimirComposite::FMHSourceAnalysis& OutAnalysis,
        bool& bOutExecuted);

    /** Explicit source-wins mesh rebuild; bypasses equal-hash NO_CHANGE. */
    bool ReimportStaticMesh(
        UStaticMesh* StaticMesh,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Explicit source-wins material rebuild; reapplies the complete managed source document. */
    bool ReimportMaterial(
        UMaterialInstanceConstant* Material,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Manual file-drop adapter. The file must already be the unique source_root candidate. */
    bool ImportCompositeFile(
        const FString& Filename,
        const FString& TargetPackageName,
        UMHCompositeAsset*& OutAsset,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Explicit full-overwrite publish. Adopt folder/name are required for unmanaged MIs. */
    bool PublishMaterial(
        UMaterialInstanceConstant* Material,
        const FString& AdoptFolder,
        const FString& AdoptLogicalName,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Opens the S2 folder+name modal only when the MI has no source receipt. */
    bool PublishMaterialInteractive(
        UMaterialInstanceConstant* Material,
        TArray<FString>& OutWarnings,
        FString& OutError);

    /** Explicit full-overwrite Composite publish; folder/name adopt unmanaged assets. */
    bool PublishComposite(
        UMHCompositeAsset* Asset,
        const FString& AdoptFolder,
        const FString& AdoptLogicalName,
        TArray<FString>& OutWarnings,
        FString& OutError);

private:
    void OnAssetRegistryFilesLoaded();
    void RunStartupPlan();
    void PresentPlan(const UE::MimirComposite::FMHSourceAnalysis& Analysis) const;

    FDelegateHandle FilesLoadedHandle;
    bool bStartupPlanRan = false;
};
