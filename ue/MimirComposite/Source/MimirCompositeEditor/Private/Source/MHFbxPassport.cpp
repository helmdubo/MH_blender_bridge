#include "Source/MHFbxPassport.h"

#include "Canonical/MHCanonical.h"
#include "Codec/MHCompositeCodec.h"
#include "Containers/StringConv.h"
#include "Math/UnrealMathUtility.h"

#include <charconv>
#include <system_error>

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace UE::MimirComposite
{
namespace
{

bool Fail(FString& OutError, const FString& Message)
{
    OutError = FString::Printf(TEXT("MH_E_PASSPORT_INVALID: %s"), *Message);
    return false;
}

TArray<uint8> PassportUtf8(const FString& Text)
{
    FTCHARToUTF8 Utf8(*Text, Text.Len());
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return Bytes;
}

FString PassportUtf8Text(TConstArrayView<uint8> Bytes)
{
    FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    return FString(Text.Length(), Text.Get());
}

void AppendAnsi(TArray<uint8>& Out, const ANSICHAR* Text)
{
    Out.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
}

int32 CompareBytes(TConstArrayView<uint8> A, TConstArrayView<uint8> B)
{
    const int32 Common = FMath::Min(A.Num(), B.Num());
    const int32 Compared = Common > 0 ? FMemory::Memcmp(A.GetData(), B.GetData(), Common) : 0;
    return Compared != 0 ? Compared : A.Num() - B.Num();
}

bool NormalizeNfc(const FString& Input, FString& OutNormalized, FString& OutError)
{
    const FMHCanonicalResult Result = MHNormalizeNfc(Input, OutNormalized);
    return Result.bSuccess || Fail(OutError, Result.Error);
}

enum class EPassportJsonType : uint8
{
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

struct FPassportJsonValue;

struct FPassportJsonMember
{
    FString Key;
    TSharedPtr<FPassportJsonValue> Value;
};

struct FPassportJsonValue
{
    EPassportJsonType Type = EPassportJsonType::Null;
    bool bBoolean = false;
    FString Text;
    TArray<TSharedPtr<FPassportJsonValue>> Array;
    TArray<FPassportJsonMember> Object;
};

TSharedPtr<FPassportJsonValue> PassportString(FString Text)
{
    TSharedPtr<FPassportJsonValue> Value = MakeShared<FPassportJsonValue>();
    Value->Type = EPassportJsonType::String;
    Value->Text = MoveTemp(Text);
    return Value;
}

TSharedPtr<FPassportJsonValue> PassportNumber(FString Text)
{
    TSharedPtr<FPassportJsonValue> Value = MakeShared<FPassportJsonValue>();
    Value->Type = EPassportJsonType::Number;
    Value->Text = MoveTemp(Text);
    return Value;
}

TSharedPtr<FPassportJsonValue> PassportArray(TArray<TSharedPtr<FPassportJsonValue>> Items)
{
    TSharedPtr<FPassportJsonValue> Value = MakeShared<FPassportJsonValue>();
    Value->Type = EPassportJsonType::Array;
    Value->Array = MoveTemp(Items);
    return Value;
}

TSharedPtr<FPassportJsonValue> PassportObject(TArray<FPassportJsonMember> Members)
{
    TSharedPtr<FPassportJsonValue> Value = MakeShared<FPassportJsonValue>();
    Value->Type = EPassportJsonType::Object;
    Value->Object = MoveTemp(Members);
    return Value;
}

class FPassportJsonParser
{
public:
    FPassportJsonParser(const FString& InText, FString& InError)
        : Text(InText)
        , Error(InError)
    {
    }

    bool Parse(TSharedPtr<FPassportJsonValue>& OutValue)
    {
        SkipWhitespace();
        if (!ParseValue(OutValue, 0))
        {
            return false;
        }
        SkipWhitespace();
        return Index == Text.Len() || ParserFail(TEXT("trailing data after JSON value"));
    }

private:
    bool ParserFail(const FString& Message)
    {
        return Fail(Error, FString::Printf(TEXT("JSON parse failed at %d: %s"), Index, *Message));
    }

    void SkipWhitespace()
    {
        while (Index < Text.Len() &&
            (Text[Index] == TEXT(' ') || Text[Index] == TEXT('\t') ||
             Text[Index] == TEXT('\r') || Text[Index] == TEXT('\n')))
        {
            ++Index;
        }
    }

    bool Consume(const TCHAR Expected)
    {
        if (Index >= Text.Len() || Text[Index] != Expected)
        {
            return false;
        }
        ++Index;
        return true;
    }

    bool ConsumeLiteral(const TCHAR* Literal)
    {
        const int32 Length = FCString::Strlen(Literal);
        if (Index + Length > Text.Len() ||
            !Text.Mid(Index, Length).Equals(Literal, ESearchCase::CaseSensitive))
        {
            return false;
        }
        Index += Length;
        return true;
    }

    static int32 HexDigit(const TCHAR Char)
    {
        if (Char >= TEXT('0') && Char <= TEXT('9'))
        {
            return Char - TEXT('0');
        }
        if (Char >= TEXT('a') && Char <= TEXT('f'))
        {
            return Char - TEXT('a') + 10;
        }
        if (Char >= TEXT('A') && Char <= TEXT('F'))
        {
            return Char - TEXT('A') + 10;
        }
        return -1;
    }

    bool ParseHexCodeUnit(uint16& OutCodeUnit)
    {
        if (Index + 4 > Text.Len())
        {
            return ParserFail(TEXT("short Unicode escape"));
        }
        uint16 CodeUnit = 0;
        for (int32 DigitIndex = 0; DigitIndex < 4; ++DigitIndex)
        {
            const int32 Digit = HexDigit(Text[Index++]);
            if (Digit < 0)
            {
                return ParserFail(TEXT("invalid Unicode escape"));
            }
            CodeUnit = static_cast<uint16>((CodeUnit << 4) | Digit);
        }
        OutCodeUnit = CodeUnit;
        return true;
    }

    bool AppendEscapedCodeUnit(FString& OutString)
    {
        uint16 First = 0;
        if (!ParseHexCodeUnit(First))
        {
            return false;
        }
        if (First >= 0xd800 && First <= 0xdbff)
        {
            if (Index + 2 > Text.Len() || Text[Index] != TEXT('\\') || Text[Index + 1] != TEXT('u'))
            {
                return ParserFail(TEXT("high surrogate is not followed by a low surrogate"));
            }
            Index += 2;
            uint16 Second = 0;
            if (!ParseHexCodeUnit(Second))
            {
                return false;
            }
            if (Second < 0xdc00 || Second > 0xdfff)
            {
                return ParserFail(TEXT("invalid low surrogate"));
            }
            OutString.AppendChar(static_cast<TCHAR>(First));
            OutString.AppendChar(static_cast<TCHAR>(Second));
            return true;
        }
        if (First >= 0xdc00 && First <= 0xdfff)
        {
            return ParserFail(TEXT("unpaired low surrogate"));
        }
        OutString.AppendChar(static_cast<TCHAR>(First));
        return true;
    }

    bool ParseString(FString& OutString)
    {
        if (!Consume(TEXT('"')))
        {
            return ParserFail(TEXT("expected string"));
        }
        OutString.Reset();
        while (Index < Text.Len())
        {
            const TCHAR Char = Text[Index++];
            if (Char == TEXT('"'))
            {
                return true;
            }
            if (Char < 0x20)
            {
                return ParserFail(TEXT("unescaped control character"));
            }
            if (Char != TEXT('\\'))
            {
                if (Char >= 0xd800 && Char <= 0xdbff)
                {
                    if (Index >= Text.Len() || Text[Index] < 0xdc00 || Text[Index] > 0xdfff)
                    {
                        return ParserFail(TEXT("unpaired high surrogate"));
                    }
                    OutString.AppendChar(Char);
                    OutString.AppendChar(Text[Index++]);
                }
                else if (Char >= 0xdc00 && Char <= 0xdfff)
                {
                    return ParserFail(TEXT("unpaired low surrogate"));
                }
                else
                {
                    OutString.AppendChar(Char);
                }
                continue;
            }
            if (Index >= Text.Len())
            {
                return ParserFail(TEXT("short escape"));
            }
            const TCHAR Escape = Text[Index++];
            switch (Escape)
            {
            case TEXT('"'):
            case TEXT('\\'):
            case TEXT('/'):
                OutString.AppendChar(Escape);
                break;
            case TEXT('b'):
                OutString.AppendChar(TEXT('\b'));
                break;
            case TEXT('f'):
                OutString.AppendChar(TEXT('\f'));
                break;
            case TEXT('n'):
                OutString.AppendChar(TEXT('\n'));
                break;
            case TEXT('r'):
                OutString.AppendChar(TEXT('\r'));
                break;
            case TEXT('t'):
                OutString.AppendChar(TEXT('\t'));
                break;
            case TEXT('u'):
                if (!AppendEscapedCodeUnit(OutString))
                {
                    return false;
                }
                break;
            default:
                return ParserFail(TEXT("unknown escape"));
            }
        }
        return ParserFail(TEXT("unterminated string"));
    }

    bool ParseNumber(TSharedPtr<FPassportJsonValue>& OutValue)
    {
        const int32 Start = Index;
        if (Index < Text.Len() && Text[Index] == TEXT('-'))
        {
            ++Index;
        }
        if (Index >= Text.Len())
        {
            return ParserFail(TEXT("short number"));
        }
        if (Text[Index] == TEXT('0'))
        {
            ++Index;
        }
        else if (Text[Index] >= TEXT('1') && Text[Index] <= TEXT('9'))
        {
            do
            {
                ++Index;
            } while (Index < Text.Len() && Text[Index] >= TEXT('0') && Text[Index] <= TEXT('9'));
        }
        else
        {
            return ParserFail(TEXT("invalid integer part"));
        }
        if (Index < Text.Len() && Text[Index] == TEXT('.'))
        {
            ++Index;
            const int32 FractionStart = Index;
            while (Index < Text.Len() && Text[Index] >= TEXT('0') && Text[Index] <= TEXT('9'))
            {
                ++Index;
            }
            if (Index == FractionStart)
            {
                return ParserFail(TEXT("fraction has no digits"));
            }
        }
        if (Index < Text.Len() && (Text[Index] == TEXT('e') || Text[Index] == TEXT('E')))
        {
            ++Index;
            if (Index < Text.Len() && (Text[Index] == TEXT('+') || Text[Index] == TEXT('-')))
            {
                ++Index;
            }
            const int32 ExponentStart = Index;
            while (Index < Text.Len() && Text[Index] >= TEXT('0') && Text[Index] <= TEXT('9'))
            {
                ++Index;
            }
            if (Index == ExponentStart)
            {
                return ParserFail(TEXT("exponent has no digits"));
            }
        }
        OutValue = PassportNumber(Text.Mid(Start, Index - Start));
        return true;
    }

    bool ParseArrayValue(TSharedPtr<FPassportJsonValue>& OutValue, const int32 Depth)
    {
        Consume(TEXT('['));
        SkipWhitespace();
        TArray<TSharedPtr<FPassportJsonValue>> Items;
        if (Consume(TEXT(']')))
        {
            OutValue = PassportArray(MoveTemp(Items));
            return true;
        }
        for (;;)
        {
            TSharedPtr<FPassportJsonValue> Item;
            if (!ParseValue(Item, Depth + 1))
            {
                return false;
            }
            Items.Add(MoveTemp(Item));
            SkipWhitespace();
            if (Consume(TEXT(']')))
            {
                OutValue = PassportArray(MoveTemp(Items));
                return true;
            }
            if (!Consume(TEXT(',')))
            {
                return ParserFail(TEXT("expected array comma"));
            }
            SkipWhitespace();
        }
    }

    bool ParseObjectValue(TSharedPtr<FPassportJsonValue>& OutValue, const int32 Depth)
    {
        Consume(TEXT('{'));
        SkipWhitespace();
        TArray<FPassportJsonMember> Members;
        if (Consume(TEXT('}')))
        {
            OutValue = PassportObject(MoveTemp(Members));
            return true;
        }
        for (;;)
        {
            FPassportJsonMember& Member = Members.AddDefaulted_GetRef();
            if (!ParseString(Member.Key))
            {
                return false;
            }
            SkipWhitespace();
            if (!Consume(TEXT(':')))
            {
                return ParserFail(TEXT("expected object colon"));
            }
            SkipWhitespace();
            if (!ParseValue(Member.Value, Depth + 1))
            {
                return false;
            }
            SkipWhitespace();
            if (Consume(TEXT('}')))
            {
                OutValue = PassportObject(MoveTemp(Members));
                return true;
            }
            if (!Consume(TEXT(',')))
            {
                return ParserFail(TEXT("expected object comma"));
            }
            SkipWhitespace();
        }
    }

    bool ParseValue(TSharedPtr<FPassportJsonValue>& OutValue, const int32 Depth)
    {
        if (Depth > 128)
        {
            return ParserFail(TEXT("JSON nesting is too deep"));
        }
        if (Index >= Text.Len())
        {
            return ParserFail(TEXT("expected value"));
        }
        if (Text[Index] == TEXT('"'))
        {
            FString String;
            if (!ParseString(String))
            {
                return false;
            }
            OutValue = PassportString(MoveTemp(String));
            return true;
        }
        if (Text[Index] == TEXT('{'))
        {
            return ParseObjectValue(OutValue, Depth);
        }
        if (Text[Index] == TEXT('['))
        {
            return ParseArrayValue(OutValue, Depth);
        }
        if (Text[Index] == TEXT('-') || (Text[Index] >= TEXT('0') && Text[Index] <= TEXT('9')))
        {
            return ParseNumber(OutValue);
        }
        if (ConsumeLiteral(TEXT("true")))
        {
            OutValue = MakeShared<FPassportJsonValue>();
            OutValue->Type = EPassportJsonType::Boolean;
            OutValue->bBoolean = true;
            return true;
        }
        if (ConsumeLiteral(TEXT("false")))
        {
            OutValue = MakeShared<FPassportJsonValue>();
            OutValue->Type = EPassportJsonType::Boolean;
            return true;
        }
        if (ConsumeLiteral(TEXT("null")))
        {
            OutValue = MakeShared<FPassportJsonValue>();
            return true;
        }
        return ParserFail(TEXT("unknown value"));
    }

    const FString& Text;
    FString& Error;
    int32 Index = 0;
};

bool AppendPythonString(const FString& Input, TArray<uint8>& Out, FString& OutError)
{
    FString Normalized;
    if (!NormalizeNfc(Input, Normalized, OutError))
    {
        return false;
    }
    const TArray<uint8> Utf8 = PassportUtf8(Normalized);
    Out.Add(static_cast<uint8>('"'));
    for (const uint8 Byte : Utf8)
    {
        switch (Byte)
        {
        case '"': AppendAnsi(Out, "\\\""); break;
        case '\\': AppendAnsi(Out, "\\\\"); break;
        case '\b': AppendAnsi(Out, "\\b"); break;
        case '\f': AppendAnsi(Out, "\\f"); break;
        case '\n': AppendAnsi(Out, "\\n"); break;
        case '\r': AppendAnsi(Out, "\\r"); break;
        case '\t': AppendAnsi(Out, "\\t"); break;
        default:
            if (Byte < 0x20)
            {
                static constexpr ANSICHAR Hex[] = "0123456789abcdef";
                Out.Append({
                    static_cast<uint8>('\\'), static_cast<uint8>('u'),
                    static_cast<uint8>('0'), static_cast<uint8>('0'),
                    static_cast<uint8>(Hex[Byte >> 4]), static_cast<uint8>(Hex[Byte & 0x0f])});
            }
            else
            {
                Out.Add(Byte);
            }
            break;
        }
    }
    Out.Add(static_cast<uint8>('"'));
    return true;
}

bool IsIntegerToken(const FString& Raw)
{
    return !Raw.Contains(TEXT(".")) && !Raw.Contains(TEXT("e")) && !Raw.Contains(TEXT("E"));
}

bool PythonFloatText(const double Value, FString& OutText)
{
    if (!FMath::IsFinite(Value))
    {
        return false;
    }
    ANSICHAR Buffer[128];
    const std::to_chars_result Result = std::to_chars(
        Buffer, Buffer + UE_ARRAY_COUNT(Buffer) - 1, Value, std::chars_format::scientific);
    if (Result.ec != std::errc())
    {
        return false;
    }
    *Result.ptr = '\0';

    const ANSICHAR* Cursor = Buffer;
    const bool bNegative = Cursor < Result.ptr && *Cursor == '-';
    if (bNegative)
    {
        ++Cursor;
    }
    const ANSICHAR* ExponentMarker = Cursor;
    while (ExponentMarker < Result.ptr && *ExponentMarker != 'e')
    {
        ++ExponentMarker;
    }
    if (ExponentMarker == Result.ptr)
    {
        return false;
    }
    FString Digits;
    for (const ANSICHAR* Digit = Cursor; Digit < ExponentMarker; ++Digit)
    {
        if (*Digit != '.')
        {
            Digits.AppendChar(static_cast<TCHAR>(*Digit));
        }
    }
    if (Digits.IsEmpty())
    {
        return false;
    }

    const int32 Exponent = FCStringAnsi::Atoi(ExponentMarker + 1);
    const FString Sign = bNegative ? TEXT("-") : FString();
    if (Exponent >= -4 && Exponent < 16)
    {
        const int32 DecimalPosition = Exponent + 1;
        if (DecimalPosition <= 0)
        {
            OutText = Sign + TEXT("0.") + FString::ChrN(-DecimalPosition, TEXT('0')) + Digits;
        }
        else if (DecimalPosition >= Digits.Len())
        {
            OutText = Sign + Digits + FString::ChrN(DecimalPosition - Digits.Len(), TEXT('0')) + TEXT(".0");
        }
        else
        {
            OutText = Sign + Digits.Left(DecimalPosition) + TEXT(".") + Digits.Mid(DecimalPosition);
        }
        return true;
    }

    OutText = Sign + Digits.Left(1);
    if (Digits.Len() > 1)
    {
        OutText += TEXT(".") + Digits.Mid(1);
    }
    FString ExponentDigits = LexToString(FMath::Abs(Exponent));
    while (ExponentDigits.Len() < 2)
    {
        ExponentDigits = TEXT("0") + ExponentDigits;
    }
    OutText += FString::Printf(TEXT("e%c%s"), Exponent < 0 ? TEXT('-') : TEXT('+'), *ExponentDigits);
    return true;
}

bool AppendPythonJson(
    const TSharedPtr<FPassportJsonValue>& Value,
    TArray<uint8>& Out,
    FString& OutError)
{
    if (!Value.IsValid())
    {
        return Fail(OutError, TEXT("invalid JSON value"));
    }
    switch (Value->Type)
    {
    case EPassportJsonType::Null:
        AppendAnsi(Out, "null");
        return true;
    case EPassportJsonType::Boolean:
        AppendAnsi(Out, Value->bBoolean ? "true" : "false");
        return true;
    case EPassportJsonType::String:
        return AppendPythonString(Value->Text, Out, OutError);
    case EPassportJsonType::Number:
    {
        FString CanonicalNumber;
        if (IsIntegerToken(Value->Text))
        {
            CanonicalNumber = Value->Text == TEXT("-0") ? TEXT("0") : Value->Text;
        }
        else
        {
            double Number = 0.0;
            if (!LexTryParseString(Number, *Value->Text) || !PythonFloatText(Number, CanonicalNumber))
            {
                return Fail(OutError, TEXT("properties contains NaN or infinity"));
            }
        }
        Out.Append(PassportUtf8(CanonicalNumber));
        return true;
    }
    case EPassportJsonType::Array:
        Out.Add(static_cast<uint8>('['));
        for (int32 Index = 0; Index < Value->Array.Num(); ++Index)
        {
            if (Index > 0)
            {
                Out.Add(static_cast<uint8>(','));
            }
            if (!AppendPythonJson(Value->Array[Index], Out, OutError))
            {
                return false;
            }
        }
        Out.Add(static_cast<uint8>(']'));
        return true;
    case EPassportJsonType::Object:
    {
        struct FEntry
        {
            FString Key;
            TArray<uint8> KeyBytes;
            TSharedPtr<FPassportJsonValue> Value;
        };
        TArray<FEntry> Entries;
        TArray<FString> NormalizedKeys;
        for (const FPassportJsonMember& Member : Value->Object)
        {
            FString Key;
            if (!NormalizeNfc(Member.Key, Key, OutError))
            {
                return false;
            }
            if (NormalizedKeys.ContainsByPredicate([&Key](const FString& Existing)
                {
                    return Existing.Equals(Key, ESearchCase::CaseSensitive);
                }))
            {
                return Fail(OutError, TEXT("keys collide after Unicode NFC"));
            }
            NormalizedKeys.Add(Key);
            FEntry& Entry = Entries.AddDefaulted_GetRef();
            Entry.Key = MoveTemp(Key);
            Entry.KeyBytes = PassportUtf8(Entry.Key);
            Entry.Value = Member.Value;
        }
        Entries.Sort([](const FEntry& A, const FEntry& B)
        {
            return CompareBytes(A.KeyBytes, B.KeyBytes) < 0;
        });
        Out.Add(static_cast<uint8>('{'));
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            if (Index > 0)
            {
                Out.Add(static_cast<uint8>(','));
            }
            if (!AppendPythonString(Entries[Index].Key, Out, OutError))
            {
                return false;
            }
            Out.Add(static_cast<uint8>(':'));
            if (!AppendPythonJson(Entries[Index].Value, Out, OutError))
            {
                return false;
            }
        }
        Out.Add(static_cast<uint8>('}'));
        return true;
    }
    default:
        return Fail(OutError, TEXT("unsupported JSON value"));
}
}

bool SerializePythonJson(
    const TSharedPtr<FPassportJsonValue>& Value,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    OutBytes.Reset();
    return AppendPythonJson(Value, OutBytes, OutError);
}

const TSharedPtr<FPassportJsonValue>* FindExactField(
    const TSharedPtr<FPassportJsonValue>& Object,
    const TCHAR* Field)
{
    if (!Object.IsValid() || Object->Type != EPassportJsonType::Object)
    {
        return nullptr;
    }
    for (const FPassportJsonMember& Member : Object->Object)
    {
        if (Member.Key.Equals(Field, ESearchCase::CaseSensitive))
        {
            return &Member.Value;
        }
    }
    return nullptr;
}

bool TryGetStringField(
    const TSharedPtr<FPassportJsonValue>& Object,
    const TCHAR* Field,
    FString& OutString)
{
    const TSharedPtr<FPassportJsonValue>* Value = FindExactField(Object, Field);
    if (Value == nullptr || !Value->IsValid() || (*Value)->Type != EPassportJsonType::String)
    {
        return false;
    }
    OutString = (*Value)->Text;
    return true;
}

bool TryGetInteger(const TSharedPtr<FPassportJsonValue>& Value, int64& OutInteger)
{
    return Value.IsValid() && Value->Type == EPassportJsonType::Number &&
        IsIntegerToken(Value->Text) && Value->Text != TEXT("-0") &&
        LexTryParseString(OutInteger, *Value->Text);
}

bool IsHashString(const FString& Value)
{
    if (Value.Len() != 21 || !Value.StartsWith(TEXT("xxh3:"), ESearchCase::CaseSensitive))
    {
        return false;
    }
    for (int32 Index = 5; Index < Value.Len(); ++Index)
    {
        const TCHAR Char = Value[Index];
        if (!((Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f'))))
        {
            return false;
        }
    }
    return true;
}

bool ValidatePassportObject(
    const TSharedPtr<FPassportJsonValue>& Root,
    FMHFbxPassport& OutPassport,
    FString& OutError)
{
    static const TCHAR* RequiredFields[] = {
        TEXT("schema"), TEXT("schema_version"), TEXT("resource_uid"), TEXT("kind"),
        TEXT("name"), TEXT("lod_levels"), TEXT("lod_policy"), TEXT("geometry_hash"),
        TEXT("material_slots"), TEXT("properties"), TEXT("exporter")};
    if (!Root.IsValid() || Root->Type != EPassportJsonType::Object ||
        Root->Object.Num() != UE_ARRAY_COUNT(RequiredFields))
    {
        return Fail(OutError, TEXT("passport fields differ from mh.fbx_passport v1"));
    }
    for (const TCHAR* Field : RequiredFields)
    {
        if (FindExactField(Root, Field) == nullptr)
        {
            return Fail(OutError, FString::Printf(TEXT("missing passport field %s"), Field));
        }
    }

    FString Schema;
    if (!TryGetStringField(Root, TEXT("schema"), Schema) ||
        !Schema.Equals(TEXT("mh.fbx_passport"), ESearchCase::CaseSensitive))
    {
        return Fail(OutError, TEXT("unknown schema"));
    }
    int64 Version = 0;
    if (!TryGetInteger(*FindExactField(Root, TEXT("schema_version")), Version) || Version != 1)
    {
        return Fail(OutError, TEXT("unsupported schema_version"));
    }

    FString ResourceUid;
    if (!TryGetStringField(Root, TEXT("resource_uid"), ResourceUid) || !MHIsCanonicalUuid(ResourceUid))
    {
        return Fail(OutError, TEXT("resource_uid must be a canonical lowercase UUID"));
    }
    FString Kind;
    if (!TryGetStringField(Root, TEXT("kind"), Kind) ||
        !Kind.Equals(TEXT("static_mesh"), ESearchCase::CaseSensitive))
    {
        return Fail(OutError, TEXT("kind must be static_mesh"));
    }
    FString Name;
    if (!TryGetStringField(Root, TEXT("name"), Name) || !MHIsValidResourceName(Name))
    {
        return Fail(OutError, TEXT("name must match [A-Za-z0-9_ -]"));
    }

    const TSharedPtr<FPassportJsonValue> LevelsValue = *FindExactField(Root, TEXT("lod_levels"));
    if (!LevelsValue.IsValid() || LevelsValue->Type != EPassportJsonType::Array || LevelsValue->Array.IsEmpty())
    {
        return Fail(OutError, TEXT("lod_levels must be a non-empty array"));
    }
    TArray<int32> Levels;
    for (const TSharedPtr<FPassportJsonValue>& LevelValue : LevelsValue->Array)
    {
        int64 Level = -1;
        if (!TryGetInteger(LevelValue, Level) || Level != Levels.Num())
        {
            return Fail(OutError, TEXT("lod_levels must be contiguous [0..N]"));
        }
        Levels.Add(static_cast<int32>(Level));
    }
    FString LodPolicy;
    if (!TryGetStringField(Root, TEXT("lod_policy"), LodPolicy) ||
        (!LodPolicy.Equals(TEXT("authored"), ESearchCase::CaseSensitive) &&
         !LodPolicy.Equals(TEXT("generated"), ESearchCase::CaseSensitive) &&
         !LodPolicy.Equals(TEXT("nanite"), ESearchCase::CaseSensitive)))
    {
        return Fail(OutError, TEXT("lod_policy must be authored, generated or nanite"));
    }
    if (Levels.Num() > 1 && !LodPolicy.Equals(TEXT("authored"), ESearchCase::CaseSensitive))
    {
        return Fail(OutError, TEXT("multiple lod_levels require authored lod_policy"));
    }
    FString GeometryHash;
    if (!TryGetStringField(Root, TEXT("geometry_hash"), GeometryHash) || !IsHashString(GeometryHash))
    {
        return Fail(OutError, TEXT("geometry_hash must be xxh3 plus 16 lowercase hex digits"));
    }

    const TSharedPtr<FPassportJsonValue> SlotsValue = *FindExactField(Root, TEXT("material_slots"));
    if (!SlotsValue.IsValid() || SlotsValue->Type != EPassportJsonType::Array)
    {
        return Fail(OutError, TEXT("material_slots must be an array"));
    }
    TArray<FMHFbxPassportSlot> ParsedSlots;
    TArray<FString> SlotNames;
    TArray<uint8> PreviousSlotName;
    for (const TSharedPtr<FPassportJsonValue>& SlotValue : SlotsValue->Array)
    {
        FMHFbxPassportSlot Slot;
        if (!SlotValue.IsValid() || SlotValue->Type != EPassportJsonType::Object ||
            SlotValue->Object.Num() != 3 ||
            !TryGetStringField(SlotValue, TEXT("slot_name"), Slot.SlotName) || Slot.SlotName.IsEmpty() ||
            !TryGetStringField(SlotValue, TEXT("material_uid"), Slot.MaterialUid) ||
            !MHIsCanonicalUuid(Slot.MaterialUid) ||
            !TryGetStringField(SlotValue, TEXT("material_name_hint"), Slot.MaterialNameHint))
        {
            return Fail(OutError, TEXT("material_slots entry has invalid fields"));
        }
        if (SlotNames.ContainsByPredicate([&Slot](const FString& Existing)
            {
                return Existing.Equals(Slot.SlotName, ESearchCase::CaseSensitive);
            }))
        {
            return Fail(OutError, FString::Printf(TEXT("duplicate material slot_name %s"), *Slot.SlotName));
        }
        const TArray<uint8> SlotNameBytes = PassportUtf8(Slot.SlotName);
        if (!ParsedSlots.IsEmpty() && CompareBytes(PreviousSlotName, SlotNameBytes) > 0)
        {
            return Fail(OutError, TEXT("material_slots must be sorted by slot_name"));
        }
        PreviousSlotName = SlotNameBytes;
        SlotNames.Add(Slot.SlotName);
        ParsedSlots.Add(MoveTemp(Slot));
    }

    const TSharedPtr<FPassportJsonValue> Properties = *FindExactField(Root, TEXT("properties"));
    if (!Properties.IsValid() || Properties->Type != EPassportJsonType::Object)
    {
        return Fail(OutError, TEXT("properties must be an object"));
    }
    TArray<uint8> PropertiesBytes;
    if (!SerializePythonJson(Properties, PropertiesBytes, OutError))
    {
        return false;
    }
    FString Exporter;
    if (!TryGetStringField(Root, TEXT("exporter"), Exporter) || Exporter.IsEmpty())
    {
        return Fail(OutError, TEXT("exporter must be a non-empty string"));
    }

    OutPassport.ResourceUid = MoveTemp(ResourceUid);
    OutPassport.Name = MoveTemp(Name);
    OutPassport.LodPolicy = MoveTemp(LodPolicy);
    OutPassport.LodLevels = MoveTemp(Levels);
    OutPassport.GeometryHash = MoveTemp(GeometryHash);
    OutPassport.MaterialSlots = MoveTemp(ParsedSlots);
    OutPassport.PropertiesJson = PassportUtf8Text(PropertiesBytes);
    OutPassport.Exporter = MoveTemp(Exporter);
    return true;
}

void AddMember(
    TArray<FPassportJsonMember>& Members,
    const TCHAR* Key,
    TSharedPtr<FPassportJsonValue> Value)
{
    FPassportJsonMember& Member = Members.AddDefaulted_GetRef();
    Member.Key = Key;
    Member.Value = MoveTemp(Value);
}

TSharedPtr<FPassportJsonValue> BuildPassportObject(
    const FMHFbxPassport& Passport,
    FString& OutError)
{
    TSharedPtr<FPassportJsonValue> Properties;
    const FString PropertiesJson = Passport.PropertiesJson.IsEmpty() ? TEXT("{}") : Passport.PropertiesJson;
    FPassportJsonParser PropertiesParser(PropertiesJson, OutError);
    if (!PropertiesParser.Parse(Properties) || !Properties.IsValid() || Properties->Type != EPassportJsonType::Object)
    {
        if (OutError.IsEmpty())
        {
            Fail(OutError, TEXT("properties cannot be parsed as an object"));
        }
        return nullptr;
    }
    TArray<uint8> CanonicalProperties;
    if (!SerializePythonJson(Properties, CanonicalProperties, OutError) ||
        PassportUtf8(PropertiesJson) != CanonicalProperties)
    {
        if (OutError.IsEmpty())
        {
            Fail(OutError, TEXT("properties is not canonical Python JSON"));
        }
        return nullptr;
    }

    TArray<FPassportJsonMember> Members;
    AddMember(Members, TEXT("schema"), PassportString(TEXT("mh.fbx_passport")));
    AddMember(Members, TEXT("schema_version"), PassportNumber(TEXT("1")));
    AddMember(Members, TEXT("resource_uid"), PassportString(Passport.ResourceUid));
    AddMember(Members, TEXT("kind"), PassportString(TEXT("static_mesh")));
    AddMember(Members, TEXT("name"), PassportString(Passport.Name));
    AddMember(Members, TEXT("lod_policy"), PassportString(Passport.LodPolicy));
    AddMember(Members, TEXT("geometry_hash"), PassportString(Passport.GeometryHash));

    TArray<TSharedPtr<FPassportJsonValue>> Levels;
    for (const int32 Level : Passport.LodLevels)
    {
        Levels.Add(PassportNumber(LexToString(Level)));
    }
    AddMember(Members, TEXT("lod_levels"), PassportArray(MoveTemp(Levels)));

    TArray<TSharedPtr<FPassportJsonValue>> Slots;
    for (const FMHFbxPassportSlot& Slot : Passport.MaterialSlots)
    {
        TArray<FPassportJsonMember> SlotMembers;
        AddMember(SlotMembers, TEXT("slot_name"), PassportString(Slot.SlotName));
        AddMember(SlotMembers, TEXT("material_uid"), PassportString(Slot.MaterialUid));
        AddMember(SlotMembers, TEXT("material_name_hint"), PassportString(Slot.MaterialNameHint));
        Slots.Add(PassportObject(MoveTemp(SlotMembers)));
    }
    AddMember(Members, TEXT("material_slots"), PassportArray(MoveTemp(Slots)));
    AddMember(Members, TEXT("properties"), MoveTemp(Properties));
    AddMember(Members, TEXT("exporter"), PassportString(Passport.Exporter));
    return PassportObject(MoveTemp(Members));
}

} // namespace

bool MHParseFbxPassportText(
    const FString& CarrierText,
    FMHFbxPassport& OutPassport,
    FString& OutError)
{
    OutPassport = FMHFbxPassport();
    OutError.Reset();

    TSharedPtr<FPassportJsonValue> Root;
    FPassportJsonParser Parser(CarrierText, OutError);
    if (!Parser.Parse(Root) || !Root.IsValid() || Root->Type != EPassportJsonType::Object)
    {
        if (OutError.IsEmpty())
        {
            return Fail(OutError, TEXT("carrier is not a valid JSON object"));
        }
        return false;
    }
    TArray<uint8> CanonicalBytes;
    if (!SerializePythonJson(Root, CanonicalBytes, OutError))
    {
        return false;
    }
    if (PassportUtf8(CarrierText) != CanonicalBytes)
    {
        return Fail(OutError, TEXT("carrier value is not canonical one-line JSON"));
    }
    if (!ValidatePassportObject(Root, OutPassport, OutError))
    {
        OutPassport = FMHFbxPassport();
        return false;
    }
    OutPassport.CarrierText = CarrierText;
    return true;
}

bool MHBuildFbxPassportDescriptorBytes(
    const FMHFbxPassport& Passport,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    OutBytes.Reset();
    OutError.Reset();
    const TSharedPtr<FPassportJsonValue> Root = BuildPassportObject(Passport, OutError);
    if (!Root.IsValid())
    {
        return false;
    }
    FMHFbxPassport Checked;
    if (!ValidatePassportObject(Root, Checked, OutError))
    {
        return false;
    }
    Root->Object.RemoveAll([](const FPassportJsonMember& Member)
    {
        return Member.Key.Equals(TEXT("geometry_hash"), ESearchCase::CaseSensitive);
    });
    return SerializePythonJson(Root, OutBytes, OutError);
}

bool MHReadFbxPassport(
    const FString& FilePath,
    FMHFbxPassport& OutPassport,
    FString& OutError)
{
    OutPassport = FMHFbxPassport();
    OutError.Reset();

    FbxManager* Manager = FbxManager::Create();
    if (Manager == nullptr)
    {
        return Fail(OutError, TEXT("unable to create FBX manager"));
    }
    FbxIOSettings* IoSettings = FbxIOSettings::Create(Manager, IOSROOT);
    Manager->SetIOSettings(IoSettings);
    FbxImporter* Importer = FbxImporter::Create(Manager, "MHFbxPassportImporter");
    if (!Importer->Initialize(TCHAR_TO_UTF8(*FilePath), -1, IoSettings))
    {
        const FString Message = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return Fail(OutError, FString::Printf(TEXT("cannot open %s: %s"), *FilePath, *Message));
    }
    FbxScene* Scene = FbxScene::Create(Manager, "MHFbxPassportScene");
    if (Scene == nullptr || !Importer->Import(Scene))
    {
        const FString Message = Importer != nullptr
            ? FString(UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString()))
            : FString();
        Manager->Destroy();
        return Fail(OutError, FString::Printf(TEXT("cannot import %s: %s"), *FilePath, *Message));
    }
    Importer->Destroy();

    TArray<FString> CarrierTexts;
    const int32 NodeCount = Scene->GetNodeCount();
    for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
    {
        FbxNode* Node = Scene->GetNode(NodeIndex);
        if (Node == nullptr || Node->GetMesh() == nullptr)
        {
            continue;
        }
        int32 CarriersOnNode = 0;
        FString CarrierText;
        for (FbxProperty Property = Node->GetFirstProperty(); Property.IsValid();
             Property = Node->GetNextProperty(Property))
        {
            if (!FString(UTF8_TO_TCHAR(Property.GetNameAsCStr())).Equals(
                TEXT("mh_fbx_passport"), ESearchCase::CaseSensitive))
            {
                continue;
            }
            ++CarriersOnNode;
            if (!Property.GetFlag(FbxPropertyFlags::eUserDefined) ||
                Property.GetPropertyDataType().GetType() != eFbxString)
            {
                Manager->Destroy();
                return Fail(OutError, FString::Printf(
                    TEXT("MESH Model %s has a passport property that is not a user-defined string"),
                    UTF8_TO_TCHAR(Node->GetName())));
            }
            const FbxString Value = Property.Get<FbxString>();
            const ANSICHAR* RawValue = Value.Buffer();
            const int32 RawLength = Value.GetLen();
            FUTF8ToTCHAR Converted(RawValue, RawLength);
            CarrierText = FString(Converted.Length(), Converted.Get());
            const TArray<uint8> RoundTrip = PassportUtf8(CarrierText);
            if (RoundTrip.Num() != RawLength ||
                (RawLength > 0 && FMemory::Memcmp(RoundTrip.GetData(), RawValue, RawLength) != 0))
            {
                Manager->Destroy();
                return Fail(OutError, FString::Printf(
                    TEXT("MESH Model %s passport is not UTF-8"),
                    UTF8_TO_TCHAR(Node->GetName())));
            }
        }
        if (CarriersOnNode != 1)
        {
            const FString NodeName = UTF8_TO_TCHAR(Node->GetName());
            Manager->Destroy();
            return Fail(OutError, FString::Printf(
                TEXT("MESH Model %s must contain exactly one mh_fbx_passport; found %d"),
                *NodeName, CarriersOnNode));
        }
        CarrierTexts.Add(MoveTemp(CarrierText));
    }
    Manager->Destroy();

    if (CarrierTexts.IsEmpty())
    {
        return Fail(OutError, TEXT("FBX has no MESH Models"));
    }
    for (int32 Index = 1; Index < CarrierTexts.Num(); ++Index)
    {
        if (!CarrierTexts[Index].Equals(CarrierTexts[0], ESearchCase::CaseSensitive))
        {
            return Fail(OutError, TEXT("Carrier B copies differ byte-for-byte across MESH Models"));
        }
    }
    if (!MHParseFbxPassportText(CarrierTexts[0], OutPassport, OutError))
    {
        return false;
    }
    OutPassport.CopyCount = CarrierTexts.Num();
    return true;
}

} // namespace UE::MimirComposite
