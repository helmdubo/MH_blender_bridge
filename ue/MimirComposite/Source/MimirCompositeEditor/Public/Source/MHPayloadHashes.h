#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

/** Raw byte hash used only for reader-side change detection. */
MIMIRCOMPOSITEEDITOR_API FString MHRawPayloadHash(TConstArrayView<uint8> Bytes);

/** Exact tagged representation emitted by MHRawPayloadHash. */
MIMIRCOMPOSITEEDITOR_API bool MHIsCanonicalRawPayloadHash(const FString& Value);

} // namespace UE::MimirComposite
