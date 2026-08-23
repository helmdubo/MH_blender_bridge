#include "Source/MHPayloadScanResolver.h"

#include "HAL/FileManager.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite::Tests
{
namespace Private
{

const TCHAR* CycleAUid = TEXT("b0ca6032-8884-534d-8ceb-20f4a4edb766");
const TCHAR* MaterialUid = TEXT("7d995e54-d084-4466-a613-a1cd8f3248b2");

FString MakeTempRoot()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("MimirCompositeTests"),
		FString::Printf(TEXT("Resolver_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

bool WriteText(const FString& Path, const FString& Text)
{
	return FFileHelper::SaveStringToFile(
		Text,
		*Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString MaterialJson(const FString& Name)
{
	return FString::Printf(TEXT(R"({
  "schema": "mh.material",
  "schema_version": 1,
  "uid": "%s",
  "name": "%s",
  "shader_class": "rendinst_simple",
  "params": {},
  "textures": {}
})"), MaterialUid, *Name);
}

} // namespace Private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMHPayloadResolverTest,
	"Mimir.C1.PayloadResolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPayloadResolverTest::RunTest(const FString& Parameters)
{
	FString GoldenRoot;
	if (!ResolveGoldenRoot(*this, GoldenRoot))
	{
		return false;
	}

	FString CycleAText;
	const FString CycleAPath =
		FPaths::Combine(GoldenRoot, TEXT("fixtures/composite_cycle/cycle_a.composite"));
	if (!FFileHelper::LoadFileToString(CycleAText, *CycleAPath))
	{
		AddError(FString::Printf(TEXT("cannot read fixture %s"), *CycleAPath));
		return false;
	}

	const FString TempRoot = Private::MakeTempRoot();
	IFileManager& FileManager = IFileManager::Get();
	bool bPassed = true;

	// Single valid candidate resolves; the wrong expected kind does not.
	{
		Private::WriteText(FPaths::Combine(TempRoot, TEXT("a/cycle_a.composite")), CycleAText);
		FMHPayloadScanResolver Resolver(TempRoot);
		FString Error;
		if (!TestTrue(TEXT("scan initializes"), Resolver.Initialize(Error)))
		{
			AddError(Error);
			return false;
		}
		TestEqual(TEXT("one candidate file"), Resolver.GetCandidateFileCount(), 1);

		FMHResolveOutcome Outcome = Resolver.Resolve(Private::CycleAUid, EMHResourceKind::Composite);
		bPassed &= TestEqual(
			TEXT("composite resolves"),
			static_cast<int32>(Outcome.Status),
			static_cast<int32>(EMHResolveStatus::Resolved));
		bPassed &= TestEqual(TEXT("resolved name"), Outcome.Name, TEXT("cycle_a"));
		bPassed &= TestTrue(
			TEXT("snapshot carries semantic hash"),
			Outcome.DescriptorHash.StartsWith(TEXT("xxh3:")));
		bPassed &= TestTrue(
			TEXT("snapshot carries exact byte fingerprint"),
			Outcome.Fingerprint.StartsWith(TEXT("sha256:")));
		bPassed &= TestTrue(
			TEXT("composite snapshot has no geometry hash"),
			Outcome.GeometryHash.IsEmpty());

		Outcome = Resolver.Resolve(Private::CycleAUid, EMHResourceKind::StaticMesh);
		bPassed &= TestEqual(
			TEXT("kind mismatch is honest"),
			static_cast<int32>(Outcome.Status),
			static_cast<int32>(EMHResolveStatus::KindMismatch));

		Outcome = Resolver.Resolve(TEXT("00000000-0000-0000-0000-000000000000"), EMHResourceKind::Composite);
		bPassed &= TestEqual(
			TEXT("unknown uid unresolved"),
			static_cast<int32>(Outcome.Status),
			static_cast<int32>(EMHResolveStatus::Unresolved));
		bPassed &= TestTrue(
			TEXT("unresolved diagnostic"),
			Outcome.Diagnostic.StartsWith(TEXT("MH_E_RESOURCE_NOT_FOUND")));
	}

	// Byte-identical duplicate stays one semantic resource with a warning.
	{
		Private::WriteText(FPaths::Combine(TempRoot, TEXT("b/cycle_a.composite")), CycleAText);
		FMHPayloadScanResolver Resolver(TempRoot);
		FString Error;
		Resolver.Initialize(Error);
		const FMHResolveOutcome Outcome = Resolver.Resolve(Private::CycleAUid, EMHResourceKind::Composite);
		bPassed &= TestEqual(
			TEXT("duplicate identical resolves"),
			static_cast<int32>(Outcome.Status),
			static_cast<int32>(EMHResolveStatus::Resolved));
		bPassed &= TestTrue(
			TEXT("duplicate warning"),
			Outcome.Diagnostic.StartsWith(TEXT("MH_W_DUPLICATE_IDENTICAL_PAYLOAD")));
		bPassed &= TestEqual(TEXT("duplicate candidate count"), Outcome.CandidatePaths.Num(), 2);
	}

	// Divergent bytes of the same UID block resolve.
	{
		FString Divergent = CycleAText;
		Divergent.ReplaceInline(TEXT("\"name\": \"cycle_a\""), TEXT("\"name\": \"cycle_ax\""));
		Private::WriteText(FPaths::Combine(TempRoot, TEXT("c/cycle_a.composite")), Divergent);
		FMHPayloadScanResolver Resolver(TempRoot);
		FString Error;
		Resolver.Initialize(Error);
		const FMHResolveOutcome Outcome = Resolver.Resolve(Private::CycleAUid, EMHResourceKind::Composite);
		bPassed &= TestEqual(
			TEXT("divergent revisions block"),
			static_cast<int32>(Outcome.Status),
			static_cast<int32>(EMHResolveStatus::DivergentRevisions));
		bPassed &= TestTrue(
			TEXT("divergent diagnostic"),
			Outcome.Diagnostic.StartsWith(TEXT("MH_E_DIVERGENT_REVISIONS")));
		FileManager.Delete(*FPaths::Combine(TempRoot, TEXT("b/cycle_a.composite")));
		FileManager.Delete(*FPaths::Combine(TempRoot, TEXT("c/cycle_a.composite")));
	}

	// Identity is global: one UID declared by different resource kinds blocks
	// even when the caller asks for the otherwise valid composite candidate.
	{
		FString CrossKindMaterial = Private::MaterialJson(TEXT("cross_kind"));
		CrossKindMaterial.ReplaceInline(Private::MaterialUid, Private::CycleAUid);
		const FString CrossKindPath =
			FPaths::Combine(TempRoot, TEXT("materials/cross_kind.material"));
		Private::WriteText(CrossKindPath, CrossKindMaterial);
		FMHPayloadScanResolver Resolver(TempRoot);
		FString Error;
		Resolver.Initialize(Error);
		const FMHResolveOutcome Outcome =
			Resolver.Resolve(Private::CycleAUid, EMHResourceKind::Composite);
		bPassed &= TestEqual(
			TEXT("same UID across kinds blocks globally"),
			static_cast<int32>(Outcome.Status),
			static_cast<int32>(EMHResolveStatus::DivergentRevisions));
		bPassed &= TestEqual(TEXT("cross-kind candidates are both reported"), Outcome.CandidatePaths.Num(), 2);
		FileManager.Delete(*CrossKindPath);
	}

	// Material self-identity resolves; malformed payloads quarantine per file.
	{
		Private::WriteText(
			FPaths::Combine(TempRoot, TEXT("materials/m_test.material")),
			Private::MaterialJson(TEXT("m_test")));
		Private::WriteText(
			FPaths::Combine(TempRoot, TEXT("materials/broken.material")),
			TEXT("{\"schema\": \"mh.material\""));
		FMHPayloadScanResolver Resolver(TempRoot);
		FString Error;
		Resolver.Initialize(Error);
		const FMHResolveOutcome Outcome = Resolver.Resolve(Private::MaterialUid, EMHResourceKind::Material);
		bPassed &= TestEqual(
			TEXT("material resolves"),
			static_cast<int32>(Outcome.Status),
			static_cast<int32>(EMHResolveStatus::Resolved));
		bPassed &= TestEqual(TEXT("one quarantined payload"), Resolver.GetQuarantined().Num(), 1);
		bPassed &= TestTrue(
			TEXT("quarantine names the material error"),
			Resolver.GetQuarantined().Num() == 1 &&
				Resolver.GetQuarantined()[0].Contains(TEXT("MH_E_INVALID_MATERIAL_VALUE")));
		const FMHSourceSnapshot Snapshot = Resolver.GetSnapshot();
		bPassed &= TestEqual(TEXT("quarantine is a scan error"), Snapshot.Errors.Num(), 1);
		bPassed &= TestEqual(TEXT("quarantine is structured"), Snapshot.Quarantined.Num(), 1);
		bPassed &= TestFalse(
			TEXT("quarantine is never downgraded to warning"),
			Snapshot.Warnings.ContainsByPredicate([](const FString& Warning)
			{
				return Warning.Contains(TEXT("quarantined"));
			}));
		if (Snapshot.Quarantined.Num() == 1)
		{
			bPassed &= TestTrue(
				TEXT("structured quarantine has absolute normalized path"),
				FPaths::IsRelative(Snapshot.Quarantined[0].PayloadPath) == false &&
				Snapshot.Quarantined[0].PayloadPath.EndsWith(TEXT("broken.material")));
			bPassed &= TestTrue(
				TEXT("structured quarantine preserves blocking diagnostic"),
				Snapshot.Quarantined[0].Diagnostic.Contains(TEXT("MH_E_INVALID_MATERIAL_VALUE")));
		}
	}

	// FBX without the mandatory Carrier B passport is quarantined.
	{
		TArray<uint8> AxisProbe;
		const FString AxisProbePath = FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"));
		if (FFileHelper::LoadFileToArray(AxisProbe, *AxisProbePath))
		{
			const FString MeshPath = FPaths::Combine(TempRoot, TEXT("meshes/axis_probe.mesh.fbx"));
			FFileHelper::SaveArrayToFile(AxisProbe, *MeshPath);
			FMHPayloadScanResolver Resolver(TempRoot);
			FString Error;
			Resolver.Initialize(Error);
			bool bQuarantinedPassport = false;
			for (const FString& Entry : Resolver.GetQuarantined())
			{
				bQuarantinedPassport |=
					Entry.Contains(TEXT("axis_probe.mesh.fbx")) &&
					Entry.Contains(TEXT("MH_E_PASSPORT_INVALID"));
			}
			bPassed &= TestTrue(TEXT("passport-less FBX quarantined"), bQuarantinedPassport);
		}
		else
		{
			AddError(FString::Printf(TEXT("cannot read fixture %s"), *AxisProbePath));
			bPassed = false;
		}
	}

	FileManager.DeleteDirectory(*TempRoot, false, true);
	return bPassed;
}

} // namespace UE::MimirComposite::Tests
