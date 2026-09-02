#include "Composite/MHMaterializeLayout.h"

namespace UE::MimirComposite
{

FMHMaterializeResult MHMaterializeLayout(
    const FMHCompiledRecipe& Recipe,
    const int32 Seed,
    const int32 AppearanceSeed,
    const FTransform& ActorTransform)
{
    FMHMaterializeResult Result;
    Result.Seed = Seed;
    Result.AppearanceSeed = AppearanceSeed;
    Result.Error = TEXT("MH_E_NOT_IMPLEMENTED: MHMaterializeLayout is not implemented (R2b-1 red)");
    return Result;
}

} // namespace UE::MimirComposite
