#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Source/MHSourceAnalyzer.h"
#include "MHSourceImporter.generated.h"

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

/** Pure C1 coordinator core: analyze/filter only, with no asset or Ledger write. */
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
 * Gate C1 implements Scan -> Resolve -> Analyze -> Plan presentation only.
 * ImportSources never mutates assets or advances the Ledger until the C2
 * builders exist; bOutExecuted makes that boundary explicit to callers.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHSourceImporter final : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** Game-thread only. C1 returns a plan and always leaves bOutExecuted false. */
    bool ImportSources(
        const UE::MimirComposite::FMHImportSourcesScope& Scope,
        UE::MimirComposite::FMHSourceAnalysis& OutAnalysis,
        bool& bOutExecuted);

private:
    void OnAssetRegistryFilesLoaded();
    void RunStartupPlan();
    void PresentPlan(const UE::MimirComposite::FMHSourceAnalysis& Analysis) const;

    FDelegateHandle FilesLoadedHandle;
    bool bStartupPlanRan = false;
};
