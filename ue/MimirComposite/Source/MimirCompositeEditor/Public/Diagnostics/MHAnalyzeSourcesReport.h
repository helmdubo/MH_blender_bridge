#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceAnalyzer.h"

namespace UE::MimirComposite
{

inline constexpr const TCHAR* MHAnalyzeSourcesReportTag = TEXT("mh.analyze_sources:4");

/** Deterministic v4 diagnostics report. Arrays and entries are sorted copies. */
MIMIRCOMPOSITEEDITOR_API bool MHSerializeAnalyzeSourcesReportV4(
    const FString& SourceRoot,
    const FMHSourceAnalysis& Analysis,
    TArray<uint8>& OutBytes,
    FString& OutError);

/** Read-back guard for the report writer; rejects the wrong tag or malformed JSON. */
MIMIRCOMPOSITEEDITOR_API bool MHValidateAnalyzeSourcesReportV4(
    const TArray<uint8>& Bytes,
    FString& OutError);

/**
 * Writes beneath Saved/Mimir only: sibling tmp -> exact read-back/tag check ->
 * atomic replace. RequestedPath may be relative to Saved/Mimir.
 */
MIMIRCOMPOSITEEDITOR_API bool MHWriteAnalyzeSourcesReportV4(
    const FString& SourceRoot,
    const FString& RequestedPath,
    const FMHSourceAnalysis& Analysis,
    FString& OutAbsolutePath,
    FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
MIMIRCOMPOSITEEDITOR_API void MHSetBeforeAnalyzeSourcesReportReadBackTestHook(
    TFunction<void(const FString&)> Hook);
#endif

} // namespace UE::MimirComposite
