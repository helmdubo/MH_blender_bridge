#include "MHGoldenRoot.h"

#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite::Tests
{
namespace
{

bool IsGoldenRoot(const FString& Candidate)
{
	return FPaths::FileExists(FPaths::Combine(Candidate, TEXT("canonical_vectors.json")));
}

} // namespace

bool ResolveGoldenRoot(FAutomationTestBase& Test, FString& OutGoldenRoot)
{
	FString ExplicitRoot;
	if (FParse::Value(FCommandLine::Get(), TEXT("MHGoldenRoot="), ExplicitRoot))
	{
		ExplicitRoot = FPaths::ConvertRelativePathToFull(ExplicitRoot);
		if (!IsGoldenRoot(ExplicitRoot))
		{
			Test.AddError(FString::Printf(
				TEXT("-MHGoldenRoot points at '%s' but canonical_vectors.json is not there"),
				*ExplicitRoot));
			return false;
		}
		OutGoldenRoot = ExplicitRoot;
		return true;
	}

	TArray<FString> Candidates;
	const FString FromEnvironment = FPlatformMisc::GetEnvironmentVariable(TEXT("MH_GOLDEN_ROOT"));
	if (!FromEnvironment.IsEmpty())
	{
		Candidates.Add(FPaths::ConvertRelativePathToFull(FromEnvironment));
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MimirComposite"));
	if (Plugin.IsValid())
	{
		Candidates.Add(FPaths::ConvertRelativePathToFull(
			FPaths::Combine(Plugin->GetBaseDir(), TEXT(".."), TEXT(".."), TEXT("golden"))));
	}

	for (const FString& Candidate : Candidates)
	{
		if (IsGoldenRoot(Candidate))
		{
			OutGoldenRoot = Candidate;
			return true;
		}
	}

	Test.AddError(FString::Printf(
		TEXT("Unable to resolve the repository golden directory (candidates: '%s'). ")
		TEXT("Pass -MHGoldenRoot=<repo>/golden or set the MH_GOLDEN_ROOT environment variable."),
		*FString::Join(Candidates, TEXT("', '"))));
	return false;
}

} // namespace UE::MimirComposite::Tests
