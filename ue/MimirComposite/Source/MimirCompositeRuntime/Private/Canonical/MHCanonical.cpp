#include "Canonical/MHCanonical.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Hash/xxhash.h"
#include "Math/UnrealMathUtility.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#ifndef UE_ENABLE_ICU
#define UE_ENABLE_ICU 0
#endif

#if UE_ENABLE_ICU
THIRD_PARTY_INCLUDES_START
#include <unicode/unorm2.h>
THIRD_PARTY_INCLUDES_END
#endif

namespace UE::MimirComposite
{
namespace Private
{

constexpr int32 DefaultPrecision = 6;
constexpr int32 RotationPrecision = 6;
constexpr int32 TranslationPrecision = 3;
constexpr int32 ScalePrecision = 6;
constexpr int32 WeightPrecision = 4;

const FString* FindExactSourceKey(
	const TArray<TPair<FString, FString>>& SourceKeys,
	const FString& NormalizedKey)
{
	for (const TPair<FString, FString>& Pair : SourceKeys)
	{
		if (Pair.Key.Equals(NormalizedKey, ESearchCase::CaseSensitive))
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonValue> MakeInteger(const int64 Value)
{
	return MakeShared<FJsonValueNumberString>(LexToString(Value));
}

bool ParseNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber, FString& OutRaw)
{
	if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetString(OutRaw))
	{
		return false;
	}
	return LexTryParseString(OutNumber, *OutRaw) && FMath::IsFinite(OutNumber);
}

bool IsPlainInteger(const FString& Raw)
{
	if (Raw == TEXT("-0"))
	{
		return false;
	}
	int32 Index = 0;
	if (Raw.StartsWith(TEXT("-")))
	{
		Index = 1;
	}
	if (Index >= Raw.Len())
	{
		return false;
	}
	if (Raw[Index] == TEXT('0'))
	{
		return Index + 1 == Raw.Len();
	}
	if (Raw[Index] < TEXT('1') || Raw[Index] > TEXT('9'))
	{
		return false;
	}
	for (++Index; Index < Raw.Len(); ++Index)
	{
		if (Raw[Index] < TEXT('0') || Raw[Index] > TEXT('9'))
		{
			return false;
		}
	}
	return true;
}

FMHCanonicalResult CloneNfc(const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& OutValue);

FMHCanonicalResult NormalizeObject(
	const TSharedPtr<FJsonObject>& Object,
	TSharedPtr<FJsonObject>& OutObject,
	TFunctionRef<FMHCanonicalResult(const FString&, const TSharedPtr<FJsonValue>&, TSharedPtr<FJsonValue>&)> Transform)
{
	if (!Object.IsValid())
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_CANONICAL_JSON: expected object"));
	}

	OutObject = MakeShared<FJsonObject>();
	TArray<TPair<FString, FString>> SourceKeys;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		FString NormalizedKey;
		FMHCanonicalResult Result = MHNormalizeNfc(Pair.Key, NormalizedKey);
		if (!Result.bSuccess)
		{
			return Result;
		}
		if (const FString* Existing = FindExactSourceKey(SourceKeys, NormalizedKey))
		{
			return FMHCanonicalResult::Failure(FString::Printf(
				TEXT("MH_E_INVALID_CANONICAL_JSON: keys collide after NFC normalization: %s and %s"),
				**Existing,
				*Pair.Key));
		}
		SourceKeys.Emplace(NormalizedKey, Pair.Key);

		TSharedPtr<FJsonValue> Transformed;
		Result = Transform(NormalizedKey, Pair.Value, Transformed);
		if (!Result.bSuccess)
		{
			return Result;
		}
		OutObject->SetField(NormalizedKey, Transformed);
	}
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult CanonGeneric(
	const TSharedPtr<FJsonValue>& Value,
	const int32 Precision,
	TSharedPtr<FJsonValue>& OutValue)
{
	if (!Value.IsValid())
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_CANONICAL_JSON: invalid value"));
	}

	switch (Value->Type)
	{
	case EJson::Null:
		OutValue = MakeShared<FJsonValueNull>();
		return FMHCanonicalResult::Success();
	case EJson::Boolean:
		OutValue = MakeShared<FJsonValueBoolean>(Value->AsBool());
		return FMHCanonicalResult::Success();
	case EJson::String:
	{
		FString Normalized;
		FMHCanonicalResult Result = MHNormalizeNfc(Value->AsString(), Normalized);
		if (Result.bSuccess)
		{
			OutValue = MakeShared<FJsonValueString>(MoveTemp(Normalized));
		}
		return Result;
	}
	case EJson::Number:
	{
		double Number = 0.0;
		FString Raw;
		if (!ParseNumber(Value, Number, Raw))
		{
			return FMHCanonicalResult::Failure(TEXT("MH_E_NAN_INF_VALUE: invalid JSON number"));
		}
		int64 Quantized = 0;
		FMHCanonicalResult Result = MHQuantize(Number, Precision, Quantized);
		if (Result.bSuccess)
		{
			OutValue = MakeInteger(Quantized);
		}
		return Result;
	}
	case EJson::Array:
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			TSharedPtr<FJsonValue> CanonicalItem;
			FMHCanonicalResult Result = CanonGeneric(Item, Precision, CanonicalItem);
			if (!Result.bSuccess)
			{
				return Result;
			}
			Items.Add(CanonicalItem);
		}
		OutValue = MakeShared<FJsonValueArray>(MoveTemp(Items));
		return FMHCanonicalResult::Success();
	}
	case EJson::Object:
	{
		TSharedPtr<FJsonObject> OutObject;
		FMHCanonicalResult Result = NormalizeObject(
			Value->AsObject(),
			OutObject,
			[Precision](const FString&, const TSharedPtr<FJsonValue>& SubValue, TSharedPtr<FJsonValue>& OutSubValue)
			{
				return CanonGeneric(SubValue, Precision, OutSubValue);
			});
		if (Result.bSuccess)
		{
			OutValue = MakeShared<FJsonValueObject>(OutObject);
		}
		return Result;
	}
	default:
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_CANONICAL_JSON: unsupported JSON type"));
	}
}

