#include "Canonical/MHCanonical.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite::Tests
{
namespace Private
{

FString BytesToLowerHex(TConstArrayView<uint8> Bytes)
{
	static constexpr TCHAR Hex[] = TEXT("0123456789abcdef");
	FString Result;
	Result.Reserve(Bytes.Num() * 2);
	for (const uint8 Byte : Bytes)
	{
		Result.AppendChar(Hex[Byte >> 4]);
		Result.AppendChar(Hex[Byte & 0x0f]);
	}
	return Result;
}

bool JsonEquivalent(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
{
	if (!A.IsValid() || !B.IsValid() || A->Type != B->Type)
	{
		return false;
	}
	switch (A->Type)
	{
	case EJson::Null:
		return true;
	case EJson::String:
		return A->AsString() == B->AsString();
	case EJson::Boolean:
		return A->AsBool() == B->AsBool();
	case EJson::Number:
	{
		FString RawA;
		FString RawB;
		return A->TryGetString(RawA) && B->TryGetString(RawB) && RawA == RawB;
	}
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>& ArrayA = A->AsArray();
		const TArray<TSharedPtr<FJsonValue>>& ArrayB = B->AsArray();
		if (ArrayA.Num() != ArrayB.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < ArrayA.Num(); ++Index)
		{
			if (!JsonEquivalent(ArrayA[Index], ArrayB[Index]))
			{
				return false;
			}
		}
		return true;
	}
	case EJson::Object:
	{
		const TMap<FString, TSharedPtr<FJsonValue>>& ObjectA = A->AsObject()->Values;
		const TMap<FString, TSharedPtr<FJsonValue>>& ObjectB = B->AsObject()->Values;
		if (ObjectA.Num() != ObjectB.Num())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ObjectA)
		{
			const TSharedPtr<FJsonValue>* Other = ObjectB.Find(Pair.Key);
			if (Other == nullptr || !JsonEquivalent(Pair.Value, *Other))
			{
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

bool GetNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
{
	return Value.IsValid() && Value->Type == EJson::Number && Value->TryGetNumber(OutNumber);
}

bool GetInteger(const TSharedPtr<FJsonValue>& Value, int64& OutInteger)
{
	return Value.IsValid() && Value->Type == EJson::Number && Value->TryGetNumber(OutInteger);
}

bool VerifyCanonicalBytesAndHash(
	FAutomationTestBase& Test,
	const FString& VectorName,
	const TSharedPtr<FJsonValue>& Value,
	const TSharedPtr<FJsonObject>& Expected)
{
	TArray<uint8> Bytes;
	const FMHCanonicalResult Result = MHCanonicalJsonBytes(Value, Bytes);
	if (!Result.bSuccess)
	{
		Test.AddError(FString::Printf(TEXT("%s: canonical writer failed: %s"), *VectorName, *Result.Error));
		return false;
	}
	const FString ActualHex = BytesToLowerHex(Bytes);
	const FString ExpectedHex = Expected->GetStringField(TEXT("bytes_hex"));
	const FString ActualHash = MHXxh3Hash(Bytes);
	const FString ExpectedHash = Expected->GetStringField(TEXT("hash"));
	bool bPassed = true;
	if (ActualHex != ExpectedHex)
	{
		Test.AddError(FString::Printf(TEXT("%s: bytes mismatch\nexpected %s\nactual   %s"), *VectorName, *ExpectedHex, *ActualHex));
		bPassed = false;
	}
	if (ActualHash != ExpectedHash)
	{
		Test.AddError(FString::Printf(TEXT("%s: hash mismatch, expected %s, actual %s"), *VectorName, *ExpectedHash, *ActualHash));
		bPassed = false;
	}
	return bPassed;
}

bool VerifyObjectPairsBytesAndHash(
	FAutomationTestBase& Test,
	const FString& VectorName,
	TConstArrayView<TPair<FString, TSharedPtr<FJsonValue>>> Pairs,
	const TSharedPtr<FJsonObject>& Expected)
{
	TArray<uint8> Bytes;
	const FMHCanonicalResult Result = MHCanonicalJsonObjectPairsBytes(Pairs, Bytes);
	if (!Result.bSuccess)
	{
		Test.AddError(FString::Printf(TEXT("%s: canonical pair-object writer failed: %s"), *VectorName, *Result.Error));
		return false;
	}
	const FString ActualHex = BytesToLowerHex(Bytes);
	const FString ExpectedHex = Expected->GetStringField(TEXT("bytes_hex"));
	const FString ActualHash = MHXxh3Hash(Bytes);
	const FString ExpectedHash = Expected->GetStringField(TEXT("hash"));
	bool bPassed = true;
	if (ActualHex != ExpectedHex)
	{
		Test.AddError(FString::Printf(TEXT("%s: pair-object bytes mismatch\nexpected %s\nactual   %s"), *VectorName, *ExpectedHex, *ActualHex));
		bPassed = false;
	}
	if (ActualHash != ExpectedHash)
	{
		Test.AddError(FString::Printf(TEXT("%s: pair-object hash mismatch, expected %s, actual %s"), *VectorName, *ExpectedHash, *ActualHash));
		bPassed = false;
	}
	return bPassed;
}

bool RunPathContractTable(FAutomationTestBase& Test)
{
	struct FCase
	{
		const TCHAR* Authored;
		const TCHAR* Root;
		const TCHAR* Expected;
		bool bExternal;
	};
	// Frozen Source Schema v1 section 5.3. This table remains even after shared
	// path vectors are added to golden/canonical_vectors.json.
	static const FCase Cases[] = {
		{TEXT("textures\\wall_d.tif"), TEXT("c:\\Project\\Source"), TEXT("textures/wall_d.tif"), false},
		{TEXT("textures/../common/brick.tif"), TEXT("C:/Project/Source"), TEXT("common/brick.tif"), false},
		{TEXT("C:\\PROJECT\\SOURCE\\Textures\\A.TIF"), TEXT("C:/Project/Source"), TEXT("Textures/A.TIF"), false},
		{TEXT("C:/Project/Source/../Shared/A.tif"), TEXT("C:/Project/Source"), TEXT("C:/Project/Shared/A.tif"), true},
		{TEXT("..\\Shared\\A.tif"), TEXT("C:/Project/Source"), TEXT("C:/Project/Shared/A.tif"), true},
		{TEXT("d:\\Lib\\A.tif"), TEXT("C:/Project/Source"), TEXT("D:/Lib/A.tif"), true},
		{TEXT("\\\\Server\\Share\\Project\\tex\\a.tif"), TEXT("//server/share/project"), TEXT("tex/a.tif"), false},
		{TEXT("/opt/mh/source/textures/a.tif"), TEXT("/opt/mh/source"), TEXT("textures/a.tif"), false},
		{TEXT("../shared/a.tif"), TEXT("/opt/mh/source"), TEXT("/opt/mh/shared/a.tif"), true},
		{TEXT("/opt/mh/source2/a.tif"), TEXT("/opt/mh/source"), TEXT("/opt/mh/source2/a.tif"), true},
	};

	bool bPassed = true;
	for (const FCase& Case : Cases)
	{
		const FMHTexturePathResult Result = MHCanonicalizeTexturePath(Case.Authored, Case.Root);
		if (!Result.bSuccess || Result.Path != Case.Expected || Result.bExternal != Case.bExternal)
		{
			Test.AddError(FString::Printf(
				TEXT("path contract mismatch for '%s' under '%s': success=%d external=%d path='%s' error='%s'"),
				Case.Authored,
				Case.Root,
				Result.bSuccess,
				Result.bExternal,
				*Result.Path,
				*Result.Error));
			bPassed = false;
		}
	}

	const FMHTexturePathResult InternalDisk = MHValidateDiskTexturePath(TEXT("textures/a.tif"), TEXT("C:/Project/Source"));
	const FMHTexturePathResult ExternalDisk = MHValidateDiskTexturePath(TEXT("D:/Library/a.tif"), TEXT("C:/Project/Source"));
	const FMHTexturePathResult MalformedDisk = MHValidateDiskTexturePath(TEXT("textures/../a.tif"), TEXT("C:/Project/Source"));
	const FMHTexturePathResult AbsoluteInside = MHValidateDiskTexturePath(TEXT("C:/Project/Source/a.tif"), TEXT("C:/Project/Source"));
	if (!InternalDisk.bSuccess || InternalDisk.bExternal || !ExternalDisk.bSuccess || !ExternalDisk.bExternal ||
		MalformedDisk.bSuccess || AbsoluteInside.bSuccess)
	{
		Test.AddError(TEXT("on-disk path validation does not match frozen section 5.3"));
		bPassed = false;
	}
	return bPassed;
}

bool RunInt64QuantizationBoundary(FAutomationTestBase& Test)
{
	int64 Quantized = 0;
	const FMHCanonicalResult UpperOverflow = MHQuantize(9223372036854775808.0, 0, Quantized);
	if (UpperOverflow.bSuccess)
	{
		Test.AddError(TEXT("quantization must reject the exclusive int64 upper boundary"));
		return false;
	}

	const FMHCanonicalResult LowerBoundary = MHQuantize(-9223372036854775808.0, 0, Quantized);
	if (!LowerBoundary.bSuccess || Quantized != MIN_int64)
	{
		Test.AddError(TEXT("quantization must accept the inclusive int64 lower boundary"));
		return false;
	}
	return true;
}

} // namespace Private

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMHCanonicalVectorsTest,
	"Mimir.C0.CanonicalVectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCanonicalVectorsTest::RunTest(const FString& Parameters)
{
	FString GoldenRoot;
	if (!ResolveGoldenRoot(*this, GoldenRoot))
	{
		return false;
	}
	const FString VectorPath = FPaths::Combine(GoldenRoot, TEXT("canonical_vectors.json"));
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *VectorPath))
	{
		AddError(FString::Printf(TEXT("cannot read canonical vectors: %s"), *VectorPath));
		return false;
	}

	TSharedPtr<FJsonValue> RootValue;
	const FMHCanonicalResult ParseResult = MHParseJsonUtf8(FileBytes, RootValue);
	if (!ParseResult.bSuccess || !RootValue.IsValid() || RootValue->Type != EJson::Object)
	{
		AddError(FString::Printf(TEXT("cannot parse canonical vectors: %s"), *ParseResult.Error));
		return false;
	}
	const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
	if (Root->GetStringField(TEXT("schema")) != TEXT("mh.canonical_vectors"))
	{
		AddError(TEXT("canonical vector schema mismatch"));
		return false;
	}
	int64 SchemaVersion = 0;
	const TSharedPtr<FJsonValue>* SchemaValue = Root->Values.Find(TEXT("schema_version"));
	if (SchemaValue == nullptr || !Private::GetInteger(*SchemaValue, SchemaVersion) || SchemaVersion != 1)
	{
		AddError(TEXT("canonical vector schema_version must be integer 1"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Vectors = nullptr;
	if (!Root->TryGetArrayField(TEXT("vectors"), Vectors) || Vectors == nullptr || Vectors->Num() != 39)
	{
		AddError(TEXT("golden/canonical_vectors.json must contain exactly the frozen 39 C0 vectors"));
		return false;
	}

	bool bPassed = true;
	TSet<FString> Names;
	int32 Processed = 0;
	for (const TSharedPtr<FJsonValue>& VectorValue : *Vectors)
	{
		if (!VectorValue.IsValid() || VectorValue->Type != EJson::Object)
		{
			AddError(TEXT("canonical vector row must be an object"));
			bPassed = false;
			continue;
		}
		const TSharedPtr<FJsonObject> Vector = VectorValue->AsObject();
		const FString Name = Vector->GetStringField(TEXT("name"));
		const FString Kind = Vector->GetStringField(TEXT("kind"));
		if (Names.Contains(Name))
		{
			AddError(FString::Printf(TEXT("duplicate canonical vector name: %s"), *Name));
			bPassed = false;
			continue;
		}
		Names.Add(Name);
		const TSharedPtr<FJsonObject> Input = Vector->GetObjectField(TEXT("input"));
		const TSharedPtr<FJsonObject> Expected = Vector->GetObjectField(TEXT("expected"));

		if (Kind == TEXT("quantize"))
		{
			double Value = 0.0;
			int64 Precision = 0;
			int64 ExpectedQ = 0;
			const TSharedPtr<FJsonValue>* ValueField = Input->Values.Find(TEXT("value"));
			const TSharedPtr<FJsonValue>* PrecisionField = Input->Values.Find(TEXT("p"));
			const TSharedPtr<FJsonValue>* ExpectedField = Expected->Values.Find(TEXT("q"));
			if (ValueField == nullptr || PrecisionField == nullptr || ExpectedField == nullptr ||
				!Private::GetNumber(*ValueField, Value) || !Private::GetInteger(*PrecisionField, Precision) ||
				!Private::GetInteger(*ExpectedField, ExpectedQ))
			{
				AddError(FString::Printf(TEXT("%s: malformed quantize vector"), *Name));
				bPassed = false;
				continue;
			}
			int64 ActualQ = 0;
			const FMHCanonicalResult Result = MHQuantize(Value, static_cast<int32>(Precision), ActualQ);
			if (!Result.bSuccess || ActualQ != ExpectedQ)
			{
				AddError(FString::Printf(TEXT("%s: quantization mismatch, expected %lld, actual %lld (%s)"), *Name, ExpectedQ, ActualQ, *Result.Error));
				bPassed = false;
			}
		}
		else if (Kind == TEXT("quat"))
		{
			const TArray<TSharedPtr<FJsonValue>>* InputItems = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* ExpectedItems = nullptr;
			if (!Input->TryGetArrayField(TEXT("q"), InputItems) || !Expected->TryGetArrayField(TEXT("quat"), ExpectedItems) ||
				InputItems == nullptr || ExpectedItems == nullptr)
			{
				AddError(FString::Printf(TEXT("%s: malformed quaternion vector"), *Name));
				bPassed = false;
				continue;
			}
			TArray<double> InputQuaternion;
			TArray<int64> ExpectedQuaternion;
			for (const TSharedPtr<FJsonValue>& Item : *InputItems)
			{
				double Number = 0.0;
				if (!Private::GetNumber(Item, Number))
				{
					bPassed = false;
				}
				InputQuaternion.Add(Number);
			}
			for (const TSharedPtr<FJsonValue>& Item : *ExpectedItems)
			{
				int64 Number = 0;
				if (!Private::GetInteger(Item, Number))
				{
					bPassed = false;
				}
				ExpectedQuaternion.Add(Number);
			}
			TArray<int64> ActualQuaternion;
			const FMHCanonicalResult Result = MHCanonicalizeQuaternion(InputQuaternion, ActualQuaternion);
			if (!Result.bSuccess || ActualQuaternion != ExpectedQuaternion)
			{
				AddError(FString::Printf(TEXT("%s: quaternion mismatch: %s"), *Name, *Result.Error));
				bPassed = false;
			}
		}
		else if (Kind == TEXT("json"))
		{
			TSharedPtr<FJsonValue> JsonInput;
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> ObjectPairs;
			if (const TSharedPtr<FJsonValue>* ValueField = Input->Values.Find(TEXT("value")))
			{
				JsonInput = *ValueField;
			}
			else
			{
				const TArray<TSharedPtr<FJsonValue>>* Pairs = nullptr;
				if (!Input->TryGetArrayField(TEXT("pairs"), Pairs) || Pairs == nullptr)
				{
					AddError(FString::Printf(TEXT("%s: malformed JSON vector"), *Name));
					bPassed = false;
					continue;
				}
				for (const TSharedPtr<FJsonValue>& PairValue : *Pairs)
				{
					const TArray<TSharedPtr<FJsonValue>>& Pair = PairValue->AsArray();
					ObjectPairs.Emplace(Pair[0]->AsString(), Pair[1]);
				}
			}
			if (JsonInput.IsValid())
			{
				bPassed &= Private::VerifyCanonicalBytesAndHash(*this, Name, JsonInput, Expected);
			}
			else
			{
				bPassed &= Private::VerifyObjectPairsBytesAndHash(*this, Name, ObjectPairs, Expected);
			}
		}
		else if (Kind == TEXT("composite"))
		{
			TSharedPtr<FJsonValue> Canonical;
			const FMHCanonicalResult Result = MHCompositeCanonicalForm(Input->Values.FindChecked(TEXT("doc")), Canonical);
			if (!Result.bSuccess)
			{
				AddError(FString::Printf(TEXT("%s: composite canonicalization failed: %s"), *Name, *Result.Error));
				bPassed = false;
			}
			else
			{
				TArray<uint8> ExpectedCanonicalBytes;
				const FMHCanonicalResult ExpectedResult = MHCanonicalJsonBytes(Expected->Values.FindChecked(TEXT("canonical")), ExpectedCanonicalBytes);
				TArray<uint8> ActualCanonicalBytes;
				const FMHCanonicalResult ActualResult = MHCanonicalJsonBytes(Canonical, ActualCanonicalBytes);
				if (!ExpectedResult.bSuccess || !ActualResult.bSuccess || ExpectedCanonicalBytes != ActualCanonicalBytes)
				{
					AddError(FString::Printf(TEXT("%s: canonical value tree mismatch"), *Name));
					bPassed = false;
				}
				bPassed &= Private::VerifyCanonicalBytesAndHash(*this, Name, Canonical, Expected);
			}
		}
		else if (Kind == TEXT("material"))
		{
			TSharedPtr<FJsonValue> Disk;
			TSharedPtr<FJsonValue> Canonical;
			const FMHCanonicalResult Result = MHMaterialForms(
				Input->GetStringField(TEXT("shader_class")),
				Input->Values.FindChecked(TEXT("params")),
				Input->Values.FindChecked(TEXT("textures")),
				Disk,
				Canonical);
			if (!Result.bSuccess)
			{
				AddError(FString::Printf(TEXT("%s: material canonicalization failed: %s"), *Name, *Result.Error));
				bPassed = false;
			}
			else
			{
				if (!Private::JsonEquivalent(Disk, Expected->Values.FindChecked(TEXT("disk"))))
				{
					AddError(FString::Printf(TEXT("%s: normalized disk material payload mismatch"), *Name));
					bPassed = false;
				}
				TArray<uint8> ExpectedCanonicalBytes;
				const FMHCanonicalResult ExpectedResult = MHCanonicalJsonBytes(Expected->Values.FindChecked(TEXT("canonical")), ExpectedCanonicalBytes);
				TArray<uint8> ActualCanonicalBytes;
				const FMHCanonicalResult ActualResult = MHCanonicalJsonBytes(Canonical, ActualCanonicalBytes);
				if (!ExpectedResult.bSuccess || !ActualResult.bSuccess || ExpectedCanonicalBytes != ActualCanonicalBytes)
				{
					AddError(FString::Printf(TEXT("%s: canonical material value tree mismatch"), *Name));
					bPassed = false;
				}
				bPassed &= Private::VerifyCanonicalBytesAndHash(*this, Name, Canonical, Expected);
			}
		}
		else
		{
			AddError(FString::Printf(TEXT("%s: unsupported canonical vector kind '%s'"), *Name, *Kind));
			bPassed = false;
		}
		++Processed;
	}

	if (Processed != Vectors->Num())
	{
		AddError(FString::Printf(TEXT("processed %d of %d canonical vectors"), Processed, Vectors->Num()));
		bPassed = false;
	}
	bPassed &= Private::RunPathContractTable(*this);
	bPassed &= Private::RunInt64QuantizationBoundary(*this);
	return bPassed;
}

} // namespace UE::MimirComposite::Tests
