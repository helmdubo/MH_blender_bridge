#pragma once

#include "Commandlets/Commandlet.h"
#include "MHAnalyzeSourcesCommandlet.generated.h"

/**
 * -run=MHAnalyzeSources -root=<source_root>
 *
 * Headless Source Protocol v4 reader pass: scans the source payload set through
 * the current Project Resource Index services and prints one line per
 * classified ResourceKey. Nothing is imported and the source tree is never
 * written.
 * Without -root the commandlet falls back to the project SourceRoot setting.
 * Optional -report writes deterministic mh.analyze_sources:4 JSON through the
 * Saved/Mimir output guard. Exit code 0 means no MH_E_*, 1 means an operation
 * or resource error, and 2 means invalid usage.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHAnalyzeSourcesCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
