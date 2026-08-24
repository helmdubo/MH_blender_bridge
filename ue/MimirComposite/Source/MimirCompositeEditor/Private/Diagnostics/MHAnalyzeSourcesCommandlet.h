#pragma once

#include "Commandlets/Commandlet.h"
#include "MHAnalyzeSourcesCommandlet.generated.h"

/**
 * -run=MHAnalyzeSources -root=<source_root> [-ledger=<snapshot.json>]
 *
 * Headless Source Protocol v4 reader pass: scans the source payload set,
 * compares it with the deprecated transitional Ledger snapshot and prints one
 * line per classified ResourceKey. Nothing is imported and the source tree is
 * never written.
 * Without -root the commandlet falls back to the project SourceRoot setting.
 * -writeledger is explicitly rejected in C1: Analyze/Plan cannot advance
 * applied state. -report is also rejected until OPEN-V4-5 ratifies a v4 JSON
 * diagnostic schema. Exit code 0 when no MH_E_* was raised, 1 otherwise, 2 on
 * usage errors or a forbidden output/writer option.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHAnalyzeSourcesCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
