#include "Codec/MHCompositeCodec.h"

#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite::Tests
{
namespace Private
{

TArray<uint8> Utf8Bytes(const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text, Text.Len());
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

FString MakeCompositeJson(const FString& NodesJson, const FString& VersionText, const FString& NameText)
{
	return FString::Printf(TEXT(R"({
  "schema": "mh.composite",
  "schema_version": %s,
  "uid": "b0ca6032-8884-534d-8ceb-20f4a4edb766",
  "name": "%s",
  "properties": {},
  "nodes": [%s]
})"), *VersionText, *NameText, *NodesJson);
}

FString MakeNodeJson(const FString& NodeUid, const FString& ParentText, const FString& KindFields)
{
	return FString::Printf(TEXT(R"({
    "node_uid": "%s",
    "parent_uid": %s,
    %s,
    "display_name": "node",
    "local_transform": {
      "translation_cm": [0.0, 0.0, 0.0],
      "rotation_quat": [0.0, 0.0, 0.0, 1.0],
      "scale": [1.0, 1.0, 1.0]
    },
    "properties": {}
  })"), *NodeUid, *ParentText, *KindFields);
}

bool ExpectFailure(
	FAutomationTestBase& Test,
	const FString& Label,
	const FString& Json,
	const TCHAR* ExpectedPrefix)
{
	FMHCompositeDocument Document;
	const FMHCanonicalResult Result = MHParseCompositeV2(Utf8Bytes(Json), Document);
	if (Result.bSuccess)
	{
		Test.AddError(FString::Printf(TEXT("%s: expected failure %s, parse succeeded"), *Label, ExpectedPrefix));
		return false;
	}
	if (!Result.Error.StartsWith(ExpectedPrefix))
	{
		Test.AddError(FString::Printf(
			TEXT("%s: expected error prefix %s, got %s"),
			*Label,
			ExpectedPrefix,
			*Result.Error));
		return false;
	}
	return true;
}

} // namespace Private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMHCompositeCodecTest,
	"Mimir.C1.CompositeCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeCodecTest::RunTest(const FString& Parameters)
{
	FString GoldenRoot;
	if (!ResolveGoldenRoot(*this, GoldenRoot))
	{
		return false;
	}

	// The committed v2 cycle fixture parses into the expected document image.
	TArray<uint8> FixtureBytes;
	const FString FixturePath =
		FPaths::Combine(GoldenRoot, TEXT("fixtures/composite_cycle/cycle_a.composite"));
	if (!FFileHelper::LoadFileToArray(FixtureBytes, *FixturePath))
	{
		AddError(FString::Printf(TEXT("cannot read fixture %s"), *FixturePath));
		return false;
	}
	FMHCompositeDocument Fixture;
	const FMHCanonicalResult FixtureResult = MHParseCompositeV2(FixtureBytes, Fixture);
	if (!TestTrue(TEXT("cycle_a fixture parses"), FixtureResult.bSuccess))
	{
		AddError(FixtureResult.Error);
		return false;
	}
	TestEqual(TEXT("fixture uid"), Fixture.Uid, TEXT("b0ca6032-8884-534d-8ceb-20f4a4edb766"));
	TestEqual(TEXT("fixture name"), Fixture.Name, TEXT("cycle_a"));
	TestEqual(TEXT("fixture resource properties"), Fixture.ResourcePropertiesJson, TEXT("{}"));
	if (!TestEqual(TEXT("fixture node count"), Fixture.Nodes.Num(), 1))
	{
		return false;
	}
	TestEqual(
		TEXT("fixture node kind"),
		static_cast<int32>(Fixture.Nodes[0].Kind),
		static_cast<int32>(EMHCompositeNodeKind::CompositeRef));
	TestEqual(
		TEXT("fixture node resource"),
		Fixture.Nodes[0].ResourceUid,
		TEXT("77829da7-624e-5c91-a132-82178e866a80"));

	const FString UidA = TEXT("f6d91b61-1922-5e94-aed2-42198862ee65");
	const FString UidB = TEXT("a0ccf18c-2e7a-4270-8cf2-36505a060e3d");
	const FString GroupFields = TEXT(R"("kind": "group")");
	const FString ValidGroup = Private::MakeNodeJson(UidA, TEXT("null"), GroupFields);

	bool bPassed = true;

	// Legacy v1 spelling is excluded from the runtime candidate set.
	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("v1 payload"),
		Private::MakeCompositeJson(ValidGroup, TEXT("1"), TEXT("cycle_a")),
		TEXT("MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED"));

	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("future version"),
		Private::MakeCompositeJson(ValidGroup, TEXT("3"), TEXT("cycle_a")),
		TEXT("MH_E_UNKNOWN_SCHEMA_VERSION"));

	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("non-ascii name"),
		Private::MakeCompositeJson(ValidGroup, TEXT("2"), TEXT("стена")),
		TEXT("MH_E_NON_ASCII_RESOURCE_NAME"));

	FString UnknownTopLevel = Private::MakeCompositeJson(ValidGroup, TEXT("2"), TEXT("cycle_a"));
	UnknownTopLevel.ReplaceInline(TEXT("\"properties\": {}"), TEXT("\"properties\": {}, \"extra\": 1"));
	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("unknown top-level field"),
		UnknownTopLevel,
		TEXT("MH_E_INVALID_COMPOSITE"));

	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("reserved kind"),
		Private::MakeCompositeJson(
			Private::MakeNodeJson(UidA, TEXT("null"), TEXT(R"("kind": "actor")")),
			TEXT("2"),
			TEXT("cycle_a")),
		TEXT("MH_E_UNSUPPORTED_NODE_KIND"));

	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("dangling parent"),
		Private::MakeCompositeJson(
			Private::MakeNodeJson(UidA, FString::Printf(TEXT("\"%s\""), *UidB), GroupFields),
			TEXT("2"),
			TEXT("cycle_a")),
		TEXT("MH_E_INVALID_COMPOSITE"));

	const FString CycleNodes = FString::Printf(
		TEXT("%s, %s"),
		*Private::MakeNodeJson(UidA, FString::Printf(TEXT("\"%s\""), *UidB), GroupFields),
		*Private::MakeNodeJson(UidB, FString::Printf(TEXT("\"%s\""), *UidA), GroupFields));
	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("parent cycle"),
		Private::MakeCompositeJson(CycleNodes, TEXT("2"), TEXT("cycle_a")),
		TEXT("MH_E_PARENT_CYCLE"));

	FString ZeroScale = Private::MakeCompositeJson(ValidGroup, TEXT("2"), TEXT("cycle_a"));
	ZeroScale.ReplaceInline(TEXT("\"scale\": [1.0, 1.0, 1.0]"), TEXT("\"scale\": [1.0, 0.0, 1.0]"));
	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("zero scale"),
		ZeroScale,
		TEXT("MH_E_INVALID_SCALE"));

	FString BadQuat = Private::MakeCompositeJson(ValidGroup, TEXT("2"), TEXT("cycle_a"));
	BadQuat.ReplaceInline(
		TEXT("\"rotation_quat\": [0.0, 0.0, 0.0, 1.0]"),
		TEXT("\"rotation_quat\": [0.0, 0.0, 0.0, 2.0]"));
	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("non-normalized quaternion"),
		BadQuat,
		TEXT("MH_E_INVALID_COMPOSITE"));

	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("group with resource_uid"),
		Private::MakeCompositeJson(
			Private::MakeNodeJson(
				UidA,
				TEXT("null"),
				TEXT(R"("kind": "group", "resource_uid": "77829da7-624e-5c91-a132-82178e866a80")")),
			TEXT("2"),
			TEXT("cycle_a")),
		TEXT("MH_E_INVALID_COMPOSITE"));

	bPassed &= Private::ExpectFailure(
		*this,
		TEXT("mesh without resource_uid"),
		Private::MakeCompositeJson(
			Private::MakeNodeJson(UidA, TEXT("null"), TEXT(R"("kind": "mesh")")),
			TEXT("2"),
			TEXT("cycle_a")),
		TEXT("MH_E_INVALID_COMPOSITE"));

	return bPassed;
}

} // namespace UE::MimirComposite::Tests
