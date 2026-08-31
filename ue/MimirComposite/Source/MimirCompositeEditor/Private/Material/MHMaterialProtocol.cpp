#include "Material/MHMaterialProtocol.h"

#include "Canonical/MHCanonical.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"

#include <charconv>

namespace UE::MimirComposite
{
namespace
{

bool GrammarError(FString& OutError, const FString& Detail)
{
    OutError = FString::Printf(TEXT("MH_E_MATERIAL_GRAMMAR: %s"), *Detail);
    return false;
}

bool IsTextureKey(const FString& Key, int32& OutSlot)
{
    if (!Key.StartsWith(TEXT("tex"), ESearchCase::CaseSensitive))
    {
        return false;
    }
    const FString Digits = Key.Mid(3);
    if (Digits.IsEmpty() || (Digits.Len() > 1 && Digits[0] == TEXT('0')))
    {
        return false;
    }
    for (const TCHAR Character : Digits)
    {
        if (Character < TEXT('0') || Character > TEXT('9'))
        {
            return false;
        }
    }
    return LexTryParseString(OutSlot, *Digits) && OutSlot >= 0 && OutSlot <= 15;
}

bool ReadFloat(const TSharedPtr<FJsonValue>& Value, float& OutValue)
{
    if (!Value.IsValid() || Value->Type != EJson::Number)
    {
        return false;
    }
    const double Number = Value->AsNumber();
    OutValue = static_cast<float>(Number);
    return FMath::IsFinite(Number) && FMath::IsFinite(OutValue);
}

void AppendNumber(const float Value, FString& Out)
{
    ANSICHAR Buffer[128];
    std::to_chars_result Result;
    if (Value == 0.0f)
    {
        Out.AppendChar(TEXT('0'));
        return;
    }
    Result = std::to_chars(Buffer, Buffer + UE_ARRAY_COUNT(Buffer), Value);
    check(Result.ec == std::errc());
    const FUTF8ToTCHAR Converted(Buffer, static_cast<int32>(Result.ptr - Buffer));
    Out.AppendChars(Converted.Get(), Converted.Length());
}

void AppendUtf8(const FString& Text, TArray<uint8>& OutBytes)
{
    const FTCHARToUTF8 Utf8(*Text, Text.Len());
    OutBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

void AppendJsonString(const FString& Value, FString& Out)
{
    Out.AppendChar(TEXT('"'));
    for (const TCHAR Character : Value)
    {
        switch (Character)
        {
        case TEXT('"'): Out += TEXT("\\\""); break;
        case TEXT('\\'): Out += TEXT("\\\\"); break;
        case TEXT('\b'): Out += TEXT("\\b"); break;
        case TEXT('\f'): Out += TEXT("\\f"); break;
        case TEXT('\n'): Out += TEXT("\\n"); break;
        case TEXT('\r'): Out += TEXT("\\r"); break;
        case TEXT('\t'): Out += TEXT("\\t"); break;
        default:
            if (Character < TEXT(' '))
            {
                Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character));
            }
            else
            {
                Out.AppendChar(Character);
            }
            break;
        }
    }
    Out.AppendChar(TEXT('"'));
}

} // namespace

bool MHIsCanonicalMaterialToken(const FString& Value)
{
    if (Value.IsEmpty())
    {
        return false;
    }
    for (const TCHAR Character : Value)
    {
        if (!((Character >= TEXT('a') && Character <= TEXT('z')) ||
              (Character >= TEXT('0') && Character <= TEXT('9')) ||
              Character == TEXT('_')))
        {
            return false;
        }
    }
    return true;
}

