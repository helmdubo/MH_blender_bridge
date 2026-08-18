#include "Codec/MHMaterialCodec.h"

#include "Codec/MHCompositeCodec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::MimirComposite
{
namespace
{

FMHCanonicalResult Invalid(const TCHAR* Message)
{
	return FMHCanonicalResult::Failure(
		FString::Printf(TEXT("MH_E_INVALID_MATERIAL_VALUE: %s"), Message));
}

bool SerializeCompactObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
{
	OutJson.Reset();
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
}

bool IsTextureSlotName(const FString& Key)
{
	if (!Key.StartsWith(TEXT("tex")))
	{
		return false;
	}
	const FString Digits = Key.Mid(3);
	if (Digits.IsEmpty() || Digits.Len() > 2 || (Digits.Len() > 1 && Digits[0] == TEXT('0')))
	{
		return false;
	}
	int32 Slot = 0;
	return LexTryParseString(Slot, *Digits) && Slot >= 0 && Slot <= 15;
}

} // namespace

FMHCanonicalResult MHParseMaterialV1(
	TConstArrayView<uint8> Bytes,
	FMHMaterialDocument& OutDocument)
{
	OutDocument = FMHMaterialDocument();

	TSharedPtr<FJsonValue> RootValue;
	FMHCanonicalResult Result = MHParseJsonUtf8(Bytes, RootValue);
	if (!Result.bSuccess)
	{
		return Invalid(*Result.Error);
	}
	if (!RootValue.IsValid() || RootValue->Type != EJson::Object)
	{
		return Invalid(TEXT("document must be an object"));
	}
	const TSharedPtr<FJsonObject> Root = RootValue->AsObject();

	const TSharedPtr<FJsonValue> Schema = Root->TryGetField(TEXT("schema"));
	if (!Schema.IsValid() || Schema->Type != EJson::String || Schema->AsString() != TEXT("mh.material"))
	{
		return Invalid(TEXT("schema must be mh.material"));
	}
	int64 Version = 0;
	const TSharedPtr<FJsonValue> SchemaVersion = Root->TryGetField(TEXT("schema_version"));
	if (!SchemaVersion.IsValid() || SchemaVersion->Type != EJson::Number ||
		!SchemaVersion->TryGetNumber(Version))
	{
		return Invalid(TEXT("schema_version must be an integer"));
	}
	if (Version != 1)
	{
		return FMHCanonicalResult::Failure(FString::Printf(
			TEXT("MH_E_UNKNOWN_SCHEMA_VERSION: mh.material version %lld is not supported"),
			Version));
	}

	if (Root->Values.Num() != 7)
	{
		return Invalid(TEXT("document carries unknown or missing top-level fields"));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		const FString& Key = Pair.Key;
		if (Key != TEXT("schema") && Key != TEXT("schema_version") && Key != TEXT("uid") &&
			Key != TEXT("name") && Key != TEXT("shader_class") && Key != TEXT("params") &&
			Key != TEXT("textures"))
		{
			return Invalid(*FString::Printf(TEXT("unknown top-level field %s"), *Key));
		}
	}

	const TSharedPtr<FJsonValue> Uid = Root->TryGetField(TEXT("uid"));
	if (!Uid.IsValid() || Uid->Type != EJson::String || !MHIsCanonicalUuid(Uid->AsString()))
	{
		return Invalid(TEXT("uid must be a canonical lowercase UUID"));
	}
	OutDocument.Uid = Uid->AsString();

	const TSharedPtr<FJsonValue> Name = Root->TryGetField(TEXT("name"));
	if (!Name.IsValid() || Name->Type != EJson::String)
	{
		return Invalid(TEXT("name must be a string"));
	}
	if (!MHIsValidResourceName(Name->AsString()))
	{
		return FMHCanonicalResult::Failure(TEXT(
			"MH_E_NON_ASCII_RESOURCE_NAME: resource name must match [A-Za-z0-9_ -]"));
	}
	OutDocument.Name = Name->AsString();

	const TSharedPtr<FJsonValue> ShaderClass = Root->TryGetField(TEXT("shader_class"));
	if (!ShaderClass.IsValid() || ShaderClass->Type != EJson::String || ShaderClass->AsString().IsEmpty())
	{
		return Invalid(TEXT("shader_class must be a non-empty string"));
	}
	Result = MHNormalizeNfc(ShaderClass->AsString(), OutDocument.ShaderClass);
	if (!Result.bSuccess)
	{
		return Result;
	}

	const TSharedPtr<FJsonValue> Params = Root->TryGetField(TEXT("params"));
	if (!Params.IsValid() || Params->Type != EJson::Object)
	{
		return Invalid(TEXT("params must be an object"));
	}
	if (!SerializeCompactObject(Params->AsObject(), OutDocument.ParamsJson))
	{
		return Invalid(TEXT("params cannot be serialized"));
	}

	const TSharedPtr<FJsonValue> Textures = Root->TryGetField(TEXT("textures"));
	if (!Textures.IsValid() || Textures->Type != EJson::Object)
	{
		return Invalid(TEXT("textures must be an object"));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Textures->AsObject()->Values)
	{
		if (!IsTextureSlotName(Pair.Key))
		{
			return Invalid(*FString::Printf(TEXT("unknown texture slot %s"), *Pair.Key));
		}
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String || Pair.Value->AsString().IsEmpty())
		{
			return Invalid(*FString::Printf(TEXT("texture slot %s must be a non-empty string"), *Pair.Key));
		}
	}
	if (!SerializeCompactObject(Textures->AsObject(), OutDocument.TexturesJson))
	{
		return Invalid(TEXT("textures cannot be serialized"));
	}
	return FMHCanonicalResult::Success();
}

} // namespace UE::MimirComposite
