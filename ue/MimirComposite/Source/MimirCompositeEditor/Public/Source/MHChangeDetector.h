#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

class IMHSourceResolver;
struct FMHSourceAnalysis;

/**
 * Replaceable reader-side change-detection seam. Higher-level startup,
 * watcher, prompt and commandlet paths consume only this interface; the
 * implementation decides where applied state is stored.
 */
class MIMIRCOMPOSITEEDITOR_API IMHChangeDetector
{
public:
    virtual ~IMHChangeDetector() = default;

    virtual void DetectChanges(
        IMHSourceResolver& Resolver,
        const FString& SourceRoot,
        FMHSourceAnalysis& OutAnalysis) = 0;
};

} // namespace UE::MimirComposite
