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

constexpr int32 TransformPrecision = 6;

FMHCanonicalResult Invalid(const TCHAR* Message)
{
	return FMHCanonicalResult::Failure(FString::Printf(TEXT("MH_E_INVALID_COMPOSITE: %s"), Message));
}

bool GetFiniteNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
{
	if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(OutNumber))
	{
		return false;
	}
	return FMath::IsFinite(OutNumber);
}

bool SerializeCompactObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
{
	OutJson.Reset();
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
}

FMHCanonicalResult ParseVector(
	const TSharedPtr<FJsonValue>& Value,
	const int32 ExpectedLength,
	const TCHAR* FieldName,
	TArray<double>& OutComponents)
{
	OutComponents.Reset();
	if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != ExpectedLength)
	{
		return Invalid(*FString::Printf(TEXT("%s must contain exactly %d numbers"), FieldName, ExpectedLength));
	}
	for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
	{
		double Number = 0.0;
		if (!GetFiniteNumber(Item, Number))
		{
			return Invalid(*FString::Printf(TEXT("%s must contain finite numbers"), FieldName));
		}
		OutComponents.Add(Number);
	}
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult ParseTransform(const TSharedPtr<FJsonValue>& Value, FMHCompositeNode& OutNode)
{
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return Invalid(TEXT("local_transform must be an object"));
	}
	const TSharedPtr<FJsonObject> Object = Value->AsObject();
	if (Object->Values.Num() != 3 ||
		!Object->Values.Contains(TEXT("translation_cm")) ||
		!Object->Values.Contains(TEXT("rotation_quat")) ||
		!Object->Values.Contains(TEXT("scale")))
	{
		return Invalid(TEXT("local_transform must contain exactly translation_cm, rotation_quat and scale"));
	}

	TArray<double> Translation;
	FMHCanonicalResult Result = ParseVector(
		Object->TryGetField(TEXT("translation_cm")), 3, TEXT("translation_cm"), Translation);
	if (!Result.bSuccess)
	{
		return Result;
	}

	TArray<double> Quaternion;
	Result = ParseVector(Object->TryGetField(TEXT("rotation_quat")), 4, TEXT("rotation_quat"), Quaternion);
	if (!Result.bSuccess)
	{
		return Result;
	}
	TArray<int64> CanonicalQuaternion;
	Result = MHCanonicalizeQuaternion(Quaternion, CanonicalQuaternion);
	if (!Result.bSuccess)
	{
		return Result;
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		int64 AuthoredTick = 0;
		Result = MHQuantize(Quaternion[Index], TransformPrecision, AuthoredTick);
		if (!Result.bSuccess)
		{
			return Result;
		}
		if (AuthoredTick != CanonicalQuaternion[Index])
		{
			return Invalid(TEXT("rotation_quat must be normalized and sign-canonicalized"));
		}
	}

	TArray<double> Scale;
	Result = ParseVector(Object->TryGetField(TEXT("scale")), 3, TEXT("scale"), Scale);
	if (!Result.bSuccess)
	{
		return Result;
	}
	for (const double Component : Scale)
	{
		int64 ScaleTick = 0;
		Result = MHQuantize(Component, TransformPrecision, ScaleTick);
		if (!Result.bSuccess)
		{
			return Result;
		}
		if (Component <= 0.0 || ScaleTick <= 0)
		{
			return FMHCanonicalResult::Failure(TEXT("MH_E_INVALID_SCALE: scale must be strictly positive per axis"));
		}
	}

	OutNode.TranslationCm = FVector(Translation[0], Translation[1], Translation[2]);
	OutNode.RotationQuat = FQuat(Quaternion[0], Quaternion[1], Quaternion[2], Quaternion[3]);
	OutNode.Scale = FVector(Scale[0], Scale[1], Scale[2]);
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult ParseNode(const TSharedPtr<FJsonValue>& Value, FMHCompositeNode& OutNode)
{
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return Invalid(TEXT("nodes entries must be objects"));
	}
	const TSharedPtr<FJsonObject> Object = Value->AsObject();

	const TSharedPtr<FJsonValue> KindValue = Object->TryGetField(TEXT("kind"));
	if (!KindValue.IsValid() || KindValue->Type != EJson::String)
	{
		return Invalid(TEXT("node kind must be a string"));
	}
	const FString Kind = KindValue->AsString();
	bool bNeedsResource = false;
	if (Kind == TEXT("group"))
	{
		OutNode.Kind = EMHCompositeNodeKind::Group;
	}
	else if (Kind == TEXT("mesh"))
	{
		OutNode.Kind = EMHCompositeNodeKind::Mesh;
		bNeedsResource = true;
	}
	else if (Kind == TEXT("composite_ref"))
	{
		OutNode.Kind = EMHCompositeNodeKind::CompositeRef;
		bNeedsResource = true;
	}
	else if (Kind == TEXT("variant_set") || Kind == TEXT("variant") || Kind == TEXT("actor"))
	{
		return FMHCanonicalResult::Failure(FString::Printf(
			TEXT("MH_E_UNSUPPORTED_NODE_KIND: reserved kind %s has no runtime contract"),
			*Kind));
	}
	else
	{
		return Invalid(*FString::Printf(TEXT("unknown node kind %s"), *Kind));
	}

	const int32 ExpectedFields = bNeedsResource ? 7 : 6;
	if (Object->Values.Num() != ExpectedFields)
	{
		return Invalid(TEXT("node carries unknown or missing fields"));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		const FString& Key = Pair.Key;
		const bool bKnown =
			Key == TEXT("node_uid") || Key == TEXT("parent_uid") || Key == TEXT("kind") ||
			Key == TEXT("display_name") || Key == TEXT("local_transform") || Key == TEXT("properties") ||
			(bNeedsResource && Key == TEXT("resource_uid"));
		if (!bKnown)
		{
			return Invalid(*FString::Printf(TEXT("unknown node field %s"), *Key));
		}
	}

	const TSharedPtr<FJsonValue> NodeUid = Object->TryGetField(TEXT("node_uid"));
	if (!NodeUid.IsValid() || NodeUid->Type != EJson::String || !MHIsCanonicalUuid(NodeUid->AsString()))
	{
		return Invalid(TEXT("node_uid must be a canonical lowercase UUID"));
	}
	OutNode.NodeUid = NodeUid->AsString();

	const TSharedPtr<FJsonValue> ParentUid = Object->TryGetField(TEXT("parent_uid"));
	if (!ParentUid.IsValid())
	{
		return Invalid(TEXT("parent_uid is required"));
	}
	if (ParentUid->Type == EJson::Null)
	{
		OutNode.ParentUid.Reset();
	}
	else if (ParentUid->Type == EJson::String && MHIsCanonicalUuid(ParentUid->AsString()))
	{
		OutNode.ParentUid = ParentUid->AsString();
	}
	else
	{
		return Invalid(TEXT("parent_uid must be null or a canonical lowercase UUID"));
	}

	const TSharedPtr<FJsonValue> DisplayName = Object->TryGetField(TEXT("display_name"));
	if (!DisplayName.IsValid() || DisplayName->Type != EJson::String)
	{
		return Invalid(TEXT("display_name must be a string"));
	}
	FMHCanonicalResult Result = MHNormalizeNfc(DisplayName->AsString(), OutNode.DisplayName);
	if (!Result.bSuccess)
	{
		return Result;
	}

	if (bNeedsResource)
	{
		const TSharedPtr<FJsonValue> ResourceUid = Object->TryGetField(TEXT("resource_uid"));
		if (!ResourceUid.IsValid() || ResourceUid->Type != EJson::String ||
			!MHIsCanonicalUuid(ResourceUid->AsString()))
		{
			return Invalid(TEXT("resource_uid must be a canonical lowercase UUID"));
		}
		OutNode.ResourceUid = ResourceUid->AsString();
	}
	else
	{
		OutNode.ResourceUid.Reset();
	}

	Result = ParseTransform(Object->TryGetField(TEXT("local_transform")), OutNode);
	if (!Result.bSuccess)
	{
		return Result;
	}

	const TSharedPtr<FJsonValue> Properties = Object->TryGetField(TEXT("properties"));
	if (!Properties.IsValid() || Properties->Type != EJson::Object)
	{
		return Invalid(TEXT("node properties must be an object"));
	}
	if (!SerializeCompactObject(Properties->AsObject(), OutNode.PropertiesJson))
	{
		return Invalid(TEXT("node properties cannot be serialized"));
	}
	return FMHCanonicalResult::Success();
}

