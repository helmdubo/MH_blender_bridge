#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

/** Mirrored machine-code registry. Exact counts are golden-tested. */
MIMIRCOMPOSITERUNTIME_API const TSet<FString>& MHRegisteredErrorCodes();
MIMIRCOMPOSITERUNTIME_API const TSet<FString>& MHRegisteredWarningCodes();

} // namespace UE::MimirComposite
