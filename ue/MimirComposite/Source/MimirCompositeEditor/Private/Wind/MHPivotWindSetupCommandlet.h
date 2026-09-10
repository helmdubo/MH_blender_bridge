#pragma once

#include "Commandlets/Commandlet.h"
#include "MHPivotWindSetupCommandlet.generated.h"

/**
 * -run=MHPivotWindSetup -DestinationRoot=/Game/MH/Wind
 *     [-MasterRoot=/Game/MimirHead/MasterMaterials]
 *
 * Creates versioned wind resources and a controller Blueprint. MasterRoot is
 * optional: when supplied, attaches only rendinst_tree_colored and
 * rendinst_tree_colored_alpha_split after preflighting both. Existing resources
 * must match this implementation; partial/stale sets are rejected, never rebuilt.
 * Requires -AllowCommandletRendering for shader validation (no -NullRHI). Returns zero
 * only after compilation and saving all changed assets; source files and material
 * instances are never modified. A failed run saves no assets before validation.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHPivotWindSetupCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
