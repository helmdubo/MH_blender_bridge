#pragma once

#include "CoreMinimal.h"

class FAutomationTestBase;

namespace UE::MimirComposite::Tests
{

// Resolves the repository `golden/` directory for Mimir.C0 automation. An
// explicit `-MHGoldenRoot=` argument wins and must be valid; otherwise the
// `MH_GOLDEN_ROOT` environment variable and the in-repo plugin layout
// (`<plugin>/../../golden`) are probed, so the tests also run from the editor
// Session Frontend without extra command-line arguments. On failure an error
// naming every rejected candidate is added to `Test` and false is returned.
bool ResolveGoldenRoot(FAutomationTestBase& Test, FString& OutGoldenRoot);

} // namespace UE::MimirComposite::Tests
