#include "Canonical/MHCanonical.h"

#include "Diagnostics/MHFbxDump.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite::Tests
{
namespace Private
{

bool VerifyDump(
	FAutomationTestBase& Test,
	const FString& FixturePath,
	const FString& ExpectedPath,
	const bool bFull)
{
	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!MH::FbxDump::BuildDom(FixturePath, bFull, Root, Error))
	{
		Test.AddError(FString::Printf(TEXT("fbxdump failed for %s: %s"), *FixturePath, *Error));
		return false;
	}
	if (!Root.IsValid() || Root->GetStringField(TEXT("tag")) != TEXT("mh.fbxdump:1") ||
		Root->GetBoolField(TEXT("full")) != bFull)
	{
		Test.AddError(TEXT("fbxdump root tag or mode mismatch"));
		return false;
	}

	const TSharedPtr<FJsonObject> Summary = Root->GetObjectField(TEXT("summary"));
	const TArray<TSharedPtr<FJsonValue>> LodLevels = Summary->GetArrayField(TEXT("lod_levels"));
	int64 LodLevel = -1;
	if (LodLevels.Num() != 1 || !LodLevels[0]->TryGetNumber(LodLevel) || LodLevel != 0)
	{
		Test.AddError(TEXT("axis_probe must report exactly effective LOD level 0"));
		return false;
	}

	TArray<uint8> ActualBytes;
	const FMHCanonicalResult SerializeResult = MHCanonicalJsonBytes(
		MakeShared<FJsonValueObject>(Root),
		ActualBytes);
	if (!SerializeResult.bSuccess)
	{
		Test.AddError(FString::Printf(TEXT("fbxdump canonical serialization failed: %s"), *SerializeResult.Error));
		return false;
	}
	ActualBytes.Add(static_cast<uint8>('\n'));

	TArray<uint8> ExpectedBytes;
	if (!FFileHelper::LoadFileToArray(ExpectedBytes, *ExpectedPath))
	{
		Test.AddError(FString::Printf(TEXT("cannot read expected fbxdump: %s"), *ExpectedPath));
		return false;
	}
	if (ActualBytes != ExpectedBytes)
	{
		Test.AddError(FString::Printf(
			TEXT("fbxdump byte mismatch for %s (expected %d bytes, actual %d bytes)"),
			*ExpectedPath,
			ExpectedBytes.Num(),
			ActualBytes.Num()));
		return false;
	}
	return true;
}

} // namespace Private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMHFbxDumpAxisProbeTest,
	"Mimir.C0.FbxDump.AxisProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHFbxDumpAxisProbeTest::RunTest(const FString& Parameters)
{
	FString GoldenRoot;
	if (!ResolveGoldenRoot(*this, GoldenRoot))
	{
		return false;
	}

	const FString FixturePath = FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"));
	bool bPassed = Private::VerifyDump(
		*this,
		FixturePath,
		FPaths::Combine(GoldenRoot, TEXT("fbxdump/axis_probe.summary.json")),
		false);
	bPassed &= Private::VerifyDump(
		*this,
		FixturePath,
		FPaths::Combine(GoldenRoot, TEXT("fbxdump/axis_probe.full.json")),
		true);
	return bPassed;
}

} // namespace UE::MimirComposite::Tests
