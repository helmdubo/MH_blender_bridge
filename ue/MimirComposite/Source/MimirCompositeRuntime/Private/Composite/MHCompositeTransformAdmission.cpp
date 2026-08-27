#include "Composite/MHCompositeTransformAdmission.h"

#include "Math/Transform.h"

namespace UE::MimirComposite
{

bool MHMatrixElementsWithinTrsTolerance(
    const FMatrix& Matrix,
    const FMatrix& Reconstructed)
{
    constexpr float Float32Epsilon = 1.1920928955078125e-7f;
    constexpr float Ulps = 8.0f;
    for (int32 Row = 0; Row < 4; ++Row)
    {
        for (int32 Column = 0; Column < 4; ++Column)
        {
            const float Source = static_cast<float>(Matrix.M[Row][Column]);
            const float Restored = static_cast<float>(Reconstructed.M[Row][Column]);
            if (!FMath::IsFinite(Source) || !FMath::IsFinite(Restored)) return false;
            const float Magnitude = FMath::Max3(1.0f, FMath::Abs(Source), FMath::Abs(Restored));
            if (FMath::Abs(Source - Restored) > Ulps * Float32Epsilon * Magnitude)
            {
                return false;
            }
        }
    }
    return true;
}

bool MHIsRepresentableTransformMatrix(const FMatrix& Matrix)
{
    for (int32 Row = 0; Row < 4; ++Row)
    {
        for (int32 Column = 0; Column < 4; ++Column)
        {
            if (!FMath::IsFinite(Matrix.M[Row][Column])) return false;
        }
    }
    const FTransform Decomposed(Matrix);
    return MHMatrixElementsWithinTrsTolerance(Matrix, Decomposed.ToMatrixWithScale());
}

bool MHValidateResolvedPlacementTransforms(const FMHResolvedCompositePlan& Plan, const FTransform& PlacementTransform, FString& OutError)
{
    OutError.Reset();
    const FMatrix PlacementMatrix = PlacementTransform.ToMatrixWithScale();
    for (const FMHResolvedCompositeNode& Node : Plan.Nodes)
    {
        if (!MHIsRepresentableTransformMatrix(Node.WorldMatrix) || !MHIsRepresentableTransformMatrix(Node.WorldMatrix * PlacementMatrix))
        {
            OutError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: ") + Node.NodePath + TEXT(" cannot round-trip through FTransform within 8 float32 ULP");
            return false;
        }
    }
    return true;
}

} // namespace UE::MimirComposite