bool MHParseMaterialV4(
    const TConstArrayView<uint8> Bytes,
    FMHMaterialDocument& OutDocument,
    FString& OutError)
{
    OutDocument = FMHMaterialDocument();
    OutError.Reset();

    TSharedPtr<FJsonValue> RootValue;
    const FMHCanonicalResult ParseResult = MHParseJsonUtf8(Bytes, RootValue);
    if (!ParseResult.bSuccess || !RootValue.IsValid() || RootValue->Type != EJson::Object)
    {
        return GrammarError(OutError, TEXT("payload must be one UTF-8 JSON object"));
    }
    const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
    const bool bHasClass = Root->HasField(TEXT("class"));
    const bool bHasLibrary = Root->HasField(TEXT("library"));
    if (bHasClass == bHasLibrary)
    {
        return GrammarError(OutError, TEXT("exactly one of class or library is required"));
    }

    if (bHasLibrary)
    {
        if (Root->Values.Num() != 1 || !Root->TryGetStringField(TEXT("library"), OutDocument.Parent) ||
            !MHIsCanonicalMaterialToken(OutDocument.Parent))
        {
            return GrammarError(OutError, TEXT("library form must contain exactly one canonical library field"));
        }
        OutDocument.Mode = EMHMaterialMode::Library;
        return true;
    }

    static const TSet<FString> AllowedFields = {
        TEXT("class"), TEXT("twosided"), TEXT("textures"), TEXT("params")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
    {
        if (!AllowedFields.Contains(Pair.Key))
        {
            return GrammarError(OutError, FString::Printf(TEXT("unknown field '%s'"), *Pair.Key));
        }
    }
    if (!Root->TryGetStringField(TEXT("class"), OutDocument.Parent) ||
        !MHIsCanonicalMaterialToken(OutDocument.Parent))
    {
        return GrammarError(OutError, TEXT("class must be a canonical registry token"));
    }
    OutDocument.Mode = EMHMaterialMode::Class;

    if (const TSharedPtr<FJsonValue>* TwoSided = Root->Values.Find(TEXT("twosided")))
    {
        if (!TwoSided->IsValid() || (*TwoSided)->Type != EJson::Boolean)
        {
            return GrammarError(OutError, TEXT("twosided must be boolean"));
        }
        OutDocument.bHasTwoSided = true;
        OutDocument.bTwoSided = (*TwoSided)->AsBool();
    }

    if (const TSharedPtr<FJsonValue>* TexturesValue = Root->Values.Find(TEXT("textures")))
    {
        if (!TexturesValue->IsValid() || (*TexturesValue)->Type != EJson::Object)
        {
            return GrammarError(OutError, TEXT("textures must be an object"));
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*TexturesValue)->AsObject()->Values)
        {
            int32 Slot = INDEX_NONE;
            FString Token;
            if (!IsTextureKey(Pair.Key, Slot))
            {
                return GrammarError(OutError, FString::Printf(TEXT("invalid texture slot '%s'"), *Pair.Key));
            }
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Token))
            {
                return GrammarError(OutError, FString::Printf(TEXT("texture '%s' must be a string"), *Pair.Key));
            }
            if (!MHIsCanonicalMaterialToken(Token))
            {
                OutError = FString::Printf(
                    TEXT("MH_E_NONCANONICAL_TEXTURE_REFERENCE: texture '%s' must be an extensionless [a-z0-9_]+ token"),
                    *Pair.Key);
                return false;
            }
            OutDocument.Textures.Add(Slot, MoveTemp(Token));
        }
    }

    if (const TSharedPtr<FJsonValue>* ParamsValue = Root->Values.Find(TEXT("params")))
    {
        if (!ParamsValue->IsValid() || (*ParamsValue)->Type != EJson::Object)
        {
            return GrammarError(OutError, TEXT("params must be an object"));
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ParamsValue)->AsObject()->Values)
        {
            if (!MHIsCanonicalMaterialToken(Pair.Key))
            {
                return GrammarError(OutError, FString::Printf(TEXT("invalid parameter key '%s'"), *Pair.Key));
            }
            FMHMaterialParameter Parameter;
            if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
            {
                Parameter.bString = true;
                Parameter.String = Pair.Value->AsString();
            }
            else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Boolean)
            {
                Parameter.bBool = true;
                Parameter.Bool = Pair.Value->AsBool();
            }
            else if (ReadFloat(Pair.Value, Parameter.Scalar))
            {
                Parameter.bVector = false;
            }
            else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array && Pair.Value->AsArray().Num() == 4)
            {
                Parameter.bVector = true;
                for (int32 Index = 0; Index < 4; ++Index)
                {
                    if (!ReadFloat(Pair.Value->AsArray()[Index], Parameter.Vector[Index]))
                    {
                        return GrammarError(OutError, FString::Printf(TEXT("parameter '%s' vector components must be finite numbers"), *Pair.Key));
                    }
                }
            }
            else
            {
                return GrammarError(OutError, FString::Printf(TEXT("parameter '%s' must be a string, boolean, number, or four-number array"), *Pair.Key));
            }
            OutDocument.Params.Add(Pair.Key, Parameter);
        }
    }
    return true;
}

