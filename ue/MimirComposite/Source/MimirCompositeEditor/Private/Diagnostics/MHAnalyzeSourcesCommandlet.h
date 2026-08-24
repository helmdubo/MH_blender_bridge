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
 * -report remains rejected until S6 implements mh.analyze_sources:4. Exit code
 * 0 when no MH_E_* was raised, 1 otherwise, 2 on usage errors or a forbidden
 * output option.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHAnalyzeSourcesCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
