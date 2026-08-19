#include "Source/MHSourceAnalyzer.h"

#include "HAL/FileManager.h"
#include "Ledger/MHImportLedger.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Source/MHPayloadScanResolver.h"

namespace UE::MimirComposite::Tests
{
namespace Private
{

const TCHAR* AnalyzerCompositeUid = TEXT("b0ca6032-8884-534d-8ceb-20f4a4edb766");
const TCHAR* AnalyzerMaterialUid = TEXT("7d995e54-d084-4466-a613-a1cd8f3248b2");

FString AnalyzerTempRoot()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("MimirCompositeTests"),
		FString::Printf(TEXT("Analyzer_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

bool AnalyzerWriteText(const FString& Path, const FString& Text)
{
	return FFileHelper::SaveStringToFile(
		Text,
		*Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString AnalyzerMaterialJson(const int32 Sides)
{
	return FString::Printf(TEXT(R"({
  "schema": "mh.material",
  "schema_version": 1,
  "uid": "%s",
  "name": "m_analyzer",
  "shader_class": "rendinst_simple",
  "params": {
    "sides": %d
  },
  "textures": {}
})"), AnalyzerMaterialUid, Sides);
}

/** Commits every entry the analyzer allows, exactly like the commandlet does. */
int32 AnalyzerAdvanceLedger(const FMHSourceAnalysis& Analysis, TMap<FString, FMHLedgerRow>& Ledger)
{
	int32 Advanced = 0;
	for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
	{
		FMHLedgerRow Row;
		if (MHLedgerRowFromAnalysis(Entry, Row))
		{
			Ledger.Add(Entry.ResourceUid, MoveTemp(Row));
			++Advanced;
		}
	}
	return Advanced;
}

} // namespace Private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMHSourceAnalyzerTest,
	"Mimir.C1.Analyzer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceAnalyzerTest::RunTest(const FString& Parameters)
{
	FString GoldenRoot;
	if (!ResolveGoldenRoot(*this, GoldenRoot))
	{
		return false;
	}

	FString CompositeText;
	const FString FixturePath =
		FPaths::Combine(GoldenRoot, TEXT("fixtures/composite_cycle/cycle_a.composite"));
	if (!FFileHelper::LoadFileToString(CompositeText, *FixturePath))
	{
		AddError(FString::Printf(TEXT("cannot read fixture %s"), *FixturePath));
		return false;
	}

	const FString TempRoot = Private::AnalyzerTempRoot();
	const FString CompositePath = FPaths::Combine(TempRoot, TEXT("a/cycle_a.composite"));
	const FString MovedCompositePath = FPaths::Combine(TempRoot, TEXT("b/cycle_a.composite"));
	const FString DivergentCompositePath = FPaths::Combine(TempRoot, TEXT("c/cycle_a.composite"));
	const FString MaterialPath = FPaths::Combine(TempRoot, TEXT("materials/m_analyzer.material"));

	IFileManager& FileManager = IFileManager::Get();
	bool bPassed = true;

	auto Analyze = [this, &TempRoot](
		const TMap<FString, FMHLedgerRow>& InLedger,
		FMHSourceAnalysis& OutAnalysis) -> bool
	{
		FMHPayloadScanResolver Resolver(TempRoot);
		FString ScanError;
		if (!Resolver.Initialize(ScanError))
		{
			AddError(ScanError);
			return false;
		}
		MHAnalyzeSources(Resolver, TempRoot, InLedger, OutAnalysis);
		return true;
	};

	auto ChangeOf = [](const FMHSourceAnalysis& InAnalysis, const TCHAR* InUid) -> int32
	{
		const FMHSourceAnalysisEntry* Entry = InAnalysis.Find(InUid);
		return Entry != nullptr ? static_cast<int32>(Entry->Change) : -1;
	};

	Private::AnalyzerWriteText(CompositePath, CompositeText);
	Private::AnalyzerWriteText(MaterialPath, Private::AnalyzerMaterialJson(0));

	TMap<FString, FMHLedgerRow> Ledger;

	// 1. An empty Ledger creates every scanned resource.
	{
		FMHSourceAnalysis Analysis;
		if (!Analyze(Ledger, Analysis))
		{
			return false;
		}
		bPassed &= TestEqual(TEXT("two resources analyzed"), Analysis.Entries.Num(), 2);
		bPassed &= TestEqual(
			TEXT("composite is CREATE"),
			ChangeOf(Analysis, Private::AnalyzerCompositeUid),
			static_cast<int32>(EMHSourceChange::Create));
		bPassed &= TestEqual(
			TEXT("material is CREATE"),
			ChangeOf(Analysis, Private::AnalyzerMaterialUid),
			static_cast<int32>(EMHSourceChange::Create));
		bPassed &= TestFalse(TEXT("create raises no error"), Analysis.HasErrors());

		const FMHSourceAnalysisEntry* Composite = Analysis.Find(Private::AnalyzerCompositeUid);
		bPassed &= TestTrue(TEXT("composite entry exists"), Composite != nullptr);
		if (Composite != nullptr)
		{
			bPassed &= TestEqual(TEXT("relative source path"), Composite->SourcePath, TEXT("a/cycle_a.composite"));
			bPassed &= TestEqual(TEXT("composite name"), Composite->Name, TEXT("cycle_a"));
			bPassed &= TestTrue(
				TEXT("composite semantic hash"),
				Composite->DescriptorHash.StartsWith(TEXT("xxh3:")));
			bPassed &= TestTrue(
				TEXT("composite carries no geometry hash"),
				Composite->GeometryHash.IsEmpty());
		}

		bPassed &= TestEqual(TEXT("two rows advanced"), Private::AnalyzerAdvanceLedger(Analysis, Ledger), 2);
	}

	// 2. The unchanged tree is silent against the advanced Ledger.
	{
		FMHSourceAnalysis Analysis;
		if (!Analyze(Ledger, Analysis))
		{
			return false;
		}
		bPassed &= TestEqual(
			TEXT("everything is NO_CHANGE"),
			Analysis.CountOf(EMHSourceChange::NoChange),
			2);
		bPassed &= TestEqual(TEXT("no scan warnings"), Analysis.Warnings.Num(), 0);
		bPassed &= TestFalse(TEXT("no errors"), Analysis.HasErrors());
		for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
		{
			bPassed &= TestEqual(TEXT("no per-resource warning"), Entry.Warnings.Num(), 0);
		}
	}

	// 3. The same payload in another directory is a MOVE.
	{
		bPassed &= TestTrue(
			TEXT("payload written to the new directory"),
			Private::AnalyzerWriteText(MovedCompositePath, CompositeText));
		bPassed &= TestTrue(TEXT("old path gone"), FileManager.Delete(*CompositePath));

		FMHSourceAnalysis Analysis;
		if (!Analyze(Ledger, Analysis))
		{
			return false;
		}
		bPassed &= TestEqual(
			TEXT("composite is MOVE"),
			ChangeOf(Analysis, Private::AnalyzerCompositeUid),
			static_cast<int32>(EMHSourceChange::Move));
		bPassed &= TestEqual(
			TEXT("material stays NO_CHANGE"),
			ChangeOf(Analysis, Private::AnalyzerMaterialUid),
			static_cast<int32>(EMHSourceChange::NoChange));
		bPassed &= TestFalse(TEXT("move raises no error"), Analysis.HasErrors());

		Private::AnalyzerAdvanceLedger(Analysis, Ledger);
	}

	// 4. Changed material params are an UPDATE_PROPERTIES.
	{
		Private::AnalyzerWriteText(MaterialPath, Private::AnalyzerMaterialJson(1));

		FMHSourceAnalysis Analysis;
		if (!Analyze(Ledger, Analysis))
		{
			return false;
		}
		bPassed &= TestEqual(
			TEXT("material is UPDATE_PROPERTIES"),
			ChangeOf(Analysis, Private::AnalyzerMaterialUid),
			static_cast<int32>(EMHSourceChange::UpdateProperties));
		bPassed &= TestEqual(
			TEXT("composite is NO_CHANGE after the move commit"),
			ChangeOf(Analysis, Private::AnalyzerCompositeUid),
			static_cast<int32>(EMHSourceChange::NoChange));
	}

	// 5. A deleted payload leaves an orphan Ledger row.
	{
		bPassed &= TestTrue(TEXT("material deleted"), FileManager.Delete(*MaterialPath));

		FMHSourceAnalysis Analysis;
		if (!Analyze(Ledger, Analysis))
		{
			return false;
		}
		bPassed &= TestEqual(
			TEXT("material is REMOVE"),
			ChangeOf(Analysis, Private::AnalyzerMaterialUid),
			static_cast<int32>(EMHSourceChange::Remove));

		const FMHSourceAnalysisEntry* Material = Analysis.Find(Private::AnalyzerMaterialUid);
		bPassed &= TestTrue(TEXT("orphan entry exists"), Material != nullptr);
		if (Material != nullptr)
		{
			bPassed &= TestFalse(TEXT("orphan never advances the ledger"), Material->bLedgerAdvanceAllowed);
		}
	}

	// 6. A byte rewrite with the same canonical form needs confirmation.
	{
		Private::AnalyzerWriteText(MovedCompositePath, CompositeText + TEXT("\n\n"));

		FMHSourceAnalysis Analysis;
		if (!Analyze(Ledger, Analysis))
		{
			return false;
		}
		bPassed &= TestEqual(
			TEXT("composite is NO_CHANGE_EXTERNAL"),
			ChangeOf(Analysis, Private::AnalyzerCompositeUid),
			static_cast<int32>(EMHSourceChange::NoChangeExternal));

		const FMHSourceAnalysisEntry* Composite = Analysis.Find(Private::AnalyzerCompositeUid);
		bPassed &= TestTrue(TEXT("external entry exists"), Composite != nullptr);
		if (Composite != nullptr)
		{
			bool bWarned = false;
			for (const FString& Warning : Composite->Warnings)
			{
				bWarned |= Warning.StartsWith(TEXT("MH_W_PAYLOAD_EXTERNAL_MODIFIED"));
			}
			bool bBlocked = false;
			for (const FString& Error : Composite->Errors)
			{
				bBlocked |= Error.StartsWith(TEXT("MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED"));
			}
			bPassed &= TestTrue(TEXT("external modification warning"), bWarned);
			bPassed &= TestTrue(TEXT("confirmation is required"), bBlocked);
			bPassed &= TestFalse(TEXT("ledger is not advanced silently"), Composite->bLedgerAdvanceAllowed);
		}
		bPassed &= TestTrue(TEXT("analysis reports the gate"), Analysis.HasErrors());
	}

	// 7. Two divergent revisions of one UID block that resource only.
	{
		FString Divergent = CompositeText;
		Divergent.ReplaceInline(TEXT("\"name\": \"cycle_a\""), TEXT("\"name\": \"cycle_ax\""));
		Private::AnalyzerWriteText(DivergentCompositePath, Divergent);

		FMHSourceAnalysis Analysis;
		if (!Analyze(Ledger, Analysis))
		{
			return false;
		}
		bPassed &= TestEqual(
			TEXT("composite is BLOCKED"),
			ChangeOf(Analysis, Private::AnalyzerCompositeUid),
			static_cast<int32>(EMHSourceChange::Blocked));

		const FMHSourceAnalysisEntry* Composite = Analysis.Find(Private::AnalyzerCompositeUid);
		bPassed &= TestTrue(TEXT("blocked entry exists"), Composite != nullptr);
		if (Composite != nullptr)
		{
			bool bDivergent = false;
			for (const FString& Error : Composite->Errors)
			{
				bDivergent |= Error.StartsWith(TEXT("MH_E_DIVERGENT_REVISIONS"));
			}
			bPassed &= TestTrue(TEXT("divergent diagnostic"), bDivergent);
			bPassed &= TestFalse(TEXT("blocked entry never advances"), Composite->bLedgerAdvanceAllowed);
		}
		bPassed &= TestEqual(
			TEXT("the orphan material is still classified"),
			ChangeOf(Analysis, Private::AnalyzerMaterialUid),
			static_cast<int32>(EMHSourceChange::Remove));
	}

	FileManager.DeleteDirectory(*TempRoot, false, true);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMHLedgerSnapshotTest,
	"Mimir.C1.LedgerSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHLedgerSnapshotTest::RunTest(const FString& Parameters)
{
	TMap<FString, FMHLedgerRow> Rows;

	FMHLedgerRow Mesh;
	Mesh.Kind = EMHResourceKind::StaticMesh;
	Mesh.Asset = FSoftObjectPath(TEXT("/Game/MH/Meshes/SM_wall_a.SM_wall_a"));
	Mesh.SourcePath = TEXT("meshes/wall_a.mesh.fbx");
	Mesh.AppliedGeometryHash = TEXT("xxh3:9f2c01ab34cd56ef");
	Mesh.AppliedDescriptorHash = TEXT("xxh3:0123456789abcdef");
	Mesh.PayloadFingerprint = TEXT("xxh3:fedcba9876543210");
	Mesh.ImportedAt = FDateTime(2026, 8, 19, 12, 30, 15);
	Mesh.ImportStatus = TEXT("CREATE");
	Rows.Add(TEXT("2db5574c-3aca-43cc-9ab5-8242403e18cd"), Mesh);

	FMHLedgerRow Composite;
	Composite.Kind = EMHResourceKind::Composite;
	Composite.SourcePath = TEXT("a/cycle_a.composite");
	Composite.AppliedDescriptorHash = TEXT("xxh3:00112233445566aa");
	Composite.PayloadFingerprint = TEXT("xxh3:aa112233445566ff");
	Composite.ImportedAt = FDateTime(2026, 8, 19, 12, 31, 0);
	Composite.ImportStatus = TEXT("NO_CHANGE");
	Rows.Add(TEXT("b0ca6032-8884-534d-8ceb-20f4a4edb766"), Composite);

	FString Json;
	if (!TestTrue(TEXT("snapshot serializes"), MHLedgerSnapshotToJson(Rows, Json)))
	{
		return false;
	}

	TMap<FString, FMHLedgerRow> Restored;
	FString Error;
	if (!TestTrue(TEXT("snapshot parses"), MHLedgerSnapshotFromJson(Json, Restored, Error)))
	{
		AddError(Error);
		return false;
	}

	bool bPassed = TestEqual(TEXT("row count"), Restored.Num(), Rows.Num());
	for (const TPair<FString, FMHLedgerRow>& Pair : Rows)
	{
		const FMHLedgerRow* Row = Restored.Find(Pair.Key);
		if (Row == nullptr)
		{
			AddError(FString::Printf(TEXT("row %s is missing after the round trip"), *Pair.Key));
			bPassed = false;
			continue;
		}
		bPassed &= TestEqual(TEXT("kind"), static_cast<int32>(Row->Kind), static_cast<int32>(Pair.Value.Kind));
		bPassed &= TestEqual(TEXT("asset"), Row->Asset.ToString(), Pair.Value.Asset.ToString());
		bPassed &= TestEqual(TEXT("source path"), Row->SourcePath, Pair.Value.SourcePath);
		bPassed &= TestEqual(TEXT("geometry hash"), Row->AppliedGeometryHash, Pair.Value.AppliedGeometryHash);
		bPassed &= TestEqual(TEXT("descriptor hash"), Row->AppliedDescriptorHash, Pair.Value.AppliedDescriptorHash);
		bPassed &= TestEqual(TEXT("fingerprint"), Row->PayloadFingerprint, Pair.Value.PayloadFingerprint);
		bPassed &= TestEqual(TEXT("import status"), Row->ImportStatus, Pair.Value.ImportStatus);
		bPassed &= TestTrue(TEXT("imported at"), Row->ImportedAt == Pair.Value.ImportedAt);
	}

	FString BadError;
	TMap<FString, FMHLedgerRow> Rejected;
	bPassed &= TestFalse(
		TEXT("a foreign snapshot is rejected"),
		MHLedgerSnapshotFromJson(TEXT("{\"schema\":\"mh.composite\"}"), Rejected, BadError));

	return bPassed;
}

} // namespace UE::MimirComposite::Tests