FMHCanonicalResult CanonPassthroughInteger(
	const TSharedPtr<FJsonValue>& Value,
	TSharedPtr<FJsonValue>& OutValue)
{
	if (Value.IsValid() && Value->Type == EJson::Number)
	{
		FString Raw;
		double Number = 0.0;
		if (ParseNumber(Value, Number, Raw) && IsPlainInteger(Raw))
		{
			int64 Integer = 0;
			if (LexTryParseString(Integer, *Raw))
			{
				OutValue = MakeInteger(Integer);
				return FMHCanonicalResult::Success();
			}
		}
	}
	return CanonGeneric(Value, DefaultPrecision, OutValue);
}

FMHCanonicalResult CanonVector(
	const TSharedPtr<FJsonValue>& Value,
	const int32 Precision,
	const int32 ExpectedLength,
	TSharedPtr<FJsonValue>& OutValue)
{
	if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != ExpectedLength)
	{
		return CanonGeneric(Value, Precision, OutValue);
	}
	for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
	{
		if (!Item.IsValid() || Item->Type != EJson::Number)
		{
			return CanonGeneric(Value, Precision, OutValue);
		}
	}
	return CanonGeneric(Value, Precision, OutValue);
}

void CanonicalizeQuaternionSign(TArray<int64>& Quaternion)
{
	bool bNegate = Quaternion[3] < 0;
	if (Quaternion[3] == 0)
	{
		for (int32 Index = 0; Index < 3; ++Index)
		{
			if (Quaternion[Index] != 0)
			{
				bNegate = Quaternion[Index] < 0;
				break;
			}
		}
	}
	if (bNegate)
	{
		for (int64& Component : Quaternion)
		{
			Component = -Component;
		}
	}
}

FMHCanonicalResult CanonRotation(
	const TSharedPtr<FJsonValue>& Value,
	TSharedPtr<FJsonValue>& OutValue)
{
	if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 4)
	{
		return CanonGeneric(Value, RotationPrecision, OutValue);
	}

	TArray<int64> Quantized;
	for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
	{
		double Number = 0.0;
		FString Raw;
		if (!ParseNumber(Item, Number, Raw))
		{
			return CanonGeneric(Value, RotationPrecision, OutValue);
		}
		int64 Component = 0;
		FMHCanonicalResult Result = MHQuantize(Number, RotationPrecision, Component);
		if (!Result.bSuccess)
		{
			return Result;
		}
		Quantized.Add(Component);
	}
	CanonicalizeQuaternionSign(Quantized);
	TArray<TSharedPtr<FJsonValue>> Components;
	for (const int64 Component : Quantized)
	{
		Components.Add(MakeInteger(Component));
	}
	OutValue = MakeShared<FJsonValueArray>(MoveTemp(Components));
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult CanonLocalTransform(
	const TSharedPtr<FJsonValue>& Value,
	TSharedPtr<FJsonValue>& OutValue)
{
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return CanonGeneric(Value, DefaultPrecision, OutValue);
	}
	TSharedPtr<FJsonObject> Object;
	FMHCanonicalResult Result = NormalizeObject(
		Value->AsObject(),
		Object,
		[](const FString& Key, const TSharedPtr<FJsonValue>& SubValue, TSharedPtr<FJsonValue>& OutSubValue)
		{
			if (Key == TEXT("translation_cm"))
			{
				return CanonVector(SubValue, TranslationPrecision, 3, OutSubValue);
			}
			if (Key == TEXT("rotation_quat"))
			{
				return CanonRotation(SubValue, OutSubValue);
			}
			if (Key == TEXT("scale"))
			{
				return CanonVector(SubValue, ScalePrecision, 3, OutSubValue);
			}
			return CanonGeneric(SubValue, DefaultPrecision, OutSubValue);
		});
	if (Result.bSuccess)
	{
		OutValue = MakeShared<FJsonValueObject>(Object);
	}
	return Result;
}

FMHCanonicalResult CanonVariant(
	const TSharedPtr<FJsonValue>& Value,
	TSharedPtr<FJsonValue>& OutValue)
{
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return CanonGeneric(Value, DefaultPrecision, OutValue);
	}
	TSharedPtr<FJsonObject> Object;
	FMHCanonicalResult Result = NormalizeObject(
		Value->AsObject(),
		Object,
		[](const FString& Key, const TSharedPtr<FJsonValue>& SubValue, TSharedPtr<FJsonValue>& OutSubValue)
		{
			return CanonGeneric(SubValue, Key == TEXT("weight") ? WeightPrecision : DefaultPrecision, OutSubValue);
		});
	if (Result.bSuccess)
	{
		OutValue = MakeShared<FJsonValueObject>(Object);
	}
	return Result;
}

