#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Random/MHRandomStream.h"
#include "UObject/ObjectPtr.h"

class USceneComponent;

namespace UE::MimirComposite
{

/** Observational report only: never re-resolves, samples, or changes signature bytes. */
MIMIRCOMPOSITERUNTIME_API bool MHBuildCompositePlanReport(
    const FMHResolvedCompositePlan& Plan,
    const TArray<TObjectPtr<USceneComponent>>& MaterializedComponents,
    TSharedPtr<FJsonObject>& OutReport,
    FString& OutError);

/** Fixed diagnostic destination; unknown lanes are rejected by the writer. */
MIMIRCOMPOSITERUNTIME_API FString MHCompositeParityReportPath(const FString& Lane);

/** Writes UTF-8/17-digit JSON below physically alias-free Saved/Mimir/S6 only. */
MIMIRCOMPOSITERUNTIME_API bool MHWriteCompositeParityReport(
    const FString& Lane,
    const FString& WorldType,
    bool bRuntimeModulesOnly,
    const TArray<TSharedPtr<FJsonValue>>& Plans,
    FString& OutError);

} // namespace UE::MimirComposite
