#pragma once

#include "CoreMinimal.h"
#include "Source/MHChangeDetector.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{

enum class EMHSourceChange : uint8
{
    Create,
    Reimport,
    Move,
    NoChange,
    Remove,
    Blocked
};

MIMIRCOMPOSITEEDITOR_API const TCHAR* MHSourceChangeLabel(EMHSourceChange Change);

struct MIMIRCOMPOSITEEDITOR_API FMHSourceAnalysisEntry
{
    FMHResourceKey Key;
    FString PayloadPath;
    FString SourcePath;
    FString RawHash;
    EMHSourceChange Change = EMHSourceChange::NoChange;
    TArray<FString> Warnings;
    TArray<FString> Errors;
};

struct MIMIRCOMPOSITEEDITOR_API FMHSourceAnalysis
{
    TArray<FMHSourceAnalysisEntry> Entries;
    TArray<FString> Warnings;
    TArray<FString> Errors;

    const FMHSourceAnalysisEntry* Find(const FMHResourceKey& Key) const;
    int32 CountOf(EMHSourceChange Change) const;
    bool HasErrors() const;
};

MIMIRCOMPOSITEEDITOR_API void MHAnalyzeSources(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis);

} // namespace UE::MimirComposite