FMHCanonicalResult CanonNode(
	const TSharedPtr<FJsonValue>& Value,
	TSharedPtr<FJsonValue>& OutValue)
{
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return CanonGeneric(Value, DefaultPrecision, OutValue);
	}
	TSharedPtr<FJsonObject> Object;
	FMHCanonicalResult Result = NormalizeObject(
		Value->AsObject(),
		Object,
		[](const FString& Key, const TSharedPtr<FJsonValue>& SubValue, TSharedPtr<FJsonValue>& OutSubValue)
		{
			if (Key == TEXT("local_transform"))
			{
				return CanonLocalTransform(SubValue, OutSubValue);
			}
			if (Key == TEXT("properties"))
			{
				return CanonGeneric(SubValue, DefaultPrecision, OutSubValue);
			}
			if (Key == TEXT("variants") && SubValue.IsValid() && SubValue->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Variants;
				for (const TSharedPtr<FJsonValue>& Variant : SubValue->AsArray())
				{
					TSharedPtr<FJsonValue> CanonicalVariant;
					FMHCanonicalResult VariantResult = CanonVariant(Variant, CanonicalVariant);
					if (!VariantResult.bSuccess)
					{
						return VariantResult;
					}
					Variants.Add(CanonicalVariant);
				}
				OutSubValue = MakeShared<FJsonValueArray>(MoveTemp(Variants));
				return FMHCanonicalResult::Success();
			}
			if (Key == TEXT("schema_version") || Key == TEXT("seed_salt"))
			{
				return CanonPassthroughInteger(SubValue, OutSubValue);
			}
			return CanonGeneric(SubValue, DefaultPrecision, OutSubValue);
		});
	if (Result.bSuccess)
	{
		OutValue = MakeShared<FJsonValueObject>(Object);
	}
	return Result;
}

FMHCanonicalResult CloneNfc(const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& OutValue)
{
	if (!Value.IsValid())
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_CANONICAL_JSON: invalid value"));
	}
	if (Value->Type == EJson::String)
	{
		FString Normalized;
		FMHCanonicalResult Result = MHNormalizeNfc(Value->AsString(), Normalized);
		if (Result.bSuccess)
		{
			OutValue = MakeShared<FJsonValueString>(MoveTemp(Normalized));
		}
		return Result;
	}
	if (Value->Type == EJson::Object)
	{
		TSharedPtr<FJsonObject> Object;
		FMHCanonicalResult Result = NormalizeObject(
			Value->AsObject(),
			Object,
			[](const FString&, const TSharedPtr<FJsonValue>& SubValue, TSharedPtr<FJsonValue>& OutSubValue)
			{
				return CloneNfc(SubValue, OutSubValue);
			});
		if (Result.bSuccess)
		{
			OutValue = MakeShared<FJsonValueObject>(Object);
		}
		return Result;
	}
	if (Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			TSharedPtr<FJsonValue> Normalized;
			FMHCanonicalResult Result = CloneNfc(Item, Normalized);
			if (!Result.bSuccess)
			{
				return Result;
			}
			Items.Add(Normalized);
		}
		OutValue = MakeShared<FJsonValueArray>(MoveTemp(Items));
		return FMHCanonicalResult::Success();
	}
	OutValue = Value;
	return FMHCanonicalResult::Success();
}

int32 CompareBytes(const TArray<uint8>& A, const TArray<uint8>& B)
{
	const int32 Common = FMath::Min(A.Num(), B.Num());
	const int32 Compared = Common > 0 ? FMemory::Memcmp(A.GetData(), B.GetData(), Common) : 0;
	if (Compared != 0)
	{
		return Compared;
	}
	return A.Num() - B.Num();
}

void AppendAnsi(TArray<uint8>& Out, const ANSICHAR* Text)
{
	const int32 Length = FCStringAnsi::Strlen(Text);
	Out.Append(reinterpret_cast<const uint8*>(Text), Length);
}