FMHCanonicalResult ValidateHierarchy(const TArray<FMHCompositeNode>& Nodes)
{
	TMap<FString, int32> IndexByUid;
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		if (IndexByUid.Contains(Nodes[Index].NodeUid))
		{
			return Invalid(*FString::Printf(TEXT("duplicate node_uid %s"), *Nodes[Index].NodeUid));
		}
		IndexByUid.Add(Nodes[Index].NodeUid, Index);
	}
	for (const FMHCompositeNode& Node : Nodes)
	{
		if (!Node.ParentUid.IsEmpty() && !IndexByUid.Contains(Node.ParentUid))
		{
			return Invalid(*FString::Printf(TEXT("dangling parent_uid %s"), *Node.ParentUid));
		}
	}
	for (const FMHCompositeNode& Node : Nodes)
	{
		TSet<FString> Visited;
		const FMHCompositeNode* Cursor = &Node;
		while (!Cursor->ParentUid.IsEmpty())
		{
			if (Visited.Contains(Cursor->NodeUid))
			{
				return FMHCanonicalResult::Failure(FString::Printf(
					TEXT("MH_E_PARENT_CYCLE: node %s participates in a parent cycle"),
					*Node.NodeUid));
			}
			Visited.Add(Cursor->NodeUid);
			Cursor = &Nodes[IndexByUid[Cursor->ParentUid]];
		}
	}
	return FMHCanonicalResult::Success();
}

} // namespace

