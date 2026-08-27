#pragma once

#include "CoreMinimal.h"
#include "Random/MHRandomStream.h"

namespace UE::MimirComposite
{

/** Shared 8-ULP host reconstruction predicate; no approximation or repair. */
MIMIRCOMPOSITERUNTIME_API bool MHMatrixElementsWithinTrsTolerance(const FMatrix& Matrix, const FMatrix& Reconstructed);
MIMIRCOMPOSITERUNTIME_API bool MHIsRepresentableTransformMatrix(const FMatrix& Matrix);

/** Admission before any Editor/PIE/runtime component mutation. */
MIMIRCOMPOSITERUNTIME_API bool MHValidateResolvedPlacementTransforms(
    const FMHResolvedCompositePlan& Plan, const FTransform& PlacementTransform, FString& OutError);

} // namespace UE::MimirComposite