FMHCanonicalResult AppendString(const FString& Input, TArray<uint8>& Out)
{
	FString Normalized;
	FMHCanonicalResult Result = MHNormalizeNfc(Input, Normalized);
	if (!Result.bSuccess)
	{
		return Result;
	}
	FTCHARToUTF8 Utf8(*Normalized, Normalized.Len());
	Out.Add(static_cast<uint8>('"'));
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
		if (Byte == static_cast<uint8>('"'))
		{
			AppendAnsi(Out, "\\\"");
		}
		else if (Byte == static_cast<uint8>('\\'))
		{
			AppendAnsi(Out, "\\\\");
		}
		else if (Byte < 0x20)
		{
			static constexpr ANSICHAR Hex[] = "0123456789abcdef";
			Out.Append({static_cast<uint8>('\\'), static_cast<uint8>('u'), static_cast<uint8>('0'), static_cast<uint8>('0'),
				static_cast<uint8>(Hex[Byte >> 4]), static_cast<uint8>(Hex[Byte & 0x0f])});
		}
		else
		{
			Out.Add(Byte);
		}
	}
	Out.Add(static_cast<uint8>('"'));
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult AppendCanonical(const TSharedPtr<FJsonValue>& Value, TArray<uint8>& Out)
{
	if (!Value.IsValid())
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_CANONICAL_JSON: invalid value"));
	}
	switch (Value->Type)
	{
	case EJson::Null:
		AppendAnsi(Out, "null");
		return FMHCanonicalResult::Success();
	case EJson::Boolean:
		AppendAnsi(Out, Value->AsBool() ? "true" : "false");
		return FMHCanonicalResult::Success();
	case EJson::String:
		return AppendString(Value->AsString(), Out);
	case EJson::Number:
	{
		FString Raw;
		if (!Value->TryGetString(Raw) || !IsPlainInteger(Raw))
		{
			return FMHCanonicalResult::Failure(
				TEXT("MH_E_INVALID_CANONICAL_JSON: canonical numbers must be quantized integers"));
		}
		FTCHARToUTF8 Utf8(*Raw, Raw.Len());
		Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return FMHCanonicalResult::Success();
	}
	case EJson::Array:
	{
		Out.Add(static_cast<uint8>('['));
		bool bFirst = true;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			if (!bFirst)
			{
				Out.Add(static_cast<uint8>(','));
			}
			bFirst = false;
			FMHCanonicalResult Result = AppendCanonical(Item, Out);
			if (!Result.bSuccess)
			{
				return Result;
			}
		}
		Out.Add(static_cast<uint8>(']'));
		return FMHCanonicalResult::Success();
	}
	case EJson::Object:
	{
		struct FEntry
		{
			FString Key;
			TArray<uint8> KeyBytes;
			TSharedPtr<FJsonValue> Value;
		};
		TArray<FEntry> Entries;
		TArray<TPair<FString, FString>> SourceKeys;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
		{
			FString Key;
			FMHCanonicalResult Result = MHNormalizeNfc(Pair.Key, Key);
			if (!Result.bSuccess)
			{
				return Result;
			}
			if (const FString* Existing = FindExactSourceKey(SourceKeys, Key))
			{
				return FMHCanonicalResult::Failure(FString::Printf(
					TEXT("MH_E_INVALID_CANONICAL_JSON: keys collide after NFC normalization: %s and %s"),
					**Existing,
					*Pair.Key));
			}
			SourceKeys.Emplace(Key, Pair.Key);
			FTCHARToUTF8 Utf8(*Key, Key.Len());
			FEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Key = MoveTemp(Key);
			Entry.KeyBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			Entry.Value = Pair.Value;
		}
		Entries.Sort([](const FEntry& A, const FEntry& B)
		{
			return CompareBytes(A.KeyBytes, B.KeyBytes) < 0;
		});

		Out.Add(static_cast<uint8>('{'));
		bool bFirst = true;
		for (const FEntry& Entry : Entries)
		{
			if (!bFirst)
			{
				Out.Add(static_cast<uint8>(','));
			}
			bFirst = false;
			FMHCanonicalResult Result = AppendString(Entry.Key, Out);
			if (!Result.bSuccess)
			{
				return Result;
			}
			Out.Add(static_cast<uint8>(':'));
			Result = AppendCanonical(Entry.Value, Out);
			if (!Result.bSuccess)
			{
				return Result;
			}
		}
		Out.Add(static_cast<uint8>('}'));
		return FMHCanonicalResult::Success();
	}
	default:
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_CANONICAL_JSON: unsupported JSON type"));
	}
}

FString DiskNumber(const int64 Quantized)
{
	constexpr int64 Scale = 1000000;
	if (Quantized % Scale == 0)
	{
		return LexToString(Quantized / Scale);
	}
	const bool bNegative = Quantized < 0;
	const uint64 Magnitude = bNegative ? static_cast<uint64>(-(Quantized + 1)) + 1 : static_cast<uint64>(Quantized);
	const uint64 Whole = Magnitude / Scale;
	FString Fraction = FString::Printf(TEXT("%06llu"), Magnitude % Scale);
	while (Fraction.EndsWith(TEXT("0")))
	{
		Fraction.LeftChopInline(1);
	}
	return FString::Printf(TEXT("%s%llu.%s"), bNegative ? TEXT("-") : TEXT(""), Whole, *Fraction);
}

FMHCanonicalResult MaterialValue(
	const TSharedPtr<FJsonValue>& Value,
	TSharedPtr<FJsonValue>& OutDisk,
	TSharedPtr<FJsonValue>& OutCanonical)
{
	if (!Value.IsValid())
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_MATERIAL_VALUE: invalid JSON value"));
	}
	switch (Value->Type)
	{
	case EJson::Null:
		OutDisk = MakeShared<FJsonValueNull>();
		OutCanonical = MakeShared<FJsonValueNull>();
		return FMHCanonicalResult::Success();
	case EJson::Boolean:
		OutDisk = MakeShared<FJsonValueBoolean>(Value->AsBool());
		OutCanonical = MakeShared<FJsonValueBoolean>(Value->AsBool());
		return FMHCanonicalResult::Success();
	case EJson::String:
	{
		FString Normalized;
		FMHCanonicalResult Result = MHNormalizeNfc(Value->AsString(), Normalized);
		if (Result.bSuccess)
		{
			OutDisk = MakeShared<FJsonValueString>(Normalized);
			OutCanonical = MakeShared<FJsonValueString>(MoveTemp(Normalized));
		}
		return Result;
	}
	case EJson::Number:
	{
		double Number = 0.0;
		FString Raw;
		if (!ParseNumber(Value, Number, Raw))
		{
			return FMHCanonicalResult::Failure(TEXT("MH_E_NAN_INF_VALUE: invalid material number"));
		}
		int64 Quantized = 0;
		FMHCanonicalResult Result = MHQuantize(Number, DefaultPrecision, Quantized);
		if (Result.bSuccess)
		{
			OutDisk = MakeShared<FJsonValueNumberString>(DiskNumber(Quantized));
			OutCanonical = MakeInteger(Quantized);
		}
		return Result;
	}
	case EJson::Array:
	{
		TArray<TSharedPtr<FJsonValue>> DiskItems;
		TArray<TSharedPtr<FJsonValue>> CanonicalItems;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			TSharedPtr<FJsonValue> DiskItem;
			TSharedPtr<FJsonValue> CanonicalItem;
			FMHCanonicalResult Result = MaterialValue(Item, DiskItem, CanonicalItem);
			if (!Result.bSuccess)
			{
				return Result;
			}
			DiskItems.Add(DiskItem);
			CanonicalItems.Add(CanonicalItem);
		}
		OutDisk = MakeShared<FJsonValueArray>(MoveTemp(DiskItems));
		OutCanonical = MakeShared<FJsonValueArray>(MoveTemp(CanonicalItems));
		return FMHCanonicalResult::Success();
	}
	case EJson::Object:
	{
		TSharedPtr<FJsonObject> DiskObject = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> CanonicalObject = MakeShared<FJsonObject>();
		TArray<TPair<FString, FString>> SourceKeys;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
		{
			FString Key;
			FMHCanonicalResult Result = MHNormalizeNfc(Pair.Key, Key);
			if (!Result.bSuccess)
			{
				return Result;
			}
			if (const FString* Existing = FindExactSourceKey(SourceKeys, Key))
			{
				return FMHCanonicalResult::Failure(FString::Printf(
					TEXT("MH_E_INVALID_MATERIAL_VALUE: keys collide after NFC normalization: %s and %s"),
					**Existing,
					*Pair.Key));
			}
			SourceKeys.Emplace(Key, Pair.Key);
			TSharedPtr<FJsonValue> DiskItem;
			TSharedPtr<FJsonValue> CanonicalItem;
			Result = MaterialValue(Pair.Value, DiskItem, CanonicalItem);
			if (!Result.bSuccess)
			{
				return Result;
			}
			DiskObject->SetField(Key, DiskItem);
			CanonicalObject->SetField(Key, CanonicalItem);
		}
		OutDisk = MakeShared<FJsonValueObject>(DiskObject);
		OutCanonical = MakeShared<FJsonValueObject>(CanonicalObject);
		return FMHCanonicalResult::Success();
	}
	default:
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_MATERIAL_VALUE: unsupported JSON type"));
	}
}

