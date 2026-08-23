#pragma once

#include "CoreMinimal.h"

namespace MH::CompositeDump
{

struct FDumpOutput
{
    /** Human-readable hierarchy and scan summary lines. */
    TArray<FString> Lines;

    /** MH_W_* diagnostics: duplicate resolutions and legacy skips. */
    TArray<FString> Warnings;

    /** MH_E_* diagnostics, including quarantine; non-empty blocks the dump. */
    TArray<FString> Errors;
};

/**
 * Builds the hierarchy snapshot of one mh.composite v2 payload. With a
 * non-empty SourceRoot the Clean Sources v2 payload set is scanned and
 * composite_ref dependencies are expanded recursively with per-resource
 * resolve statuses. Returns false when Errors is non-empty.
 */
bool BuildDump(const FString& FilePath, const FString& SourceRoot, FDumpOutput& Out);

} // namespace MH::CompositeDump