bool MHIsCanonicalUuid(const FString& Value)
{
	if (Value.Len() != 36)
	{
		return false;
	}
	for (int32 Index = 0; Index < 36; ++Index)
	{
		const TCHAR Char = Value[Index];
		if (Index == 8 || Index == 13 || Index == 18 || Index == 23)
		{
			if (Char != TEXT('-'))
			{
				return false;
			}
		}
		else if (!((Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

bool MHIsValidResourceName(const FString& Value)
{
	if (Value.IsEmpty())
	{
		return false;
	}
	for (const TCHAR Char : Value)
	{
		const bool bAllowed =
			(Char >= TEXT('A') && Char <= TEXT('Z')) ||
			(Char >= TEXT('a') && Char <= TEXT('z')) ||
			(Char >= TEXT('0') && Char <= TEXT('9')) ||
			Char == TEXT('_') || Char == TEXT(' ') || Char == TEXT('-');
		if (!bAllowed)
		{
			return false;
		}
	}
	return true;
}

FMHCanonicalResult MHParseCompositeV2(
	TConstArrayView<uint8> Bytes,
	FMHCompositeDocument& OutDocument)
{
	OutDocument = FMHCompositeDocument();

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
	if (!Schema.IsValid() || Schema->Type != EJson::String || Schema->AsString() != TEXT("mh.composite"))
	{
		return Invalid(TEXT("schema must be mh.composite"));
	}
	const TSharedPtr<FJsonValue> SchemaVersion = Root->TryGetField(TEXT("schema_version"));
	int64 Version = 0;
	if (!SchemaVersion.IsValid() || SchemaVersion->Type != EJson::Number ||
		!SchemaVersion->TryGetNumber(Version))
	{
		return Invalid(TEXT("schema_version must be an integer"));
	}
	if (Version == 1)
	{
		return FMHCanonicalResult::Failure(TEXT(
			"MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED: composite v1 is not a runtime candidate"));
	}
	if (Version != 2)
	{
		return FMHCanonicalResult::Failure(FString::Printf(
			TEXT("MH_E_UNKNOWN_SCHEMA_VERSION: mh.composite version %lld is not supported"),
			Version));
	}

	if (Root->Values.Num() != 6)
	{
		return Invalid(TEXT("document carries unknown or missing top-level fields"));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		const FString& Key = Pair.Key;
		if (Key != TEXT("schema") && Key != TEXT("schema_version") && Key != TEXT("uid") &&
			Key != TEXT("name") && Key != TEXT("properties") && Key != TEXT("nodes"))
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

	const TSharedPtr<FJsonValue> Properties = Root->TryGetField(TEXT("properties"));
	if (!Properties.IsValid() || Properties->Type != EJson::Object)
	{
		return Invalid(TEXT("top-level properties must be an object"));
	}
	if (!SerializeCompactObject(Properties->AsObject(), OutDocument.ResourcePropertiesJson))
	{
		return Invalid(TEXT("top-level properties cannot be serialized"));
	}

	const TSharedPtr<FJsonValue> Nodes = Root->TryGetField(TEXT("nodes"));
	if (!Nodes.IsValid() || Nodes->Type != EJson::Array)
	{
		return Invalid(TEXT("nodes must be an array"));
	}
	for (const TSharedPtr<FJsonValue>& NodeValue : Nodes->AsArray())
	{
		FMHCompositeNode Node;
		Result = ParseNode(NodeValue, Node);
		if (!Result.bSuccess)
		{
			return Result;
		}
		OutDocument.Nodes.Add(MoveTemp(Node));
	}
	return ValidateHierarchy(OutDocument.Nodes);
}

} // namespace UE::MimirComposite