enum class EPathFlavor : uint8
{
	Relative,
	WindowsDrive,
	WindowsUnc,
	Posix
};

struct FNormalizedPath
{
	EPathFlavor Flavor = EPathFlavor::Relative;
	FString Prefix;
	TArray<FString> Segments;
};

EPathFlavor DetectFlavor(const FString& Path)
{
	if (Path.Len() >= 3 && FChar::IsAlpha(Path[0]) && Path[1] == TEXT(':') && Path[2] == TEXT('/'))
	{
		return EPathFlavor::WindowsDrive;
	}
	if (Path.StartsWith(TEXT("//")))
	{
		return EPathFlavor::WindowsUnc;
	}
	if (Path.StartsWith(TEXT("/")))
	{
		return EPathFlavor::Posix;
	}
	return EPathFlavor::Relative;
}

FMHCanonicalResult ParseNormalizedPath(const FString& Input, FNormalizedPath& Out)
{
	FString Path = Input;
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	Out.Flavor = DetectFlavor(Path);
	int32 StartIndex = 0;
	int32 ProtectedSegments = 0;
	if (Out.Flavor == EPathFlavor::WindowsDrive)
	{
		Out.Prefix = FString::Printf(TEXT("%c:/"), FChar::ToUpper(Path[0]));
		StartIndex = 3;
	}
	else if (Out.Flavor == EPathFlavor::WindowsUnc)
	{
		Out.Prefix = TEXT("//");
		StartIndex = 2;
		ProtectedSegments = 2;
	}
	else if (Out.Flavor == EPathFlavor::Posix)
	{
		Out.Prefix = TEXT("/");
		StartIndex = 1;
	}

	TArray<FString> RawSegments;
	Path.Mid(StartIndex).ParseIntoArray(RawSegments, TEXT("/"), true);
	for (const FString& Segment : RawSegments)
	{
		if (Segment.IsEmpty() || Segment == TEXT("."))
		{
			continue;
		}
		if (Segment == TEXT(".."))
		{
			if (Out.Segments.Num() > ProtectedSegments && Out.Segments.Last() != TEXT(".."))
			{
				Out.Segments.Pop();
			}
			else if (Out.Flavor == EPathFlavor::Relative)
			{
				Out.Segments.Add(Segment);
			}
			continue;
		}
		Out.Segments.Add(Segment);
	}
	if (Out.Flavor == EPathFlavor::WindowsUnc && Out.Segments.Num() < 2)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_MATERIAL_VALUE: UNC path requires server and share"));
	}
	return FMHCanonicalResult::Success();
}

FString RenderPath(const FNormalizedPath& Path)
{
	const FString Body = FString::Join(Path.Segments, TEXT("/"));
	if (Path.Flavor == EPathFlavor::Relative)
	{
		return Body.IsEmpty() ? TEXT(".") : Body;
	}
	return Path.Prefix + Body;
}

bool IsWindowsFlavor(const EPathFlavor Flavor)
{
	return Flavor == EPathFlavor::WindowsDrive || Flavor == EPathFlavor::WindowsUnc;
}

bool SameAbsoluteFlavor(const FNormalizedPath& A, const FNormalizedPath& B)
{
	if (A.Flavor != B.Flavor)
	{
		return false;
	}
	if (A.Flavor == EPathFlavor::WindowsDrive)
	{
		return A.Prefix.Equals(B.Prefix, ESearchCase::IgnoreCase);
	}
	return A.Flavor == EPathFlavor::WindowsUnc || A.Flavor == EPathFlavor::Posix;
}

bool IsInside(const FNormalizedPath& Path, const FNormalizedPath& Root)
{
	if (!SameAbsoluteFlavor(Path, Root) || Path.Segments.Num() < Root.Segments.Num())
	{
		return false;
	}
	const ESearchCase::Type SearchCase = IsWindowsFlavor(Root.Flavor) ? ESearchCase::IgnoreCase : ESearchCase::CaseSensitive;
	for (int32 Index = 0; Index < Root.Segments.Num(); ++Index)
	{
		if (!Path.Segments[Index].Equals(Root.Segments[Index], SearchCase))
		{
			return false;
		}
	}
	return true;
}

