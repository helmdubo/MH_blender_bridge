#include "Source/MHCompositeWave.h"

#include "Codec/MHCompositeCodec.h"
#include "HAL/FileManager.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Source/MHPayloadScanResolver.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMHCompositeWaveTest,
	"Mimir.C1.CompositeWave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeWaveTest::RunTest(const FString& Parameters)
{
	FString GoldenRoot;
	if (!ResolveGoldenRoot(*this, GoldenRoot))
	{
		return false;
	}

	const FString TempRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("MimirCompositeTests"),
		FString::Printf(TEXT("Wave_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	const FString FixtureDir = FPaths::Combine(GoldenRoot, TEXT("fixtures/composite_cycle"));
	FString CycleAText;
	FString CycleBText;
	if (!FFileHelper::LoadFileToString(CycleAText, *FPaths::Combine(FixtureDir, TEXT("cycle_a.composite"))) ||
		!FFileHelper::LoadFileToString(CycleBText, *FPaths::Combine(FixtureDir, TEXT("cycle_b.composite"))))
	{
		AddError(TEXT("cannot read composite_cycle fixtures"));
		return false;
	}
	FFileHelper::SaveStringToFile(
		CycleAText,
		*FPaths::Combine(TempRoot, TEXT("cycle_a.composite")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(
		CycleBText,
		*FPaths::Combine(TempRoot, TEXT("cycle_b.composite")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	FMHPayloadScanResolver Resolver(TempRoot);
	FString Error;
	if (!TestTrue(TEXT("scan initializes"), Resolver.Initialize(Error)))
	{
		AddError(Error);
		return false;
	}

	bool bPassed = true;

	// The committed cycle pair must be reported as MH_E_COMPOSITE_CYCLE.
	{
		FMHCompositeDocument Root;
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(TempRoot, TEXT("cycle_a.composite")));
		const FMHCanonicalResult Parsed = MHParseCompositeV2(Bytes, Root);
		if (!TestTrue(TEXT("cycle_a parses"), Parsed.bSuccess))
		{
			AddError(Parsed.Error);
			return false;
		}

		FMHCompositeWaveResult Wave;
		MHWalkCompositeWave(Resolver, Root, TEXT("cycle_a.composite"), Wave);
		bPassed &= TestEqual(TEXT("both composites reached"), Wave.Composites.Num(), 2);
		bool bCycleReported = false;
		for (const FString& WaveError : Wave.Errors)
		{
			bCycleReported |= WaveError.StartsWith(TEXT("MH_E_COMPOSITE_CYCLE"));
		}
		bPassed &= TestTrue(TEXT("cycle reported"), bCycleReported);
	}

	// A missing composite_ref dependency stays an unresolved node, not a crash.
	{
		FString Orphan = CycleAText;
		Orphan.ReplaceInline(
			TEXT("b0ca6032-8884-534d-8ceb-20f4a4edb766"),
			TEXT("11111111-2222-4333-8444-555555555555"));
		Orphan.ReplaceInline(
			TEXT("77829da7-624e-5c91-a132-82178e866a80"),
			TEXT("99999999-9999-4999-8999-999999999999"));

		FMHCompositeDocument Root;
		const FTCHARToUTF8 Utf8(*Orphan, Orphan.Len());
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		const FMHCanonicalResult Parsed = MHParseCompositeV2(Bytes, Root);
		if (!TestTrue(TEXT("orphan root parses"), Parsed.bSuccess))
		{
			AddError(Parsed.Error);
			return false;
		}

		FMHCompositeWaveResult Wave;
		MHWalkCompositeWave(Resolver, Root, TEXT("orphan.composite"), Wave);
		bPassed &= TestEqual(TEXT("unresolved dependency recorded"), Wave.UnresolvedComposites.Num(), 1);
		bool bNotFoundReported = false;
		for (const FString& WaveError : Wave.Errors)
		{
			bNotFoundReported |= WaveError.StartsWith(TEXT("MH_E_RESOURCE_NOT_FOUND"));
		}
		bPassed &= TestTrue(TEXT("missing resource reported"), bNotFoundReported);
	}

	IFileManager::Get().DeleteDirectory(*TempRoot, false, true);
	return bPassed;
}

} // namespace UE::MimirComposite::Tests
