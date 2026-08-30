#pragma once

#include "CoreMinimal.h"

class FReimportHandler;
class UObject;

namespace UE::MimirComposite
{

/** Exact managed-mesh admission used by the editor reimport handler. */
MIMIRCOMPOSITEEDITOR_API bool MHCanReimportManagedStaticMesh(
    UObject* Object,
    TArray<FString>& OutFilenames);

/** Module-owned registration lifetime. */
MIMIRCOMPOSITEEDITOR_API void MHStartupManagedStaticMeshReimportHandler();
MIMIRCOMPOSITEEDITOR_API void MHShutdownManagedStaticMeshReimportHandler();

#if WITH_DEV_AUTOMATION_TESTS
MIMIRCOMPOSITEEDITOR_API FReimportHandler* MHGetManagedStaticMeshReimportHandlerForTests();
#endif

} // namespace UE::MimirComposite