FMHCanonicalResult NormalizeRoot(const FString& Input, FNormalizedPath& OutRoot)
{
	FMHCanonicalResult Result = ParseNormalizedPath(Input, OutRoot);
	if (!Result.bSuccess)
	{
		return Result;
	}
	if (OutRoot.Flavor == EPathFlavor::Relative)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_MATERIAL_VALUE: source_root must be absolute"));
	}
	return FMHCanonicalResult::Success();
}

} // namespace Private

FMHCanonicalResult FMHCanonicalResult::Success()
{
	return {true, FString()};
}

FMHCanonicalResult FMHCanonicalResult::Failure(FString InError)
{
	return {false, MoveTemp(InError)};
}

FMHCanonicalResult MHParseJsonUtf8(TConstArrayView<uint8> Bytes, TSharedPtr<FJsonValue>& OutValue)
{
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	const FString Json(Text.Length(), Text.Get());
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, OutValue, FJsonSerializer::EFlags::StoreNumbersAsStrings) || !OutValue.IsValid())
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_CANONICAL_JSON: JSON parse failed"));
	}
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult MHQuantize(const double Value, const int32 Precision, int64& OutQuantized)
{
	if (!FMath::IsFinite(Value) || Precision < 0 || Precision > 18)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_NAN_INF_VALUE: invalid quantization input"));
	}
	double Scale = 1.0;
	for (int32 Index = 0; Index < Precision; ++Index)
	{
		Scale *= 10.0;
	}
	const double Scaled = Value * Scale;
	constexpr double Int64MinInclusive = -9223372036854775808.0;
	constexpr double Int64MaxExclusive = 9223372036854775808.0;
	if (!FMath::IsFinite(Scaled) || Scaled < Int64MinInclusive || Scaled >= Int64MaxExclusive)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_NAN_INF_VALUE: quantized value overflows int64"));
	}
	OutQuantized = FMath::RoundToNearestTiesToEven(Scaled);
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult MHCanonicalizeQuaternion(
	const TConstArrayView<double> Quaternion,
	TArray<int64>& OutQuaternion)
{
	if (Quaternion.Num() != 4)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_COMPOSITE: quaternion must have four components"));
	}
	double SquaredNorm = 0.0;
	for (const double Component : Quaternion)
	{
		if (!FMath::IsFinite(Component))
		{
			return FMHCanonicalResult::Failure(TEXT("MH_E_NAN_INF_VALUE: invalid quaternion component"));
		}
		SquaredNorm += Component * Component;
	}
	const double Norm = FMath::Sqrt(SquaredNorm);
	if (!FMath::IsFinite(Norm) || Norm == 0.0)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_COMPOSITE: degenerate quaternion"));
	}
	OutQuaternion.Reset(4);
	for (const double Component : Quaternion)
	{
		int64 Quantized = 0;
		FMHCanonicalResult Result = MHQuantize(Component / Norm, Private::RotationPrecision, Quantized);
		if (!Result.bSuccess)
		{
			return Result;
		}
		OutQuaternion.Add(Quantized);
	}
	Private::CanonicalizeQuaternionSign(OutQuaternion);
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult MHNormalizeNfc(FStringView Input, FString& OutNormalized)
{
#if UE_ENABLE_ICU
	const FString Source(Input);
	FTCHARToUTF16 Utf16(*Source, Source.Len());
	UErrorCode Status = U_ZERO_ERROR;
	const UNormalizer2* Normalizer = unorm2_getNFCInstance(&Status);
	if (U_FAILURE(Status) || Normalizer == nullptr)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_NFC_UNAVAILABLE: ICU NFC normalizer unavailable"));
	}
	Status = U_ZERO_ERROR;
	const int32 Required = unorm2_normalize(
		Normalizer,
		reinterpret_cast<const UChar*>(Utf16.Get()),
		Utf16.Length(),
		nullptr,
		0,
		&Status);
	if (Status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(Status))
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_NFC_UNAVAILABLE: ICU NFC sizing failed"));
	}
	TArray<UChar> Buffer;
	Buffer.SetNumUninitialized(Required);
	Status = U_ZERO_ERROR;
	const int32 Written = unorm2_normalize(
		Normalizer,
		reinterpret_cast<const UChar*>(Utf16.Get()),
		Utf16.Length(),
		Buffer.GetData(),
		Buffer.Num(),
		&Status);
	if (U_FAILURE(Status))
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_NFC_UNAVAILABLE: ICU NFC normalization failed"));
	}
	FUTF16ToTCHAR Converted(reinterpret_cast<const UTF16CHAR*>(Buffer.GetData()), Written);
	OutNormalized = FString(Converted.Length(), Converted.Get());
	return FMHCanonicalResult::Success();
#else
	OutNormalized.Reset();
	return FMHCanonicalResult::Failure(TEXT("MH_E_NFC_UNAVAILABLE: UE target was compiled without ICU"));
#endif
}

FMHCanonicalResult MHCanonicalJsonBytes(const TSharedPtr<FJsonValue>& Value, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	return Private::AppendCanonical(Value, OutBytes);
}

