#include "Source/MHPayloadHashes.h"

#include "Canonical/MHCanonical.h"
#include "Codec/MHCompositeCodec.h"
#include "Codec/MHMaterialCodec.h"
#include "Containers/StringConv.h"
#include "Serialization/JsonReader.h"

namespace UE::MimirComposite
{
namespace
{

// FJsonObject stores fields in a TMap<FString, ...>, which cannot represent
// every legal Source Schema object (notably simultaneous "A" and "a" keys).
// Keep ordered member pairs until canonical UTF-8 bytes have been emitted.
enum class EPayloadHashJsonType : uint8
{
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

struct FPayloadHashJsonValue;

struct FPayloadHashJsonMember
{
    FString Key;
    TSharedPtr<FPayloadHashJsonValue> Value;
};

struct FPayloadHashJsonValue
{
    EPayloadHashJsonType Type = EPayloadHashJsonType::Null;
    bool bBoolean = false;
    FString Text;
    TArray<TSharedPtr<FPayloadHashJsonValue>> Array;
    TArray<FPayloadHashJsonMember> Object;
};

bool PayloadHashesFail(FString& OutError, FString Message)
{
    OutError = MoveTemp(Message);
    return false;
}

TArray<uint8> PayloadHashesUtf8(const FString& Text)
{
    FTCHARToUTF8 Utf8(*Text, Text.Len());
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return Bytes;
}

int32 PayloadHashesCompareBytes(TConstArrayView<uint8> A, TConstArrayView<uint8> B)
{
    const int32 Common = FMath::Min(A.Num(), B.Num());
    const int32 Compared = Common > 0 ? FMemory::Memcmp(A.GetData(), B.GetData(), Common) : 0;
    return Compared != 0 ? Compared : A.Num() - B.Num();
}

TSharedPtr<FPayloadHashJsonValue> PayloadHashesValue(const EPayloadHashJsonType Type)
{
    TSharedPtr<FPayloadHashJsonValue> Value = MakeShared<FPayloadHashJsonValue>();
    Value->Type = Type;
    return Value;
}

TSharedPtr<FPayloadHashJsonValue> PayloadHashesString(FString Text)
{
    TSharedPtr<FPayloadHashJsonValue> Value = PayloadHashesValue(EPayloadHashJsonType::String);
    Value->Text = MoveTemp(Text);
    return Value;
}

TSharedPtr<FPayloadHashJsonValue> PayloadHashesInteger(const int64 Number)
{
    TSharedPtr<FPayloadHashJsonValue> Value = PayloadHashesValue(EPayloadHashJsonType::Number);
    Value->Text = LexToString(Number);
    return Value;
}

bool PayloadHashesParseLosslessJson(
    const TConstArrayView<uint8> Bytes,
    TSharedPtr<FPayloadHashJsonValue>& OutRoot,
    FString& OutError)
{
    OutRoot.Reset();
    FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    FString Text(Converted.Length(), Converted.Get());
    const TArray<uint8> RoundTripBytes = PayloadHashesUtf8(Text);
    if (RoundTripBytes.Num() != Bytes.Num() ||
        (Bytes.Num() > 0 && FMemory::Memcmp(RoundTripBytes.GetData(), Bytes.GetData(), Bytes.Num()) != 0))
    {
        return PayloadHashesFail(OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: input is not valid UTF-8"));
    }

    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(MoveTemp(Text));
    TArray<TSharedPtr<FPayloadHashJsonValue>> Stack;
    auto Attach = [&OutRoot, &OutError, &Stack](
        const FString& Identifier,
        const TSharedPtr<FPayloadHashJsonValue>& Value) -> bool
    {
        if (Stack.IsEmpty())
        {
            if (OutRoot.IsValid())
            {
                return PayloadHashesFail(
                    OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: multiple root values"));
            }
            OutRoot = Value;
            return true;
        }
        const TSharedPtr<FPayloadHashJsonValue>& Parent = Stack.Last();
        if (Parent->Type == EPayloadHashJsonType::Array)
        {
            Parent->Array.Add(Value);
            return true;
        }
        if (Parent->Type == EPayloadHashJsonType::Object)
        {
            FPayloadHashJsonMember& Member = Parent->Object.AddDefaulted_GetRef();
            Member.Key = Identifier;
            Member.Value = Value;
            return true;
        }
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: invalid parser container"));
    };

    EJsonNotation Notation = EJsonNotation::Error;
    while (Reader->ReadNext(Notation))
    {
        if (Notation == EJsonNotation::Error)
        {
            return PayloadHashesFail(
                OutError,
                TEXT("MH_E_INVALID_CANONICAL_JSON: ") + Reader->GetErrorMessage());
        }

        TSharedPtr<FPayloadHashJsonValue> Value;
        switch (Notation)
        {
        case EJsonNotation::ObjectStart:
            Value = PayloadHashesValue(EPayloadHashJsonType::Object);
            if (!Attach(Reader->GetIdentifier(), Value))
            {
                return false;
            }
            Stack.Add(Value);
            break;
        case EJsonNotation::ObjectEnd:
            if (Stack.IsEmpty() || Stack.Last()->Type != EPayloadHashJsonType::Object)
            {
                return PayloadHashesFail(
                    OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: mismatched object end"));
            }
            Stack.Pop(EAllowShrinking::No);
            break;
        case EJsonNotation::ArrayStart:
            Value = PayloadHashesValue(EPayloadHashJsonType::Array);
            if (!Attach(Reader->GetIdentifier(), Value))
            {
                return false;
            }
            Stack.Add(Value);
            break;
        case EJsonNotation::ArrayEnd:
            if (Stack.IsEmpty() || Stack.Last()->Type != EPayloadHashJsonType::Array)
            {
                return PayloadHashesFail(
                    OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: mismatched array end"));
            }
            Stack.Pop(EAllowShrinking::No);
            break;
        case EJsonNotation::String:
            Value = PayloadHashesString(Reader->GetValueAsString());
            if (!Attach(Reader->GetIdentifier(), Value))
            {
                return false;
            }
            break;
        case EJsonNotation::Number:
            Value = PayloadHashesValue(EPayloadHashJsonType::Number);
            Value->Text = Reader->GetValueAsNumberString();
            if (!Attach(Reader->GetIdentifier(), Value))
            {
                return false;
            }
            break;
        case EJsonNotation::Boolean:
            Value = PayloadHashesValue(EPayloadHashJsonType::Boolean);
            Value->bBoolean = Reader->GetValueAsBoolean();
            if (!Attach(Reader->GetIdentifier(), Value))
            {
                return false;
            }
            break;
        case EJsonNotation::Null:
            Value = PayloadHashesValue(EPayloadHashJsonType::Null);
            if (!Attach(Reader->GetIdentifier(), Value))
            {
                return false;
            }
            break;
        default:
            return PayloadHashesFail(
                OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: unsupported parser token"));
        }
    }

    if (!Reader->GetErrorMessage().IsEmpty())
    {
        return PayloadHashesFail(
            OutError,
            TEXT("MH_E_INVALID_CANONICAL_JSON: ") + Reader->GetErrorMessage());
    }
    if (!Stack.IsEmpty() || !OutRoot.IsValid())
    {
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: incomplete JSON document"));
    }
    return true;
}

const TSharedPtr<FPayloadHashJsonValue>* PayloadHashesFindExactField(
    const TSharedPtr<FPayloadHashJsonValue>& Object,
    const TCHAR* Field)
{
    if (!Object.IsValid() || Object->Type != EPayloadHashJsonType::Object)
    {
        return nullptr;
    }
    for (const FPayloadHashJsonMember& Member : Object->Object)
    {
        if (Member.Key.Equals(Field, ESearchCase::CaseSensitive))
        {
            return &Member.Value;
        }
    }
    return nullptr;
}

bool PayloadHashesValidateNfcKeys(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    FString& OutError)
{
    if (!Value.IsValid())
    {
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: invalid JSON value"));
    }
    if (Value->Type == EPayloadHashJsonType::Array)
    {
        for (const TSharedPtr<FPayloadHashJsonValue>& Item : Value->Array)
        {
            if (!PayloadHashesValidateNfcKeys(Item, OutError))
            {
                return false;
            }
        }
        return true;
    }
    if (Value->Type != EPayloadHashJsonType::Object)
    {
        return true;
    }

    TArray<FString> NormalizedKeys;
    for (const FPayloadHashJsonMember& Member : Value->Object)
    {
        FString Key;
        const FMHCanonicalResult Result = MHNormalizeNfc(Member.Key, Key);
        if (!Result.bSuccess)
        {
            return PayloadHashesFail(OutError, Result.Error);
        }
        if (NormalizedKeys.ContainsByPredicate([&Key](const FString& Existing)
            {
                return Existing.Equals(Key, ESearchCase::CaseSensitive);
            }))
        {
            return PayloadHashesFail(
                OutError,
                FString::Printf(
                    TEXT("MH_E_INVALID_CANONICAL_JSON: keys collide after NFC normalization at %s"),
                    *Member.Key));
        }
        NormalizedKeys.Add(MoveTemp(Key));
        if (!PayloadHashesValidateNfcKeys(Member.Value, OutError))
        {
            return false;
        }
    }
    return true;
}

bool PayloadHashesAddCanonicalMember(
    TArray<FPayloadHashJsonMember>& OutMembers,
    const FString& SourceKey,
    TSharedPtr<FPayloadHashJsonValue> Value,
    FString& OutError)
{
    FString Key;
    const FMHCanonicalResult Normalized = MHNormalizeNfc(SourceKey, Key);
    if (!Normalized.bSuccess)
    {
        return PayloadHashesFail(OutError, Normalized.Error);
    }
    const FPayloadHashJsonMember* Existing = OutMembers.FindByPredicate(
        [&Key](const FPayloadHashJsonMember& Member)
        {
            return Member.Key.Equals(Key, ESearchCase::CaseSensitive);
        });
    if (Existing != nullptr)
    {
        return PayloadHashesFail(
            OutError,
            FString::Printf(
                TEXT("MH_E_INVALID_CANONICAL_JSON: keys collide after NFC normalization: %s and %s"),
                *Existing->Key,
                *SourceKey));
    }
    FPayloadHashJsonMember& Member = OutMembers.AddDefaulted_GetRef();
    Member.Key = MoveTemp(Key);
    Member.Value = MoveTemp(Value);
    return true;
}

bool PayloadHashesCanonGeneric(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    int32 Precision,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError);

bool PayloadHashesCanonObjectGeneric(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    const int32 Precision,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    OutValue = PayloadHashesValue(EPayloadHashJsonType::Object);
    for (const FPayloadHashJsonMember& Member : Value->Object)
    {
        TSharedPtr<FPayloadHashJsonValue> CanonicalValue;
        if (!PayloadHashesCanonGeneric(Member.Value, Precision, CanonicalValue, OutError) ||
            !PayloadHashesAddCanonicalMember(
                OutValue->Object, Member.Key, MoveTemp(CanonicalValue), OutError))
        {
            return false;
        }
    }
    return true;
}

bool PayloadHashesCanonGeneric(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    const int32 Precision,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    if (!Value.IsValid())
    {
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: invalid JSON value"));
    }
    switch (Value->Type)
    {
    case EPayloadHashJsonType::Null:
        OutValue = PayloadHashesValue(EPayloadHashJsonType::Null);
        return true;
    case EPayloadHashJsonType::Boolean:
        OutValue = PayloadHashesValue(EPayloadHashJsonType::Boolean);
        OutValue->bBoolean = Value->bBoolean;
        return true;
    case EPayloadHashJsonType::String:
    {
        FString Normalized;
        const FMHCanonicalResult Result = MHNormalizeNfc(Value->Text, Normalized);
        if (!Result.bSuccess)
        {
            return PayloadHashesFail(OutError, Result.Error);
        }
        OutValue = PayloadHashesString(MoveTemp(Normalized));
        return true;
    }
    case EPayloadHashJsonType::Number:
    {
        double Number = 0.0;
        if (!LexTryParseString(Number, *Value->Text) || !FMath::IsFinite(Number))
        {
            return PayloadHashesFail(
                OutError, TEXT("MH_E_NAN_INF_VALUE: invalid JSON number"));
        }
        int64 Quantized = 0;
        const FMHCanonicalResult Result = MHQuantize(Number, Precision, Quantized);
        if (!Result.bSuccess)
        {
            return PayloadHashesFail(OutError, Result.Error);
        }
        OutValue = PayloadHashesInteger(Quantized);
        return true;
    }
    case EPayloadHashJsonType::Array:
        OutValue = PayloadHashesValue(EPayloadHashJsonType::Array);
        for (const TSharedPtr<FPayloadHashJsonValue>& Item : Value->Array)
        {
            TSharedPtr<FPayloadHashJsonValue> CanonicalItem;
            if (!PayloadHashesCanonGeneric(Item, Precision, CanonicalItem, OutError))
            {
                return false;
            }
            OutValue->Array.Add(MoveTemp(CanonicalItem));
        }
        return true;
    case EPayloadHashJsonType::Object:
        return PayloadHashesCanonObjectGeneric(Value, Precision, OutValue, OutError);
    default:
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: unsupported JSON value"));
    }
}

bool PayloadHashesIsIntegerToken(const FString& Raw)
{
    return !Raw.Contains(TEXT(".")) && !Raw.Contains(TEXT("e")) && !Raw.Contains(TEXT("E"));
}

bool PayloadHashesCanonPassthroughInteger(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    if (Value.IsValid() && Value->Type == EPayloadHashJsonType::Number &&
        PayloadHashesIsIntegerToken(Value->Text))
    {
        int64 Integer = 0;
        if (LexTryParseString(Integer, *Value->Text))
        {
            OutValue = PayloadHashesInteger(Integer);
            return true;
        }
    }
    return PayloadHashesCanonGeneric(Value, 6, OutValue, OutError);
}

bool PayloadHashesCanonVector(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    const int32 Precision,
    const int32 ExpectedLength,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    if (!Value.IsValid() || Value->Type != EPayloadHashJsonType::Array ||
        Value->Array.Num() != ExpectedLength ||
        Value->Array.ContainsByPredicate([](const TSharedPtr<FPayloadHashJsonValue>& Item)
        {
            return !Item.IsValid() || Item->Type != EPayloadHashJsonType::Number;
        }))
    {
        return PayloadHashesCanonGeneric(Value, Precision, OutValue, OutError);
    }
    return PayloadHashesCanonGeneric(Value, Precision, OutValue, OutError);
}

bool PayloadHashesCanonRotation(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    if (!Value.IsValid() || Value->Type != EPayloadHashJsonType::Array || Value->Array.Num() != 4 ||
        Value->Array.ContainsByPredicate([](const TSharedPtr<FPayloadHashJsonValue>& Item)
        {
            return !Item.IsValid() || Item->Type != EPayloadHashJsonType::Number;
        }))
    {
        return PayloadHashesCanonGeneric(Value, 6, OutValue, OutError);
    }

    TArray<int64> Quaternion;
    for (const TSharedPtr<FPayloadHashJsonValue>& Item : Value->Array)
    {
        double Number = 0.0;
        if (!LexTryParseString(Number, *Item->Text) || !FMath::IsFinite(Number))
        {
            return PayloadHashesFail(OutError, TEXT("MH_E_NAN_INF_VALUE: invalid quaternion number"));
        }
        int64 Quantized = 0;
        const FMHCanonicalResult Result = MHQuantize(Number, 6, Quantized);
        if (!Result.bSuccess)
        {
            return PayloadHashesFail(OutError, Result.Error);
        }
        Quaternion.Add(Quantized);
    }
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
    OutValue = PayloadHashesValue(EPayloadHashJsonType::Array);
    for (const int64 Component : Quaternion)
    {
        OutValue->Array.Add(PayloadHashesInteger(bNegate ? -Component : Component));
    }
    return true;
}

bool PayloadHashesCanonLocalTransform(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    if (!Value.IsValid() || Value->Type != EPayloadHashJsonType::Object)
    {
        return PayloadHashesCanonGeneric(Value, 6, OutValue, OutError);
    }
    OutValue = PayloadHashesValue(EPayloadHashJsonType::Object);
    for (const FPayloadHashJsonMember& Member : Value->Object)
    {
        TSharedPtr<FPayloadHashJsonValue> CanonicalValue;
        bool bSuccess = false;
        if (Member.Key.Equals(TEXT("translation_cm"), ESearchCase::CaseSensitive))
        {
            bSuccess = PayloadHashesCanonVector(Member.Value, 3, 3, CanonicalValue, OutError);
        }
        else if (Member.Key.Equals(TEXT("rotation_quat"), ESearchCase::CaseSensitive))
        {
            bSuccess = PayloadHashesCanonRotation(Member.Value, CanonicalValue, OutError);
        }
        else if (Member.Key.Equals(TEXT("scale"), ESearchCase::CaseSensitive))
        {
            bSuccess = PayloadHashesCanonVector(Member.Value, 6, 3, CanonicalValue, OutError);
        }
        else
        {
            bSuccess = PayloadHashesCanonGeneric(Member.Value, 6, CanonicalValue, OutError);
        }
        if (!bSuccess || !PayloadHashesAddCanonicalMember(
                OutValue->Object, Member.Key, MoveTemp(CanonicalValue), OutError))
        {
            return false;
        }
    }
    return true;
}

bool PayloadHashesCanonVariant(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    if (!Value.IsValid() || Value->Type != EPayloadHashJsonType::Object)
    {
        return PayloadHashesCanonGeneric(Value, 6, OutValue, OutError);
    }
    OutValue = PayloadHashesValue(EPayloadHashJsonType::Object);
    for (const FPayloadHashJsonMember& Member : Value->Object)
    {
        TSharedPtr<FPayloadHashJsonValue> CanonicalValue;
        const int32 Precision = Member.Key.Equals(TEXT("weight"), ESearchCase::CaseSensitive) ? 4 : 6;
        if (!PayloadHashesCanonGeneric(Member.Value, Precision, CanonicalValue, OutError) ||
            !PayloadHashesAddCanonicalMember(
                OutValue->Object, Member.Key, MoveTemp(CanonicalValue), OutError))
        {
            return false;
        }
    }
    return true;
}

bool PayloadHashesCanonNode(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    if (!Value.IsValid() || Value->Type != EPayloadHashJsonType::Object)
    {
        return PayloadHashesCanonGeneric(Value, 6, OutValue, OutError);
    }
    OutValue = PayloadHashesValue(EPayloadHashJsonType::Object);
    for (const FPayloadHashJsonMember& Member : Value->Object)
    {
        TSharedPtr<FPayloadHashJsonValue> CanonicalValue;
        bool bSuccess = false;
        if (Member.Key.Equals(TEXT("local_transform"), ESearchCase::CaseSensitive))
        {
            bSuccess = PayloadHashesCanonLocalTransform(Member.Value, CanonicalValue, OutError);
        }
        else if (Member.Key.Equals(TEXT("variants"), ESearchCase::CaseSensitive) &&
                 Member.Value.IsValid() && Member.Value->Type == EPayloadHashJsonType::Array)
        {
            CanonicalValue = PayloadHashesValue(EPayloadHashJsonType::Array);
            bSuccess = true;
            for (const TSharedPtr<FPayloadHashJsonValue>& Variant : Member.Value->Array)
            {
                TSharedPtr<FPayloadHashJsonValue> CanonicalVariant;
                if (!PayloadHashesCanonVariant(Variant, CanonicalVariant, OutError))
                {
                    bSuccess = false;
                    break;
                }
                CanonicalValue->Array.Add(MoveTemp(CanonicalVariant));
            }
        }
        else if (Member.Key.Equals(TEXT("schema_version"), ESearchCase::CaseSensitive) ||
                 Member.Key.Equals(TEXT("seed_salt"), ESearchCase::CaseSensitive))
        {
            bSuccess = PayloadHashesCanonPassthroughInteger(Member.Value, CanonicalValue, OutError);
        }
        else
        {
            bSuccess = PayloadHashesCanonGeneric(Member.Value, 6, CanonicalValue, OutError);
        }
        if (!bSuccess || !PayloadHashesAddCanonicalMember(
                OutValue->Object, Member.Key, MoveTemp(CanonicalValue), OutError))
        {
            return false;
        }
    }
    return true;
}

bool PayloadHashesCanonComposite(
    const TSharedPtr<FPayloadHashJsonValue>& Root,
    TSharedPtr<FPayloadHashJsonValue>& OutValue,
    FString& OutError)
{
    if (!Root.IsValid() || Root->Type != EPayloadHashJsonType::Object)
    {
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_COMPOSITE: document must be an object"));
    }
    OutValue = PayloadHashesValue(EPayloadHashJsonType::Object);
    for (const FPayloadHashJsonMember& Member : Root->Object)
    {
        TSharedPtr<FPayloadHashJsonValue> CanonicalValue;
        bool bSuccess = false;
        if (Member.Key.Equals(TEXT("nodes"), ESearchCase::CaseSensitive) &&
            Member.Value.IsValid() && Member.Value->Type == EPayloadHashJsonType::Array)
        {
            struct FCanonicalNode
            {
                TArray<uint8> SortKey;
                TSharedPtr<FPayloadHashJsonValue> Value;
            };
            TArray<FCanonicalNode> Nodes;
            for (const TSharedPtr<FPayloadHashJsonValue>& Node : Member.Value->Array)
            {
                FCanonicalNode& Row = Nodes.AddDefaulted_GetRef();
                if (!PayloadHashesCanonNode(Node, Row.Value, OutError))
                {
                    return false;
                }
                const TSharedPtr<FPayloadHashJsonValue>* Uid =
                    PayloadHashesFindExactField(Row.Value, TEXT("node_uid"));
                if (Uid != nullptr && Uid->IsValid() &&
                    (*Uid)->Type == EPayloadHashJsonType::String)
                {
                    Row.SortKey = PayloadHashesUtf8((*Uid)->Text);
                }
            }
            Nodes.Sort([](const FCanonicalNode& A, const FCanonicalNode& B)
            {
                return PayloadHashesCompareBytes(A.SortKey, B.SortKey) < 0;
            });
            CanonicalValue = PayloadHashesValue(EPayloadHashJsonType::Array);
            for (FCanonicalNode& Node : Nodes)
            {
                CanonicalValue->Array.Add(MoveTemp(Node.Value));
            }
            bSuccess = true;
        }
        else if (Member.Key.Equals(TEXT("schema_version"), ESearchCase::CaseSensitive) ||
                 Member.Key.Equals(TEXT("seed_salt"), ESearchCase::CaseSensitive))
        {
            bSuccess = PayloadHashesCanonPassthroughInteger(Member.Value, CanonicalValue, OutError);
        }
        else
        {
            bSuccess = PayloadHashesCanonGeneric(Member.Value, 6, CanonicalValue, OutError);
        }
        if (!bSuccess || !PayloadHashesAddCanonicalMember(
                OutValue->Object, Member.Key, MoveTemp(CanonicalValue), OutError))
        {
            return false;
        }
    }
    return true;
}

void PayloadHashesAppendAnsi(TArray<uint8>& Out, const ANSICHAR* Text)
{
    Out.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
}

bool PayloadHashesAppendString(const FString& Input, TArray<uint8>& Out, FString& OutError)
{
    FString Normalized;
    const FMHCanonicalResult Result = MHNormalizeNfc(Input, Normalized);
    if (!Result.bSuccess)
    {
        return PayloadHashesFail(OutError, Result.Error);
    }
    const TArray<uint8> Utf8 = PayloadHashesUtf8(Normalized);
    Out.Add(static_cast<uint8>('"'));
    for (const uint8 Byte : Utf8)
    {
        if (Byte == static_cast<uint8>('"'))
        {
            PayloadHashesAppendAnsi(Out, "\\\"");
        }
        else if (Byte == static_cast<uint8>('\\'))
        {
            PayloadHashesAppendAnsi(Out, "\\\\");
        }
        else if (Byte < 0x20)
        {
            static constexpr ANSICHAR Hex[] = "0123456789abcdef";
            Out.Add(static_cast<uint8>('\\'));
            Out.Add(static_cast<uint8>('u'));
            Out.Add(static_cast<uint8>('0'));
            Out.Add(static_cast<uint8>('0'));
            Out.Add(static_cast<uint8>(Hex[Byte >> 4]));
            Out.Add(static_cast<uint8>(Hex[Byte & 0x0f]));
        }
        else
        {
            Out.Add(Byte);
        }
    }
    Out.Add(static_cast<uint8>('"'));
    return true;
}

bool PayloadHashesAppendCanonical(
    const TSharedPtr<FPayloadHashJsonValue>& Value,
    TArray<uint8>& Out,
    FString& OutError)
{
    if (!Value.IsValid())
    {
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: invalid canonical value"));
    }
    switch (Value->Type)
    {
    case EPayloadHashJsonType::Null:
        PayloadHashesAppendAnsi(Out, "null");
        return true;
    case EPayloadHashJsonType::Boolean:
        PayloadHashesAppendAnsi(Out, Value->bBoolean ? "true" : "false");
        return true;
    case EPayloadHashJsonType::String:
        return PayloadHashesAppendString(Value->Text, Out, OutError);
    case EPayloadHashJsonType::Number:
        Out.Append(PayloadHashesUtf8(Value->Text));
        return true;
    case EPayloadHashJsonType::Array:
        Out.Add(static_cast<uint8>('['));
        for (int32 Index = 0; Index < Value->Array.Num(); ++Index)
        {
            if (Index > 0)
            {
                Out.Add(static_cast<uint8>(','));
            }
            if (!PayloadHashesAppendCanonical(Value->Array[Index], Out, OutError))
            {
                return false;
            }
        }
        Out.Add(static_cast<uint8>(']'));
        return true;
    case EPayloadHashJsonType::Object:
    {
        struct FEntry
        {
            TArray<uint8> KeyBytes;
            const FPayloadHashJsonMember* Member = nullptr;
        };
        TArray<FEntry> Entries;
        for (const FPayloadHashJsonMember& Member : Value->Object)
        {
            FEntry& Entry = Entries.AddDefaulted_GetRef();
            Entry.KeyBytes = PayloadHashesUtf8(Member.Key);
            Entry.Member = &Member;
        }
        Entries.Sort([](const FEntry& A, const FEntry& B)
        {
            return PayloadHashesCompareBytes(A.KeyBytes, B.KeyBytes) < 0;
        });
        Out.Add(static_cast<uint8>('{'));
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            if (Index > 0)
            {
                Out.Add(static_cast<uint8>(','));
            }
            if (!PayloadHashesAppendString(Entries[Index].Member->Key, Out, OutError))
            {
                return false;
            }
            Out.Add(static_cast<uint8>(':'));
            if (!PayloadHashesAppendCanonical(Entries[Index].Member->Value, Out, OutError))
            {
                return false;
            }
        }
        Out.Add(static_cast<uint8>('}'));
        return true;
    }
    default:
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_CANONICAL_JSON: unsupported canonical value"));
    }
}

bool PayloadHashesHashCanonical(
    const TSharedPtr<FPayloadHashJsonValue>& Canonical,
    FString& OutHash,
    FString& OutError)
{
    TArray<uint8> CanonicalBytes;
    if (!PayloadHashesAppendCanonical(Canonical, CanonicalBytes, OutError))
    {
        return false;
    }
    OutHash = MHXxh3Hash(CanonicalBytes);
    return true;
}

uint32 PayloadHashesRotateRight(const uint32 Value, const uint32 Shift)
{
    return (Value >> Shift) | (Value << (32 - Shift));
}

struct FPayloadHashesSha256
{
    uint32 State[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8 Buffer[64] = {};
    int32 Buffered = 0;
    uint64 TotalBytes = 0;

    void Transform(const uint8* Block)
    {
        static constexpr uint32 K[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        uint32 Words[64];
        for (int32 Index = 0; Index < 16; ++Index)
        {
            const int32 Offset = Index * 4;
            Words[Index] =
                (static_cast<uint32>(Block[Offset]) << 24) |
                (static_cast<uint32>(Block[Offset + 1]) << 16) |
                (static_cast<uint32>(Block[Offset + 2]) << 8) |
                static_cast<uint32>(Block[Offset + 3]);
        }
        for (int32 Index = 16; Index < 64; ++Index)
        {
            const uint32 S0 = PayloadHashesRotateRight(Words[Index - 15], 7) ^
                PayloadHashesRotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
            const uint32 S1 = PayloadHashesRotateRight(Words[Index - 2], 17) ^
                PayloadHashesRotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
            Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
        }

        uint32 A = State[0];
        uint32 B = State[1];
        uint32 C = State[2];
        uint32 D = State[3];
        uint32 E = State[4];
        uint32 F = State[5];
        uint32 G = State[6];
        uint32 H = State[7];
        for (int32 Index = 0; Index < 64; ++Index)
        {
            const uint32 S1 = PayloadHashesRotateRight(E, 6) ^
                PayloadHashesRotateRight(E, 11) ^ PayloadHashesRotateRight(E, 25);
            const uint32 Choice = (E & F) ^ (~E & G);
            const uint32 Temp1 = H + S1 + Choice + K[Index] + Words[Index];
            const uint32 S0 = PayloadHashesRotateRight(A, 2) ^
                PayloadHashesRotateRight(A, 13) ^ PayloadHashesRotateRight(A, 22);
            const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
            const uint32 Temp2 = S0 + Majority;
            H = G;
            G = F;
            F = E;
            E = D + Temp1;
            D = C;
            C = B;
            B = A;
            A = Temp1 + Temp2;
        }
        State[0] += A;
        State[1] += B;
        State[2] += C;
        State[3] += D;
        State[4] += E;
        State[5] += F;
        State[6] += G;
        State[7] += H;
    }

    void Update(TConstArrayView<uint8> Bytes)
    {
        TotalBytes += static_cast<uint64>(Bytes.Num());
        int32 Offset = 0;
        if (Buffered > 0)
        {
            const int32 Copied = FMath::Min(64 - Buffered, Bytes.Num());
            FMemory::Memcpy(Buffer + Buffered, Bytes.GetData(), Copied);
            Buffered += Copied;
            Offset += Copied;
            if (Buffered == 64)
            {
                Transform(Buffer);
                Buffered = 0;
            }
        }
        while (Offset + 64 <= Bytes.Num())
        {
            Transform(Bytes.GetData() + Offset);
            Offset += 64;
        }
        if (Offset < Bytes.Num())
        {
            Buffered = Bytes.Num() - Offset;
            FMemory::Memcpy(Buffer, Bytes.GetData() + Offset, Buffered);
        }
    }

    void Final(uint8 (&OutDigest)[32])
    {
        const uint64 TotalBits = TotalBytes * 8;
        Buffer[Buffered++] = 0x80;
        if (Buffered > 56)
        {
            FMemory::Memzero(Buffer + Buffered, 64 - Buffered);
            Transform(Buffer);
            Buffered = 0;
        }
        FMemory::Memzero(Buffer + Buffered, 56 - Buffered);
        for (int32 Index = 0; Index < 8; ++Index)
        {
            Buffer[56 + Index] = static_cast<uint8>(TotalBits >> (56 - Index * 8));
        }
        Transform(Buffer);
        for (int32 Index = 0; Index < 8; ++Index)
        {
            OutDigest[Index * 4] = static_cast<uint8>(State[Index] >> 24);
            OutDigest[Index * 4 + 1] = static_cast<uint8>(State[Index] >> 16);
            OutDigest[Index * 4 + 2] = static_cast<uint8>(State[Index] >> 8);
            OutDigest[Index * 4 + 3] = static_cast<uint8>(State[Index]);
        }
    }
};

FString PayloadHashesSha256(TConstArrayView<uint8> Bytes)
{
    FPayloadHashesSha256 Sha256;
    Sha256.Update(Bytes);
    uint8 Digest[32];
    Sha256.Final(Digest);
    FString Hex;
    Hex.Reserve(64);
    static constexpr TCHAR Digits[] = TEXT("0123456789abcdef");
    for (const uint8 Byte : Digest)
    {
        Hex.AppendChar(Digits[Byte >> 4]);
        Hex.AppendChar(Digits[Byte & 0x0f]);
    }
    return TEXT("sha256:") + Hex;
}

} // namespace

bool MHCompositeSemanticHash(
    const TConstArrayView<uint8> Bytes,
    FString& OutHash,
    FString& OutError)
{
    OutHash.Reset();
    OutError.Reset();

    FMHCompositeDocument Document;
    FMHCanonicalResult Result = MHParseCompositeV2(Bytes, Document);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }

    TSharedPtr<FPayloadHashJsonValue> Root;
    if (!PayloadHashesParseLosslessJson(Bytes, Root, OutError))
    {
        return false;
    }
    TSharedPtr<FPayloadHashJsonValue> Canonical;
    if (!PayloadHashesCanonComposite(Root, Canonical, OutError))
    {
        return false;
    }
    return PayloadHashesHashCanonical(Canonical, OutHash, OutError);
}

bool MHMaterialSemanticHash(
    const TConstArrayView<uint8> Bytes,
    FString& OutHash,
    FString& OutError)
{
    OutHash.Reset();
    OutError.Reset();

    FMHMaterialDocument Document;
    FMHCanonicalResult Result = MHParseMaterialV1(Bytes, Document);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }

    TSharedPtr<FPayloadHashJsonValue> Root;
    if (!PayloadHashesParseLosslessJson(Bytes, Root, OutError))
    {
        return false;
    }
    if (!Root.IsValid() || Root->Type != EPayloadHashJsonType::Object)
    {
        OutError = TEXT("MH_E_INVALID_MATERIAL_VALUE: document must be an object");
        return false;
    }
    if (!PayloadHashesValidateNfcKeys(Root, OutError))
    {
        return false;
    }

    const TSharedPtr<FPayloadHashJsonValue>* Params =
        PayloadHashesFindExactField(Root, TEXT("params"));
    const TSharedPtr<FPayloadHashJsonValue>* Textures =
        PayloadHashesFindExactField(Root, TEXT("textures"));
    if (Params == nullptr || Textures == nullptr)
    {
        return PayloadHashesFail(
            OutError, TEXT("MH_E_INVALID_MATERIAL_VALUE: params and textures are required"));
    }

    TSharedPtr<FPayloadHashJsonValue> CanonicalParams;
    TSharedPtr<FPayloadHashJsonValue> CanonicalTextures;
    if (!PayloadHashesCanonGeneric(*Params, 6, CanonicalParams, OutError) ||
        !PayloadHashesCanonGeneric(*Textures, 6, CanonicalTextures, OutError))
    {
        return false;
    }
    FString ShaderClass;
    Result = MHNormalizeNfc(Document.ShaderClass, ShaderClass);
    if (!Result.bSuccess)
    {
        return PayloadHashesFail(OutError, Result.Error);
    }

    TSharedPtr<FPayloadHashJsonValue> Canonical =
        PayloadHashesValue(EPayloadHashJsonType::Object);
    if (!PayloadHashesAddCanonicalMember(
            Canonical->Object,
            TEXT("shader_class"),
            PayloadHashesString(MoveTemp(ShaderClass)),
            OutError) ||
        !PayloadHashesAddCanonicalMember(
            Canonical->Object, TEXT("params"), MoveTemp(CanonicalParams), OutError) ||
        !PayloadHashesAddCanonicalMember(
            Canonical->Object, TEXT("textures"), MoveTemp(CanonicalTextures), OutError))
    {
        return false;
    }
    return PayloadHashesHashCanonical(Canonical, OutHash, OutError);
}

FString MHPassportDescriptorHash(const FMHFbxPassport& Passport)
{
    TArray<uint8> DescriptorBytes;
    FString Error;
    if (!MHBuildFbxPassportDescriptorBytes(Passport, DescriptorBytes, Error))
    {
        return FString();
    }
    return PayloadHashesSha256(DescriptorBytes);
}

FString MHPayloadFingerprint(const TConstArrayView<uint8> Bytes)
{
    return PayloadHashesSha256(Bytes);
}

} // namespace UE::MimirComposite
