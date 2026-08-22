#pragma once

#include "CoreMinimal.h"

namespace UE::MimirComposite
{

/**
 * Resolve reader-owned output paths without allowing writes into source_root.
 * Relative paths are rooted under ProjectSavedDir()/Mimir; absolute paths are
 * accepted only inside that same boundary. Filesystem aliases fail closed.
 */
MIMIRCOMPOSITEEDITOR_API bool MHResolveReaderOutputPath(
    const FString& SourceRoot,
    const FString& RequestedPath,
    FString& OutAbsolutePath,
    FString& OutError);

} // namespace UE::MimirComposite