FMHCanonicalResult MHCanonicalJsonObjectPairsBytes(
	TConstArrayView<TPair<FString, TSharedPtr<FJsonValue>>> Pairs,
	TArray<uint8>& OutBytes)
{
	struct FEntry
	{
		FString Key;
		TArray<uint8> KeyBytes;
		TSharedPtr<FJsonValue> Value;
	};
	TArray<FEntry> Entries;
	TArray<TPair<FString, FString>> SourceKeys;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Pairs)
	{
		FString Key;
		FMHCanonicalResult Result = MHNormalizeNfc(Pair.Key, Key);
		if (!Result.bSuccess)
		{
			return Result;
		}
		if (const FString* Existing = Private::FindExactSourceKey(SourceKeys, Key))
		{
			return FMHCanonicalResult::Failure(FString::Printf(
				TEXT("MH_E_INVALID_CANONICAL_JSON: keys collide after NFC normalization: %s and %s"),
				**Existing,
				*Pair.Key));
		}
		SourceKeys.Emplace(Key, Pair.Key);
		FTCHARToUTF8 Utf8(*Key, Key.Len());
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Key = MoveTemp(Key);
		Entry.KeyBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Entry.Value = Pair.Value;
	}
	Entries.Sort([](const FEntry& A, const FEntry& B)
	{
		return Private::CompareBytes(A.KeyBytes, B.KeyBytes) < 0;
	});

	OutBytes.Reset();
	OutBytes.Add(static_cast<uint8>('{'));
	bool bFirst = true;
	for (const FEntry& Entry : Entries)
	{
		if (!bFirst)
		{
			OutBytes.Add(static_cast<uint8>(','));
		}
		bFirst = false;
		FMHCanonicalResult Result = Private::AppendString(Entry.Key, OutBytes);
		if (!Result.bSuccess)
		{
			return Result;
		}
		OutBytes.Add(static_cast<uint8>(':'));
		Result = Private::AppendCanonical(Entry.Value, OutBytes);
		if (!Result.bSuccess)
		{
			return Result;
		}
	}
	OutBytes.Add(static_cast<uint8>('}'));
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult MHCompositeCanonicalForm(
	const TSharedPtr<FJsonValue>& Document,
	TSharedPtr<FJsonValue>& OutCanonical)
{
	if (!Document.IsValid() || Document->Type != EJson::Object)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_COMPOSITE: document must be an object"));
	}
	TSharedPtr<FJsonObject> Object;
	FMHCanonicalResult Result = Private::NormalizeObject(
		Document->AsObject(),
		Object,
		[](const FString& Key, const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& OutValue)
		{
			if (Key == TEXT("nodes") && Value.IsValid() && Value->Type == EJson::Array)
			{
				struct FNode
				{
					TArray<uint8> SortKey;
					TSharedPtr<FJsonValue> Value;
				};
				TArray<FNode> Nodes;
				for (const TSharedPtr<FJsonValue>& Node : Value->AsArray())
				{
					TSharedPtr<FJsonValue> CanonicalNode;
					FMHCanonicalResult NodeResult = Private::CanonNode(Node, CanonicalNode);
					if (!NodeResult.bSuccess)
					{
						return NodeResult;
					}
					FNode& Row = Nodes.AddDefaulted_GetRef();
					Row.Value = CanonicalNode;
					if (CanonicalNode->Type == EJson::Object)
					{
						const TSharedPtr<FJsonValue>* Uid = CanonicalNode->AsObject()->Values.Find(TEXT("node_uid"));
						if (Uid != nullptr && Uid->IsValid() && (*Uid)->Type == EJson::String)
						{
							FTCHARToUTF8 Utf8(*(*Uid)->AsString());
							Row.SortKey.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
						}
					}
				}
				Nodes.Sort([](const FNode& A, const FNode& B)
				{
					return Private::CompareBytes(A.SortKey, B.SortKey) < 0;
				});
				TArray<TSharedPtr<FJsonValue>> Sorted;
				for (FNode& Node : Nodes)
				{
					Sorted.Add(MoveTemp(Node.Value));
				}
				OutValue = MakeShared<FJsonValueArray>(MoveTemp(Sorted));
				return FMHCanonicalResult::Success();
			}
			if (Key == TEXT("schema_version") || Key == TEXT("seed_salt"))
			{
				return Private::CanonPassthroughInteger(Value, OutValue);
			}
			return Private::CanonGeneric(Value, Private::DefaultPrecision, OutValue);
		});
	if (Result.bSuccess)
	{
		OutCanonical = MakeShared<FJsonValueObject>(Object);
	}
	return Result;
}

FMHCanonicalResult MHMaterialForms(
	FStringView ShaderClass,
	const TSharedPtr<FJsonValue>& Params,
	const TSharedPtr<FJsonValue>& Textures,
	TSharedPtr<FJsonValue>& OutDisk,
	TSharedPtr<FJsonValue>& OutCanonical)
{
	if (!Params.IsValid() || Params->Type != EJson::Object || !Textures.IsValid() || Textures->Type != EJson::Object)
	{
		return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_MATERIAL_VALUE: params and textures must be objects"));
	}
	FString NormalizedShader;
	FMHCanonicalResult Result = MHNormalizeNfc(ShaderClass, NormalizedShader);
	if (!Result.bSuccess)
	{
		return Result;
	}
	TSharedPtr<FJsonValue> DiskParams;
	TSharedPtr<FJsonValue> CanonicalParams;
	Result = Private::MaterialValue(Params, DiskParams, CanonicalParams);
	if (!Result.bSuccess)
	{
		return Result;
	}
	TSharedPtr<FJsonValue> DiskTextures;
	TSharedPtr<FJsonValue> CanonicalTextures;
	Result = Private::MaterialValue(Textures, DiskTextures, CanonicalTextures);
	if (!Result.bSuccess)
	{
		return Result;
	}
	TSharedPtr<FJsonObject> DiskObject = MakeShared<FJsonObject>();
	DiskObject->SetStringField(TEXT("shader_class"), NormalizedShader);
	DiskObject->SetField(TEXT("params"), DiskParams);
	DiskObject->SetField(TEXT("textures"), DiskTextures);
	OutDisk = MakeShared<FJsonValueObject>(DiskObject);

	TSharedPtr<FJsonObject> CanonicalObject = MakeShared<FJsonObject>();
	CanonicalObject->SetStringField(TEXT("shader_class"), NormalizedShader);
	CanonicalObject->SetField(TEXT("params"), CanonicalParams);
	CanonicalObject->SetField(TEXT("textures"), CanonicalTextures);
	OutCanonical = MakeShared<FJsonValueObject>(CanonicalObject);
	return FMHCanonicalResult::Success();
}

