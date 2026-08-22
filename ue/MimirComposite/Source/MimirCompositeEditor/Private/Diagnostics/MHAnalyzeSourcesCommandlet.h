#pragma once

#include "Commandlets/Commandlet.h"
#include "MHAnalyzeSourcesCommandlet.generated.h"

/**
 * -run=MHAnalyzeSources -root=<source_root> [-ledger=<snapshot.json>]
 *                       [-report=<out.json>]
 *
 * Headless reader pass of docs/07 section 4: scans the Clean Sources v2 payload
 * set, compares it with a Ledger snapshot and prints one line per classified
 * ResourceUID. Nothing is imported and the source tree is never written.
 * Without -root the commandlet falls back to the project SourceRoot setting.
 * -writeledger is explicitly rejected in C1: Analyze/Plan cannot advance
 * applied state. Exit code 0 when no MH_E_* was raised, 1 otherwise, 2 on
 * usage errors or a forbidden writer option.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHAnalyzeSourcesCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