bool MHWriteCanonicalMaterialV4(
    const FMHMaterialDocument& Document,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    OutBytes.Reset();
    OutError.Reset();
    if (!MHIsCanonicalMaterialToken(Document.Parent))
    {
        return GrammarError(OutError, TEXT("parent registry token is not canonical"));
    }
    if (Document.Mode == EMHMaterialMode::Library &&
        (Document.bHasTwoSided || !Document.Textures.IsEmpty() || !Document.Params.IsEmpty()))
    {
        OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: library form cannot contain overrides");
        return false;
    }

    FString Text = TEXT("{\n");
    Text += Document.Mode == EMHMaterialMode::Class
        ? FString::Printf(TEXT("  \"class\": \"%s\""), *Document.Parent)
        : FString::Printf(TEXT("  \"library\": \"%s\""), *Document.Parent);

    if (Document.Mode == EMHMaterialMode::Class && Document.bHasTwoSided)
    {
        Text += FString::Printf(TEXT(",\n  \"twosided\": %s"), Document.bTwoSided ? TEXT("true") : TEXT("false"));
    }
    if (Document.Mode == EMHMaterialMode::Class && !Document.Textures.IsEmpty())
    {
        Text += TEXT(",\n  \"textures\": {\n");
        TArray<int32> Slots;
        Document.Textures.GenerateKeyArray(Slots);
        Slots.Sort();
        for (int32 Index = 0; Index < Slots.Num(); ++Index)
        {
            const int32 Slot = Slots[Index];
            const FString* Token = Document.Textures.Find(Slot);
            if (Slot < 0 || Slot > 15 || Token == nullptr || !MHIsCanonicalMaterialToken(*Token))
            {
                OutError = TEXT("MH_E_NONCANONICAL_TEXTURE_REFERENCE: writer received an invalid texture slot or token");
                return false;
            }
            Text += FString::Printf(TEXT("    \"tex%d\": \"%s\"%s\n"), Slot, **Token, Index + 1 < Slots.Num() ? TEXT(",") : TEXT(""));
        }
        Text += TEXT("  }");
    }
    if (Document.Mode == EMHMaterialMode::Class && !Document.Params.IsEmpty())
    {
        Text += TEXT(",\n  \"params\": {\n");
        TArray<FString> Names;
        Document.Params.GenerateKeyArray(Names);
        Names.Sort();
        for (int32 Index = 0; Index < Names.Num(); ++Index)
        {
            const FString& Name = Names[Index];
            const FMHMaterialParameter* Parameter = Document.Params.Find(Name);
            if (!MHIsCanonicalMaterialToken(Name) || Parameter == nullptr)
            {
                return GrammarError(OutError, TEXT("writer received an invalid parameter name"));
            }
            Text += FString::Printf(TEXT("    \"%s\": "), *Name);
            if (Parameter->bString)
            {
                if (Parameter->bBool || Parameter->bVector)
                {
                    return GrammarError(OutError, TEXT("writer received a cross-typed provenance parameter"));
                }
                AppendJsonString(Parameter->String, Text);
            }
            else if (Parameter->bBool)
            {
                if (Parameter->bVector)
                {
                    return GrammarError(OutError, TEXT("writer received a cross-typed boolean/vector parameter"));
                }
                Text += Parameter->Bool ? TEXT("true") : TEXT("false");
            }
            else if (Parameter->bVector)
            {
                Text += TEXT("[\n");
                for (int32 Component = 0; Component < 4; ++Component)
                {
                    if (!FMath::IsFinite(Parameter->Vector[Component]))
                    {
                        return GrammarError(OutError, TEXT("writer received a non-finite vector"));
                    }
                    Text += TEXT("      ");
                    AppendNumber(Parameter->Vector[Component], Text);
                    Text += Component < 3 ? TEXT(",\n") : TEXT("\n");
                }
                Text += TEXT("    ]");
            }
            else
            {
                if (!FMath::IsFinite(Parameter->Scalar))
                {
                    return GrammarError(OutError, TEXT("writer received a non-finite scalar"));
                }
                AppendNumber(Parameter->Scalar, Text);
            }
            Text += Index + 1 < Names.Num() ? TEXT(",\n") : TEXT("\n");
        }
        Text += TEXT("  }");
    }
    Text += TEXT("\n}\n");
    AppendUtf8(Text, OutBytes);
    return true;
}

} // namespace UE::MimirComposite