FString MHXxh3Hash(TConstArrayView<uint8> Bytes)
{
	const FXxHash64 Hash = FXxHash64::HashBuffer(Bytes.GetData(), Bytes.Num());
	return FString::Printf(TEXT("xxh3:%016llx"), Hash.Hash);
}

FMHTexturePathResult MHCanonicalizeTexturePath(FStringView AuthoredPath, FStringView SourceRoot)
{
	FString Authored(AuthoredPath);
	FString RootText(SourceRoot);
	Authored.TrimStartAndEndInline();
	RootText.TrimStartAndEndInline();
	FMHCanonicalResult Result = MHNormalizeNfc(Authored, Authored);
	if (!Result.bSuccess || Authored.IsEmpty())
	{
		return {false, false, FString(), Result.bSuccess ? TEXT("MH_E_INVALID_MATERIAL_VALUE: texture path must be non-empty") : Result.Error};
	}
	Result = MHNormalizeNfc(RootText, RootText);
	if (!Result.bSuccess)
	{
		return {false, false, FString(), Result.Error};
	}
	Private::FNormalizedPath Root;
	Result = Private::NormalizeRoot(RootText, Root);
	if (!Result.bSuccess)
	{
		return {false, false, FString(), Result.Error};
	}
	Private::FNormalizedPath Path;
	Result = Private::ParseNormalizedPath(Authored, Path);
	if (!Result.bSuccess)
	{
		return {false, false, FString(), Result.Error};
	}
	if (Path.Flavor == Private::EPathFlavor::Relative)
	{
		Private::FNormalizedPath Joined = Root;
		for (const FString& Segment : Path.Segments)
		{
			if (Segment == TEXT(".."))
			{
				const int32 Protected = Root.Flavor == Private::EPathFlavor::WindowsUnc ? 2 : 0;
				if (Joined.Segments.Num() > Protected)
				{
					Joined.Segments.Pop();
				}
			}
			else
			{
				Joined.Segments.Add(Segment);
			}
		}
		Path = MoveTemp(Joined);
	}
	if (Private::IsInside(Path, Root))
	{
		if (Path.Segments.Num() == Root.Segments.Num())
		{
			return {false, false, FString(), TEXT("MH_E_INVALID_MATERIAL_VALUE: texture path cannot name source_root itself")};
		}
		TArray<FString> Relative;
		for (int32 Index = Root.Segments.Num(); Index < Path.Segments.Num(); ++Index)
		{
			Relative.Add(Path.Segments[Index]);
		}
		return {true, false, FString::Join(Relative, TEXT("/")), FString()};
	}
	return {true, true, Private::RenderPath(Path), FString()};
}

FMHTexturePathResult MHValidateDiskTexturePath(FStringView DiskPath, FStringView SourceRoot)
{
	FString Value(DiskPath);
	FString RootText(SourceRoot);
	if (Value.IsEmpty())
	{
		return {false, false, FString(), TEXT("MH_E_INVALID_MATERIAL_VALUE: texture path must be non-empty")};
	}
	FString NormalizedNfc;
	FMHCanonicalResult Result = MHNormalizeNfc(Value, NormalizedNfc);
	if (!Result.bSuccess)
	{
		return {false, false, FString(), Result.Error};
	}
	if (NormalizedNfc != Value)
	{
		return {false, false, FString(), TEXT("MH_E_INVALID_MATERIAL_VALUE: texture path must be NFC-normalized")};
	}
	Result = MHNormalizeNfc(RootText, RootText);
	if (!Result.bSuccess)
	{
		return {false, false, FString(), Result.Error};
	}
	Private::FNormalizedPath Root;
	Result = Private::NormalizeRoot(RootText, Root);
	if (!Result.bSuccess)
	{
		return {false, false, FString(), Result.Error};
	}
	Private::FNormalizedPath Parsed;
	Result = Private::ParseNormalizedPath(Value, Parsed);
	if (!Result.bSuccess)
	{
		return {false, false, FString(), Result.Error};
	}
	if (Parsed.Flavor == Private::EPathFlavor::Relative)
	{
		if (Value.Contains(TEXT("\\")) || Private::RenderPath(Parsed) != Value || Value == TEXT(".") || Parsed.Segments.Contains(TEXT("..")))
		{
			return {false, false, FString(), TEXT("MH_E_INVALID_MATERIAL_VALUE: relative texture path is not normalized")};
		}
		return {true, false, Value, FString()};
	}
	const FString Normalized = Private::RenderPath(Parsed);
	if (Normalized != Value)
	{
		return {false, false, FString(), TEXT("MH_E_INVALID_MATERIAL_VALUE: absolute texture path is not normalized")};
	}
	if (Private::IsInside(Parsed, Root))
	{
		return {false, false, FString(), TEXT("MH_E_INVALID_MATERIAL_VALUE: absolute path inside source_root must be relative")};
	}
	return {true, true, Value, FString()};
}

} // namespace UE::MimirComposite
